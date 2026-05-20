/*
 * tests/calc.c — integer arithmetic evaluator.
 *
 *   calc EXPR...
 *
 * All argv tokens are concatenated with spaces and parsed as a single
 * expression. Grammar (recursive descent, left-associative):
 *
 *     expr   = term (('+' | '-') term)*
 *     term   = unary (('*' | '/' | '%') unary)*
 *     unary  = ('-' | '+') unary | primary
 *     primary= number | '(' expr ')'
 *
 * Numbers are signed 64-bit integers. Whitespace is skipped. Errors
 * are reported on stderr; result on stdout.
 *
 * Examples:
 *   calc 2 + 3 * 4              → 14
 *   calc "(7 - 3) * (2 + 8)"    → 40
 *   calc -- -5 + 3              → -2     (-- ends arg parsing)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single-pass: keep the source pointer in a global so each helper can
 * advance it. */
static const char *src;
static int         had_error;

static void die(const char *msg) {
    if (!had_error) fprintf(stderr, "calc: %s\n", msg);
    had_error = 1;
}

static void skip_ws(void) {
    while (*src == ' ' || *src == '\t') src++;
}

static long parse_expr(void);

static long parse_primary(void) {
    skip_ws();
    if (*src == '(') {
        src++;
        long v = parse_expr();
        skip_ws();
        if (*src != ')') { die("missing )"); return 0; }
        src++;
        return v;
    }
    if (!isdigit(*src)) {
        die("expected number");
        return 0;
    }
    long v = 0;
    while (isdigit(*src)) { v = v * 10 + (*src - '0'); src++; }
    return v;
}

static long parse_unary(void) {
    skip_ws();
    if (*src == '-') { src++; return -parse_unary(); }
    if (*src == '+') { src++; return  parse_unary(); }
    return parse_primary();
}

static long parse_term(void) {
    long v = parse_unary();
    for (;;) {
        skip_ws();
        char op = *src;
        if (op != '*' && op != '/' && op != '%') return v;
        src++;
        long r = parse_unary();
        if ((op == '/' || op == '%') && r == 0) { die("divide by zero"); return 0; }
        if      (op == '*') v *= r;
        else if (op == '/') v /= r;
        else                v %= r;
    }
}

static long parse_expr(void) {
    long v = parse_term();
    for (;;) {
        skip_ws();
        char op = *src;
        if (op != '+' && op != '-') return v;
        src++;
        long r = parse_term();
        v = (op == '+') ? v + r : v - r;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: calc EXPR...\n");
        return 1;
    }

    /* Concatenate argv[1..] with single spaces. */
    char buf[256];
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (off + n + 1 >= sizeof(buf)) { fprintf(stderr, "calc: too long\n"); return 1; }
        if (i > 1) buf[off++] = ' ';
        for (size_t k = 0; k < n; k++) buf[off++] = argv[i][k];
    }
    buf[off] = 0;

    src       = buf;
    had_error = 0;
    long v = parse_expr();
    skip_ws();
    if (*src != 0 && !had_error) die("trailing garbage");

    if (had_error) return 1;
    printf("%ld\n", v);
    return 0;
}
