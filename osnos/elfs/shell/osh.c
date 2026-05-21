/*
 * tests/osh.c — osnos mini script interpreter.
 *
 * Usage:
 *   osh FILE.osh         run script from file
 *   osh -e "code"        run inline code
 *   osh                  print usage + language reference
 *
 * Language (single-pass recursive-descent eval — no AST):
 *
 *   stmt        := assign | print | if | while | block
 *   assign      := IDENT '=' expr
 *   print       := 'print' (STRING | expr)*
 *   if          := 'if' expr block ('else' block)?
 *   while       := 'while' expr block
 *   block       := '{' stmt* '}'
 *   expr        := or
 *   or          := and ('||' and)*
 *   and         := cmp ('&&' cmp)*
 *   cmp         := add (('=='|'!='|'<'|'>'|'<='|'>=') add)?
 *   add         := mul (('+'|'-') mul)*
 *   mul         := unary (('*'|'/'|'%') unary)*
 *   unary       := ('-'|'!')? primary
 *   primary     := NUMBER | IDENT | '(' expr ')'
 *
 * - Integers are signed 64-bit.
 * - `true` and `false` evaluate to 1 and 0.
 * - Variables are global, max 32, names up to 15 chars.
 * - Strings are double-quoted, no escapes.
 * - Statement separator: newline or ';'. Comments: '#' to end of line.
 *
 * Skipping (`if 0` / `while 0` branches): the parser walks every token
 * but disables side effects via `exec_enabled`. Loops re-parse their
 * body by saving the source pointer.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* Lexer state and tiny utility                                      */
/* ---------------------------------------------------------------- */

static const char *src;
static int         had_error;
static int         exec_enabled = 1;

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

static void error(const char *msg) {
    if (!had_error) fprintf(stderr, "osh: %s\n", msg);
    had_error = 1;
}

/* Skip spaces / tabs / # comments; newlines kept (they end statements). */
static void skip_blank(void) {
    for (;;) {
        while (*src == ' ' || *src == '\t' || *src == '\r') src++;
        if (*src == '#') { while (*src && *src != '\n') src++; continue; }
        break;
    }
}

/* Like skip_blank but also consume newlines (used inside expressions
 * and before/after blocks where newlines are not statement separators). */
static void skip_blank_nl(void) {
    for (;;) {
        while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
        if (*src == '#') { while (*src && *src != '\n') src++; continue; }
        break;
    }
}

/* Consume an end-of-statement marker (newline, ';', or before-block '}'). */
static void skip_stmt_term(void) {
    for (;;) {
        skip_blank();
        if (*src == '\n' || *src == ';') { src++; continue; }
        break;
    }
}

/* Try to match a literal string at the current source position. Only
 * commits (advances src) if the match succeeds. */
static int match_lit(const char *s) {
    const char *p = src;
    while (*s) { if (*p++ != *s++) return 0; }
    src = p;
    return 1;
}

/* Try to match a bare keyword (the literal followed by a non-alnum
 * char or EOF). Does not advance on miss. */
static int match_kw(const char *kw) {
    size_t i = 0;
    while (kw[i] && src[i] == kw[i]) i++;
    if (kw[i] != 0) return 0;
    if (is_alnum((unsigned char)src[i])) return 0;
    src += i;
    return 1;
}

/* ---------------------------------------------------------------- */
/* Variable table                                                    */
/* ---------------------------------------------------------------- */

#define MAX_VARS 32

typedef struct { char name[16]; long value; } var_t;
static var_t vars[MAX_VARS];
static int   nvars;

static long var_get(const char *name) {
    for (int i = 0; i < nvars; i++) {
        if (strcmp(vars[i].name, name) == 0) return vars[i].value;
    }
    fprintf(stderr, "osh: undefined variable: %s\n", name);
    had_error = 1;
    return 0;
}

static void var_set(const char *name, long value) {
    for (int i = 0; i < nvars; i++) {
        if (strcmp(vars[i].name, name) == 0) { vars[i].value = value; return; }
    }
    if (nvars >= MAX_VARS) { error("too many variables"); return; }
    strncpy(vars[nvars].name, name, sizeof(vars[nvars].name) - 1);
    vars[nvars].name[sizeof(vars[nvars].name) - 1] = 0;
    vars[nvars].value = value;
    nvars++;
}

/* ---------------------------------------------------------------- */
/* Forward decls                                                     */
/* ---------------------------------------------------------------- */

static long parse_expr(void);
static void parse_stmt(void);

/* Read an identifier into `out` (size cap). Returns 1 on success. */
static int read_ident(char *out, size_t cap) {
    skip_blank();
    if (!is_alpha((unsigned char)*src)) return 0;
    size_t i = 0;
    while (is_alnum((unsigned char)*src) && i + 1 < cap) out[i++] = *src++;
    out[i] = 0;
    return 1;
}

/* ---------------------------------------------------------------- */
/* Expression parser/eval                                            */
/* ---------------------------------------------------------------- */

static long parse_primary(void) {
    skip_blank();
    if (*src == '(') {
        src++;
        long v = parse_expr();
        skip_blank_nl();
        if (*src != ')') { error("missing )"); return 0; }
        src++;
        return v;
    }
    if (is_digit((unsigned char)*src)) {
        long v = 0;
        while (is_digit((unsigned char)*src)) { v = v * 10 + (*src - '0'); src++; }
        return v;
    }
    if (is_alpha((unsigned char)*src)) {
        char name[16];
        if (!read_ident(name, sizeof(name))) { error("ident"); return 0; }
        if (strcmp(name, "true")  == 0) return 1;
        if (strcmp(name, "false") == 0) return 0;
        return exec_enabled ? var_get(name) : 0;
    }
    error("expected expression");
    return 0;
}

static long parse_unary(void) {
    skip_blank();
    if (*src == '-') { src++; return -parse_unary(); }
    if (*src == '!') { src++; return !parse_unary(); }
    return parse_primary();
}

static long parse_mul(void) {
    long v = parse_unary();
    for (;;) {
        skip_blank();
        char op = *src;
        if (op != '*' && op != '/' && op != '%') return v;
        src++;
        long r = parse_unary();
        if (!exec_enabled) continue;
        if ((op == '/' || op == '%') && r == 0) { error("divide by zero"); return 0; }
        if      (op == '*') v *= r;
        else if (op == '/') v /= r;
        else                v %= r;
    }
}

static long parse_add(void) {
    long v = parse_mul();
    for (;;) {
        skip_blank();
        char op = *src;
        if (op != '+' && op != '-') return v;
        src++;
        long r = parse_mul();
        if (!exec_enabled) continue;
        v = (op == '+') ? v + r : v - r;
    }
}

static long parse_cmp(void) {
    long l = parse_add();
    skip_blank();
    int kind = 0;          /* 1 == ; 2 != ; 3 < ; 4 > ; 5 <= ; 6 >= */
    if      (match_lit("=="))               kind = 1;
    else if (match_lit("!="))               kind = 2;
    else if (match_lit("<="))               kind = 5;
    else if (match_lit(">="))               kind = 6;
    else if (*src == '<' && src[1] != '<') { src++; kind = 3; }
    else if (*src == '>' && src[1] != '>') { src++; kind = 4; }
    if (kind == 0) return l;
    long r = parse_add();
    if (!exec_enabled) return 0;
    switch (kind) {
        case 1: return l == r;
        case 2: return l != r;
        case 3: return l <  r;
        case 4: return l >  r;
        case 5: return l <= r;
        case 6: return l >= r;
    }
    return 0;
}

static long parse_and(void) {
    long v = parse_cmp();
    for (;;) {
        skip_blank();
        if (!match_lit("&&")) return v;
        /* Short-circuit: if v is false, evaluate the right side with
         * exec disabled so we still consume the tokens. */
        int saved = exec_enabled;
        if (exec_enabled && v == 0) exec_enabled = 0;
        long r = parse_cmp();
        exec_enabled = saved;
        if (exec_enabled) v = v && r;
    }
}

static long parse_or(void) {
    long v = parse_and();
    for (;;) {
        skip_blank();
        if (!match_lit("||")) return v;
        int saved = exec_enabled;
        if (exec_enabled && v != 0) exec_enabled = 0;
        long r = parse_and();
        exec_enabled = saved;
        if (exec_enabled) v = v || r;
    }
}

static long parse_expr(void) { return parse_or(); }

/* ---------------------------------------------------------------- */
/* Statements                                                        */
/* ---------------------------------------------------------------- */

/*
 * Parse and execute (or skip) a `{ stmts }` block. Newlines inside the
 * braces are statement separators just like at top level.
 */
static void parse_block(void) {
    skip_blank_nl();
    if (*src != '{') { error("expected '{'"); return; }
    src++;
    for (;;) {
        skip_stmt_term();
        skip_blank_nl();
        if (*src == '}') { src++; return; }
        if (*src == 0)   { error("unterminated block"); return; }
        parse_stmt();
        if (had_error) return;
    }
}

static void parse_print(void) {
    int first = 1;
    for (;;) {
        skip_blank();
        if (*src == 0 || *src == '\n' || *src == ';' || *src == '}') break;
        if (!first && exec_enabled) putchar(' ');
        if (*src == '"') {
            src++;
            while (*src && *src != '"') {
                if (exec_enabled) putchar(*src);
                src++;
            }
            if (*src == '"') src++;
            else error("unterminated string");
        } else {
            long v = parse_expr();
            if (exec_enabled) printf("%ld", v);
        }
        first = 0;
    }
    if (exec_enabled) putchar('\n');
}

static void parse_if(void) {
    long cond = parse_expr();
    int  taken = exec_enabled && cond != 0;
    int  saved = exec_enabled;

    if (!taken) exec_enabled = 0;
    parse_block();
    exec_enabled = saved;
    if (had_error) return;

    /* Optional else branch. */
    skip_blank_nl();
    if (match_kw("else")) {
        if (taken) exec_enabled = 0;
        parse_block();
        exec_enabled = saved;
    }
}

static void parse_while(void) {
    /* Save the start so we can re-parse the cond + body each iteration. */
    const char *loop_start = src;
    long iter = 0;
    const long ITER_LIMIT = 1000000;        /* prevents runaway scripts */

    for (;;) {
        src = loop_start;
        long cond = parse_expr();
        int  taken = exec_enabled && cond != 0;
        int  saved = exec_enabled;
        if (!taken) exec_enabled = 0;
        parse_block();
        exec_enabled = saved;
        if (had_error) return;
        if (!taken) return;          /* falsy: stop after one syntax-walk */
        if (++iter > ITER_LIMIT) { error("loop iteration cap reached"); return; }
    }
}

static void parse_stmt(void) {
    skip_blank();
    if (*src == 0 || *src == '\n' || *src == ';' || *src == '}') return;

    /* Keywords. */
    if (match_kw("print"))  { parse_print(); return; }
    if (match_kw("if"))     { parse_if();    return; }
    if (match_kw("while"))  { parse_while(); return; }

    /* Assignment: IDENT '=' expr. We need to look ahead to distinguish
     * an assignment from a bare expression-as-statement. The simplest
     * approach: try to read an ident, then check for '='. If not '=',
     * treat the whole thing as an expression statement (evaluated and
     * its value discarded). */
    const char *back = src;
    char name[16];
    if (read_ident(name, sizeof(name))) {
        skip_blank();
        if (*src == '=' && src[1] != '=') {
            src++;
            long v = parse_expr();
            if (exec_enabled) var_set(name, v);
            return;
        }
        /* Not an assignment — rewind and parse as expression. */
        src = back;
    }

    (void)parse_expr();  /* discard */
}

/* ---------------------------------------------------------------- */
/* Top-level                                                         */
/* ---------------------------------------------------------------- */

static void run(const char *code) {
    src          = code;
    had_error    = 0;
    exec_enabled = 1;
    nvars        = 0;

    for (;;) {
        /*
         * Drain any number of statement terminators (';', newlines)
         * plus blank/comment runs before attempting the next stmt.
         * Without this, a trailing ';' loops forever because
         * parse_stmt early-returns on ';' without advancing src.
         */
        skip_stmt_term();
        skip_blank_nl();
        if (*src == 0) break;
        parse_stmt();
        if (had_error) break;
    }
}

/* ---------------------------------------------------------------- */
/* File loading                                                      */
/* ---------------------------------------------------------------- */

/* Read up to (cap-1) bytes from `path` into `buf`. NUL-terminates.
 * Returns 0 on success, -1 on error. */
static int slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t total = 0;
    for (;;) {
        if (total >= cap - 1) break;
        long n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = 0;
    close(fd);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Usage + entry point                                               */
/* ---------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  osh FILE.osh         run script from file\n"
        "  osh -e \"code\"        run inline code\n"
        "\n"
        "language:\n"
        "  x = 5                  variables (int64)\n"
        "  print x + 2            print expr\n"
        "  print \"i is\" x         print string + expr\n"
        "  if x < 10 { print x } else { print -1 }\n"
        "  while x > 0 { x = x - 1 }\n"
        "  # comment\n"
        "ops: + - * / %% == != < > <= >= && || ! ( )\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) { fprintf(stderr, "osh: -e needs code\n"); return 1; }
        run(argv[2]);
    } else {
        static char buf[8192];
        if (slurp(argv[1], buf, sizeof(buf)) < 0) {
            fprintf(stderr, "osh: cannot read %s\n", argv[1]);
            return 1;
        }
        run(buf);
    }
    return had_error ? 1 : 0;
}
