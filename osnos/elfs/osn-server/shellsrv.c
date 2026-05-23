/*
 * elfs/osn-server/shellsrv.c — Shell, ring 3 (FASE 10.4).
 *
 * chunk 1: prompt + canonical read + dispatch + osn_spawn fallback.
 * chunk 2 (this file): raw-mode line editor + history + arrow keys.
 *
 * Still in "sub-shell" mode — user invokes `shellsrv` from the
 * legacy kernel shell, gets a ring-3 prompt with full line editing,
 * `exit` returns. Replacement of shell_server.c in kmain lands in
 * a later chunk.
 *
 * Design:
 *   - Disable TTY ICANON + ECHO at startup (ISIG kept so Ctrl+C
 *     still kills the spawned child). Restore on exit.
 *   - Read one byte at a time; maintain (buf, len, cursor) state.
 *   - Recognise CSI: ESC [ A/B/C/D for history-up/down and
 *     intra-line cursor motion.
 *   - Echo + redraw via VT100 (\r ESC[K + prompt + buffer + cursor
 *     back-step).
 *   - History: ring of 16 lines, dedup consecutive duplicates.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "osnos_ipc.h"
#include "osnos_taskinfo.h"

#define LINE_MAX_LEN   256
#define HISTORY_MAX    16

/* ---- safe string helpers (libc doesn't expose os_strlcpy) ---- */

static size_t safe_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

static size_t safe_cat(char *dst, const char *src, size_t cap) {
    size_t d = 0;
    while (d < cap && dst[d]) d++;
    while (d + 1 < cap && *src) { dst[d++] = *src++; }
    if (d < cap) dst[d] = 0;
    return d;
}

/* ---- termios save / restore ---- */

static struct termios saved_termios;
static int            termios_saved = 0;

static void enter_raw(void) {
    if (tcgetattr(0, &saved_termios) != 0) return;
    termios_saved = 1;
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ECHOE);   /* keep ISIG so Ctrl+C survives */
    tcsetattr(0, TCSANOW, &raw);
}

static void leave_raw(void) {
    if (termios_saved) tcsetattr(0, TCSANOW, &saved_termios);
}

/* ---- prompt + redraw ---- */

static char prompt_buf[PATH_MAX + 32];
static int  prompt_visible_len;

static void build_prompt(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == 0) safe_copy(cwd, "?", sizeof(cwd));
    /* Visible portion (no escapes) drives cursor math. */
    int v = 0;
    v += (int)safe_copy(prompt_buf, "shellsrv:", sizeof(prompt_buf));
    v += (int)safe_cat (prompt_buf, cwd,          sizeof(prompt_buf));
    v += (int)safe_cat (prompt_buf, "$ ",         sizeof(prompt_buf));
    prompt_visible_len = v;
}

static int render_first = 1;     /* set by read_line at every entry */

static void render_line(const char *buf, int len, int cursor) {
    /* Move cursor to column 0, clear to EOL, re-print prompt + line,
     * with the char under the logical cursor highlighted via SGR
     * reverse (same trick /bin/ovi uses). When cursor == len we
     * draw a reversed space as the "I-am-here" marker.
     *
     * Sequence:
     *   \r ESC[K                     — reset visual line
     *   ESC[38;2;0;255;102m PROMPT ESC[39m  — green prompt
     *   buf[0..cursor]               — plain
     *   ESC[7m                       — reverse on
     *   buf[cursor]  (or space)      — the highlighted char
     *   ESC[27m                      — reverse off
     *   buf[cursor+1..len]           — plain tail
     */
    /* First render of a new read_line cycle: emit a `%`-style EOL
     * marker (zsh-style) on the CURRENT line if the previous output
     * didn't end with \n, then drop to a fresh line. Subsequent
     * renders (after each keystroke) use \r ESC[K to redraw in
     * place. We can't observe the framebuffer cursor, so we
     * unconditionally print "\n" only on truly partial output —
     * approximated as "first render" but with no leading \r ESC[K
     * so any incomplete output stays visible. */
    if (render_first) {
        /* Don't \r — that would clobber unterminated stdout from the
         * previous command. Just print the prompt at the current
         * position (will jam against any partial output, which is
         * the standard way bash/sh show "missing newline"). */
        printf("\x1b[38;2;0;255;102m%s\x1b[39m", prompt_buf);
        render_first = 0;
    } else {
        printf("\r\x1b[K\x1b[38;2;0;255;102m%s\x1b[39m", prompt_buf);
    }
    if (cursor > 0) fwrite(buf, 1, (size_t)cursor, stdout);

    printf("\x1b[7m");
    if (cursor < len) fwrite(buf + cursor, 1, 1, stdout);
    else              fwrite(" ", 1, 1, stdout);
    printf("\x1b[27m");

    if (cursor + 1 < len) {
        fwrite(buf + cursor + 1, 1, (size_t)(len - cursor - 1), stdout);
    }
    /* Whole line is redrawn each keypress — no need to walk the FB
     * cursor back; the next render_line resets via \r ESC[K. */
    fflush(stdout);
}

/* ---- history (in-mem ring + disk-backed at /home/.history) ---- */

#define HISTORY_FILE "/home/.history"

static char history[HISTORY_MAX][LINE_MAX_LEN];
static int  history_count;
static int  history_pos;       /* index into history[]; == count means "fresh" */
static int  history_persist = 0;  /* skip disk writes while replaying the file */

static void history_push(const char *line) {
    if (line[0] == 0) return;
    if (history_count > 0 &&
        strcmp(history[history_count - 1], line) == 0) return;
    if (history_count < HISTORY_MAX) {
        safe_copy(history[history_count], line, LINE_MAX_LEN);
        history_count++;
    } else {
        for (int i = 0; i + 1 < HISTORY_MAX; i++) {
            safe_copy(history[i], history[i + 1], LINE_MAX_LEN);
        }
        safe_copy(history[HISTORY_MAX - 1], line, LINE_MAX_LEN);
    }
}

static void history_save(const char *line) {
    history_push(line);
    if (!history_persist) return;
    /* Append "<line>\n" to the on-disk file so the next boot keeps it.
     * O_APPEND makes the write race-safe with other writers (unlikely
     * but cheap to enforce). */
    int fd = open(HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    /* Build the line + newline as one buffer + single write so the
     * VFS-backed FAT layer doesn't split it across two extents. */
    char tmp[LINE_MAX_LEN + 2];
    size_t n = strlen(line);
    if (n > sizeof(tmp) - 2) n = sizeof(tmp) - 2;
    for (size_t i = 0; i < n; i++) tmp[i] = line[i];
    tmp[n]   = '\n';
    tmp[n+1] = 0;
    write(fd, tmp, n + 1);
    close(fd);
}

static void history_load(void) {
    int fd = open(HISTORY_FILE, O_RDONLY);
    if (fd < 0) { history_persist = 1; return; }
    char buf[LINE_MAX_LEN];
    int  len = 0;
    char chunk[256];
    long n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                buf[len] = 0;
                if (len > 0) history_push(buf);
                len = 0;
            } else if (len + 1 < (int)sizeof(buf)) {
                buf[len++] = c;
            }
        }
    }
    if (len > 0) { buf[len] = 0; history_push(buf); }
    close(fd);
    history_persist = 1;
}

/* ---- line editor ---- */

#define LINE_OK     0
#define LINE_EOF   -1
#define LINE_INT   -2     /* Ctrl+C / cancel */

static int read_line(char *out, size_t cap) {
    char  buf[LINE_MAX_LEN];
    int   len = 0;
    int   cur = 0;
    buf[0] = 0;
    history_pos = history_count;
    render_first = 1;     /* first render: keep partial output visible */

    render_line(buf, len, cur);

    for (;;) {
        unsigned char c;
        long n = read(0, &c, 1);
        if (n <= 0) return LINE_EOF;

        if (c == '\n' || c == '\r') {
            /* Re-render the line WITHOUT the reverse-video cursor
             * block, then drop to the next row. Single fflush at the
             * end guarantees the redraw bytes reach the kernel BEFORE
             * the "\n" — otherwise printf's stdio buffer ordering vs
             * a raw write() leaves the cursor block visible on the
             * submitted line. */
            printf("\r\x1b[K\x1b[38;2;0;255;102m%s\x1b[39m", prompt_buf);
            if (len > 0) fwrite(buf, 1, (size_t)len, stdout);
            putchar('\n');
            fflush(stdout);
            int copy_len = (len < (int)cap - 1) ? len : (int)cap - 1;
            for (int i = 0; i < copy_len; i++) out[i] = buf[i];
            out[copy_len] = 0;
            return copy_len;
        }
        if (c == 0x03) {                /* Ctrl+C */
            write(1, "^C\n", 3);
            return LINE_INT;
        }
        if (c == 0x7f || c == 0x08) {   /* DEL / backspace */
            if (cur > 0) {
                for (int i = cur - 1; i + 1 < len; i++) buf[i] = buf[i + 1];
                len--; cur--;
                buf[len] = 0;
                render_line(buf, len, cur);
            }
            continue;
        }
        if (c == 0x1B) {                /* ESC — start CSI */
            unsigned char b1, b2;
            if (read(0, &b1, 1) != 1) continue;
            if (b1 != '[') continue;
            if (read(0, &b2, 1) != 1) continue;
            if (b2 == 'A') {                       /* up */
                if (history_pos > 0) {
                    history_pos--;
                    safe_copy(buf, history[history_pos], sizeof(buf));
                    len = (int)strlen(buf);
                    cur = len;
                    render_line(buf, len, cur);
                }
                continue;
            }
            if (b2 == 'B') {                       /* down */
                if (history_pos < history_count) {
                    history_pos++;
                    if (history_pos == history_count) {
                        buf[0] = 0; len = 0; cur = 0;
                    } else {
                        safe_copy(buf, history[history_pos], sizeof(buf));
                        len = (int)strlen(buf); cur = len;
                    }
                    render_line(buf, len, cur);
                }
                continue;
            }
            if (b2 == 'C') {                       /* right */
                if (cur < len) {
                    cur++;
                    render_line(buf, len, cur);
                }
                continue;
            }
            if (b2 == 'D') {                       /* left */
                if (cur > 0) {
                    cur--;
                    render_line(buf, len, cur);
                }
                continue;
            }
            continue;
        }
        if (c < 0x20) continue;          /* drop unhandled controls */

        if (len + 1 >= (int)sizeof(buf)) continue;
        for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
        buf[cur++] = (char)c;
        len++;
        buf[len] = 0;
        render_line(buf, len, cur);
    }
}

/* ---- argv split ---- */

/* Tokenize `line` in-place, honoring `'...'` and `"..."` as single
 * tokens whose body keeps spaces/semicolons/pipes verbatim. Backslash
 * outside quotes escapes the next char. The quote chars themselves
 * are STRIPPED from the resulting token (so `echo "hi"` makes
 * argv[1] = `hi`, not `"hi"`). This matches what the kernel's
 * build_argv_block does when re-tokenising args for external
 * commands — making the contract uniform between builtins (which
 * see argv directly from here) and externals.
 *
 * Tokens are written back into `line` in-place: the trailing-quote
 * NUL terminator overlaps the original closing quote, and the loop
 * shifts following bytes down by 1 to fill the gap of the opening
 * quote.
 */
static int split_args(char *line, char **argv, int max) {
    int n = 0;
    char *r = line;            /* read cursor  */
    char *w = line;            /* write cursor */
    while (*r && n < max - 1) {
        while (*r == ' ' || *r == '\t') r++;
        if (!*r) break;
        argv[n++] = w;

        int in_sq = 0, in_dq = 0;
        while (*r) {
            char c = *r;
            if (!in_sq && !in_dq && c == '\\' && r[1]) {
                *w++ = r[1];
                r += 2;
                continue;
            }
            if (!in_dq && c == '\'') { in_sq = !in_sq; r++; continue; }
            if (!in_sq && c == '"')  { in_dq = !in_dq; r++; continue; }
            if (!in_sq && !in_dq && (c == ' ' || c == '\t')) {
                /* Advance past the boundary space BEFORE writing NUL.
                 * `w` and `r` could be at the same position (no quote
                 * stripping happened in this token), so writing NUL
                 * at w would clobber the space r was about to skip. */
                r++;
                break;
            }
            *w++ = c;
            r++;
        }
        *w++ = 0;
    }
    argv[n] = 0;
    return n;
}

/* ---- builtins (same set as chunk 1) ---- */

static int do_help(int argc, char **argv);
static int do_exit(int argc, char **argv);
static int do_pwd (int argc, char **argv);
static int do_cd  (int argc, char **argv);
static int do_ls  (int argc, char **argv);
static int do_cat (int argc, char **argv);
static int do_echo(int argc, char **argv);
static int do_hist(int argc, char **argv);

typedef int (*cmd_fn)(int, char **);
typedef struct { const char *name; cmd_fn fn; const char *help; } cmd_t;

static int  do_test(int argc, char **argv);
static int  do_jobs(int argc, char **argv);
static int  do_fg  (int argc, char **argv);
static int  do_bg  (int argc, char **argv);
static const char *pack_envp(char *buf, size_t cap);
static int  do_kill  (int argc, char **argv);
static int  do_export(int argc, char **argv);
static int  do_unset (int argc, char **argv);
static int  do_setenv(int argc, char **argv);
static int  do_exec  (int argc, char **argv);
static void resolve_cmd_path(const char *name, char *out, size_t cap);
static void wait_pid(long pid);
static int  wait_pid_or_stop(long pid);
static int  wait_pid_capture(long pid, int *stopped_out);

/* Last-command exit status, exposed as $? via expand_vars. Set by
 * dispatch() after every foreground command (builtin or spawn) and
 * read by `&&` / `||` sequencing. Default 0 at startup. */
static int last_status;

/* Background-job tracking. shellsrv adds an entry every time the
 * user appends `&` to a pipeline; `jobs` enumerates the live ones. */
#define MAX_JOBS 16
typedef struct {
    long pid;
    char cmd[64];
} job_t;
static job_t bg_jobs[MAX_JOBS];
static int   n_bg_jobs;

static void bg_jobs_remember(long pid, const char *cmd) {
    if (n_bg_jobs >= MAX_JOBS) return;
    bg_jobs[n_bg_jobs].pid = pid;
    safe_copy(bg_jobs[n_bg_jobs].cmd, cmd, sizeof(bg_jobs[n_bg_jobs].cmd));
    n_bg_jobs++;
}

static const cmd_t COMMANDS[] = {
    { "help",    do_help, "list available commands"     },
    { "exit",    do_exit, "leave shellsrv"              },
    { "pwd",     do_pwd,  "print working directory"     },
    { "cd",      do_cd,   "change working directory"    },
    { "ls",      do_ls,   "list directory contents"     },
    { "cat",     do_cat,  "print file contents"         },
    { "echo",    do_echo, "echo arguments (no escapes)" },
    { "history", do_hist, "show command history"        },
    { "test",    do_test, "run /bin/kerntest ABI suite" },
    { "jobs",    do_jobs, "list background tasks"       },
    { "fg",      do_fg,   "fg <pid>: resume stopped task in foreground" },
    { "bg",      do_bg,   "bg <pid>: resume stopped task in background" },
    { "kill",    do_kill,   "kill <pid>"                  },
    { "export",  do_export, "export VAR=VAL (set env var)" },
    { "unset",   do_unset,  "unset VAR (remove env var)"  },
    { "setenv",  do_setenv, "setenv VAR VAL (alias of export)" },
    { "exec",    do_exec,   "exec CMD [args]: replace shell with CMD" },
};

static int should_exit = 0;

static int do_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("shellsrv — ring-3 shell (FASE 10.4 chunk 2)\n");
    for (size_t i = 0; i < sizeof(COMMANDS)/sizeof(COMMANDS[0]); i++) {
        printf("  %-8s — %s\n", COMMANDS[i].name, COMMANDS[i].help);
    }
    return 0;
}
static int do_exit(int argc, char **argv) {
    (void)argc; (void)argv; should_exit = 1; return 0;
}
static int do_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == 0) {
        fprintf(stderr, "pwd: %s\n", strerror(errno)); return 1;
    }
    printf("%s\n", buf); return 0;
}
/* Normalize an absolute path in place, collapsing "//", "./", "../".
 * Caller has guaranteed path[0] == '/'. */
static void path_normalize(char *path) {
    char tmp[PATH_MAX];
    size_t op = 0;
    tmp[op++] = '/';
    size_t i = 1;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        char tok[64];
        int tn = 0;
        while (path[i] && path[i] != '/' && tn < 63) tok[tn++] = path[i++];
        tok[tn] = 0;
        if (tn == 1 && tok[0] == '.') continue;
        if (tn == 2 && tok[0] == '.' && tok[1] == '.') {
            /* Pop trailing segment from tmp. */
            if (op > 1) {
                op--;                        /* drop separator */
                while (op > 0 && tmp[op - 1] != '/') op--;
            }
            if (op == 0) tmp[op++] = '/';
            continue;
        }
        if (op > 1) tmp[op++] = '/';
        for (int j = 0; j < tn && op < PATH_MAX - 1; j++) tmp[op++] = tok[j];
    }
    if (op == 0) tmp[op++] = '/';
    tmp[op] = 0;
    safe_copy(path, tmp, PATH_MAX);
}

/* Resolve `target` against the shell's cwd into `out` (absolute,
 * normalized). Handles "..", ".", relative paths, mixed slashes. */
static void resolve_path(const char *target, char *out, size_t cap) {
    char buf[PATH_MAX];
    if (target[0] == '/') {
        safe_copy(buf, target, sizeof(buf));
    } else {
        if (getcwd(buf, sizeof(buf)) == 0) safe_copy(buf, "/", sizeof(buf));
        size_t l = strlen(buf);
        if (l == 0 || buf[l - 1] != '/') {
            if (l + 1 < sizeof(buf)) { buf[l++] = '/'; buf[l] = 0; }
        }
        safe_cat(buf, target, sizeof(buf));
    }
    path_normalize(buf);
    safe_copy(out, buf, cap);
}

static int do_cd(int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : "/home";
    char resolved[PATH_MAX];
    resolve_path(target, resolved, sizeof(resolved));
    if (chdir(resolved) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}
/* List one directory by reading its entries via opendir/readdir. */
static int ls_one_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    struct dirent *ent;
    while ((ent = readdir(d)) != 0) {
        if (ent->d_name[0] == '.' && ent->d_name[1] == 0) continue;
        if (ent->d_name[0] == '.' && ent->d_name[1] == '.'
                                  && ent->d_name[2] == 0) continue;
        printf("%s\n", ent->d_name);
    }
    closedir(d);
    return 0;
}

/* `ls` semantics (POSIX):
 *   ls            — list cwd
 *   ls DIR        — list DIR
 *   ls FILE       — print FILE (its own path; "exists" check)
 *   ls A B C ...  — for each: if A is a dir, header + contents;
 *                              if A is a file, print the name.
 *
 * Two passes so output matches GNU ls: files first (without header),
 * then dirs (with "PATH:" header when >1 arg). Each pass preserves
 * argv order. Common-case `ls` (no args) and `ls /home` keep their
 * old shape (no header, just entries). */
static int do_ls(int argc, char **argv) {
    if (argc < 2) return ls_one_dir(".");

    /* Classify each arg. file_idx[] / dir_idx[] hold positions in
     * argv. missing args print an error in pass 0. */
    int rc = 0;
    int n_files = 0, n_dirs = 0;

    struct stat st;
    for (int i = 1; i < argc; i++) {
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "ls: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            argv[i][0] = 0;            /* mark as skipped */
            continue;
        }
        if (S_ISDIR(st.st_mode)) n_dirs++;
        else                     n_files++;
    }

    /* Pass 1 — print regular files (one per line). */
    for (int i = 1; i < argc; i++) {
        if (!argv[i][0]) continue;
        if (stat(argv[i], &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) continue;
        printf("%s\n", argv[i]);
    }

    /* Pass 2 — list each directory. Header only when there are
     * multiple paths involved (matches GNU ls). */
    int needs_blank = (n_files > 0);
    int total_paths = n_files + n_dirs;
    int dir_seen = 0;
    for (int i = 1; i < argc; i++) {
        if (!argv[i][0]) continue;
        if (stat(argv[i], &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;

        if (total_paths > 1) {
            if (needs_blank || dir_seen) printf("\n");
            printf("%s:\n", argv[i]);
        }
        if (ls_one_dir(argv[i]) != 0) rc = 1;
        dir_seen = 1;
    }
    return rc;
}
static int do_cat(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "cat: missing argument\n"); return 1; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cat: %s: %s\n", argv[1], strerror(errno)); return 1; }
    char buf[512]; long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);
    close(fd);
    return 0;
}
static int do_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) write(1, " ", 1);
        write(1, argv[i], strlen(argv[i]));
    }
    write(1, "\n", 1);
    return 0;
}
static int do_hist(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < history_count; i++) {
        printf("%4d  %s\n", i + 1, history[i]);
    }
    return 0;
}

static int do_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Sweep the kernel task table to discover the current state of
     * each tracked background pid; drop dead ones. */
    int live[MAX_JOBS] = {0};
    const char *state_label[MAX_JOBS] = {0};

    for (int i = 0; i < n_bg_jobs; i++) {
        for (size_t k = 0; k < 16; k++) {
            osnos_taskinfo_t info;
            long r = osnos_syscall2(265, (long)k, (long)&info);
            if (r < 0) continue;
            if ((long)info.pid != bg_jobs[i].pid) continue;
            if (info.state == OSNOS_TASK_DEAD)   continue;
            if (info.state == OSNOS_TASK_ZOMBIE) continue;
            live[i] = 1;
            switch (info.state) {
                case OSNOS_TASK_RUNNING: state_label[i] = "running"; break;
                case OSNOS_TASK_READY:   state_label[i] = "ready";   break;
                case OSNOS_TASK_BLOCKED: state_label[i] = "blocked"; break;
                case OSNOS_TASK_STOPPED: state_label[i] = "stopped"; break;
                default:                 state_label[i] = "?";       break;
            }
            break;
        }
    }

    int printed = 0;
    for (int i = 0; i < n_bg_jobs; i++) {
        if (!live[i]) continue;
        printf("[%d]  pid=%ld  %s  %s\n",
               i + 1, bg_jobs[i].pid, state_label[i], bg_jobs[i].cmd);
        printed++;
    }
    if (printed == 0) printf("no background jobs\n");

    /* Compact the array, dropping dead entries. */
    int w = 0;
    for (int r = 0; r < n_bg_jobs; r++) {
        if (live[r]) {
            if (w != r) bg_jobs[w] = bg_jobs[r];
            w++;
        }
    }
    n_bg_jobs = w;
    return 0;
}

static long parse_pid_arg(int argc, char **argv) {
    if (argc < 2) {
        if (n_bg_jobs > 0) return bg_jobs[n_bg_jobs - 1].pid;
        return -1;
    }
    long v = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static int do_fg(int argc, char **argv) {
    long pid = parse_pid_arg(argc, argv);
    if (pid <= 0) { fprintf(stderr, "fg: need a pid\n"); return 1; }
    if (osn_resume(pid) != 0) {
        fprintf(stderr, "fg: %ld: %s\n", pid, strerror(errno));
        return 1;
    }
    /* Now the task is READY again — bring it to foreground (Ctrl+C/Z
     * routing) and wait for it. */
    osn_set_fg(pid);
    int rc = wait_pid_or_stop(pid);
    osn_set_fg(0);
    if (rc == 1) {
        printf("\n[fg]  pid=%ld  stopped again\n", pid);
    }
    return 0;
}

static int do_bg(int argc, char **argv) {
    long pid = parse_pid_arg(argc, argv);
    if (pid <= 0) { fprintf(stderr, "bg: need a pid\n"); return 1; }
    if (osn_resume(pid) != 0) {
        fprintf(stderr, "bg: %ld: %s\n", pid, strerror(errno));
        return 1;
    }
    printf("[bg]  pid=%ld  continued\n", pid);
    return 0;
}

static int do_kill(int argc, char **argv) {
    long pid = parse_pid_arg(argc, argv);
    if (pid <= 0) { fprintf(stderr, "kill: need a pid\n"); return 1; }
    /* SYS_KILL on osnos sets kill_pending + wakes if stopped/blocked. */
    long r = osnos_syscall2(62 /* SYS_KILL */, pid, 15 /* SIGTERM */);
    if (r < 0) { fprintf(stderr, "kill: %s\n", strerror(-(int)r)); return 1; }
    return 0;
}

static int do_export(int argc, char **argv) {
    /* Support both forms:
     *   export VAR=VAL    — single token
     *   export VAR VAL    — two tokens (back-compat)
     *   export VAR        — no-op (POSIX would mark for export) */
    if (argc < 2) {
        /* List current env. */
        extern char **environ;
        if (environ) for (int i = 0; environ[i]; i++) printf("export %s\n", environ[i]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = 0;
            if (setenv(argv[i], eq + 1, 1) != 0) {
                fprintf(stderr, "export: %s: %s\n", argv[i], strerror(errno));
                *eq = '='; return 1;
            }
            *eq = '=';
        }
    }
    return 0;
}

static int do_setenv(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: setenv VAR VAL\n"); return 1; }
    if (setenv(argv[1], argv[2], 1) != 0) {
        fprintf(stderr, "setenv: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int do_unset(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: unset VAR\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (unsetenv(argv[i]) != 0) {
            fprintf(stderr, "unset: %s: %s\n", argv[i], strerror(errno));
        }
    }
    return 0;
}

/* `exec CMD [args]` — Linux convention: replace the shell with CMD
 * via execve(2), preserving pid + fds + cwd. On success NEVER
 * returns. On failure, print errno + keep running.
 *
 * IMPORTANT: claim "foreground" via osn_set_fg(getpid()) BEFORE the
 * execve so the kernel TTY routes Ctrl+C / Ctrl+Z to the NEW image
 * (same pid). Without this kernel_fg_pid stays 0 (cleared after the
 * previous run_pipeline wait) and the user has no way to kill an
 * interactive `exec /bin/top` other than rebooting. */
static int do_exec(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: exec CMD [args...]\n");
        return 1;
    }
    char path[PATH_MAX];
    resolve_cmd_path(argv[1], path, sizeof(path));

    /* Restore cooked TTY before handing off — the new process may
     * want a clean termios baseline. */
    leave_raw();

    /* Hand foreground ownership to ourselves (preserved across
     * execve since pid stays the same). */
    osn_set_fg((long)getpid());

    /* execve takes argv[0]=program name (NOT path) per POSIX. */
    extern char **environ;
    execve(path, &argv[1], environ);

    /* Only reached on failure. Roll back the fg claim and report. */
    osn_set_fg(0);
    enter_raw();
    fprintf(stderr, "exec: %s: %s\n", argv[1], strerror(errno));
    return 1;
}

static int do_test(int argc, char **argv) {
    (void)argc; (void)argv;
    char envbuf[2048];
    const char *envp = pack_envp(envbuf, sizeof(envbuf));
    leave_raw();
    long pid = osn_spawn("/bin/kerntest", "", envp, -1, -1);
    enter_raw();
    if (pid <= 0) {
        fprintf(stderr, "test: kerntest unavailable: %s\n", strerror(errno));
        return 1;
    }
    wait_pid(pid);
    return 0;
}

/* ---- pipeline parser ---- */

#define MAX_STAGES   4
#define MAX_ARGV    16

typedef struct {
    char *argv[MAX_ARGV + 1];
    int   argc;
    char *stdin_file;
    char *stdout_file;
    int   stdout_append;
} stage_t;

/* Split `line` by `|` into N stage strings (in-place: replaces `|` with
 * NUL). Each stage string is later tokenised + redirect-extracted by
 * stage_parse. Returns N or -1 on too-many-stages.
 *
 * Quote-aware: `|` inside `'...'` or `"..."` is treated as a literal
 * character, not a pipe operator. Without this, `jq '.a | .b'` was
 * being split mid-program-string. The kernel's build_argv_block (in
 * src/proc/exec.c) already understands `'...'` and `"..."` when re-
 * tokenising args, so this just needs to match that contract at the
 * pipe-split layer. `\\|` outside quotes also escapes the pipe.
 */
static int line_split_stages(char *line, char *stage_raw[MAX_STAGES]) {
    int n = 0;
    stage_raw[n++] = line;
    int in_sq = 0, in_dq = 0;
    for (char *p = line; *p; p++) {
        char c = *p;
        if (!in_sq && !in_dq && c == '\\' && p[1]) {
            p++;             /* skip escaped char */
            continue;
        }
        if (!in_dq && c == '\'') { in_sq = !in_sq; continue; }
        if (!in_sq && c == '"')  { in_dq = !in_dq; continue; }
        if (in_sq || in_dq) continue;
        if (c == '|') {
            *p = 0;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (n >= MAX_STAGES) return -1;
            stage_raw[n++] = p;
            p--;             /* for-loop ++ will skip the first char */
        }
    }
    return n;
}

/* Glob expansion for a single token containing '*'. Walks the
 * directory implied by the token's prefix (defaults to ".") and
 * matches each entry against the pattern. Matches go straight into
 * st->argv (so the caller doesn't have to interleave). If no entry
 * matches, the token is kept literal (bash default behaviour).
 *
 * Supports only the wildcard `*` (zero or more chars). `?` and
 * `[...]` are not implemented; the matcher stays tiny and covers
 * the common cases like a.txt or prefix-globs. */
static int glob_match(const char *pattern, const char *name) {
    /* Recursive descent. *pattern may have at most a few wildcards
     * per token in practice. */
    while (*pattern) {
        if (*pattern == '*') {
            /* Collapse consecutive stars. */
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;            /* trailing *: matches rest */
            /* Try matching the rest at every suffix of `name`. */
            for (const char *q = name; ; q++) {
                if (glob_match(pattern, q)) return 1;
                if (!*q) return 0;
            }
        }
        if (!*name) return 0;
        if (*pattern != *name) return 0;
        pattern++; name++;
    }
    return *name == 0;
}

static int expand_glob_into(const char *token, stage_t *st) {
    /* No `*` → no expansion. Caller will push token literally. */
    int has_star = 0;
    for (const char *p = token; *p; p++) if (*p == '*') { has_star = 1; break; }
    if (!has_star) return 0;

    /* Split into dir + leaf pattern. dir defaults to "." if the token
     * has no slash. */
    char dir[PATH_MAX];
    const char *leaf;
    const char *last_slash = 0;
    for (const char *p = token; *p; p++) if (*p == '/') last_slash = p;
    if (last_slash) {
        size_t dlen = (size_t)(last_slash - token);
        if (dlen == 0) { dir[0] = '/'; dir[1] = 0; }
        else {
            if (dlen >= sizeof(dir)) return 0;
            for (size_t k = 0; k < dlen; k++) dir[k] = token[k];
            dir[dlen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        dir[0] = '.'; dir[1] = 0;
        leaf = token;
    }

    DIR *d = opendir(dir);
    if (!d) return 0;          /* unreadable dir: keep token literal */

    int matched = 0;
    /* Storage for the matched names: tokens point into a heap-ish
     * static buffer so they outlive stage_parse's call frame. */
    static char glob_buf[4096];
    static size_t glob_pos;
    /* Reset only on the FIRST expand of a given stage_parse pass.
     * We approximate that by using a per-stage offset stored in the
     * stage_t — but simpler: reset every time. The buffer is reused
     * across stages in a pipeline; pipelines are short so 4 KiB is
     * comfortable. Once stage_parse returns, the buffer can be
     * reused by the next stage in the same dispatch_segment because
     * the stages are processed serially. */
    (void)glob_pos;

    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        /* Skip "." and ".." unless explicitly requested. */
        if (e->d_name[0] == '.' && leaf[0] != '.') continue;
        if (!glob_match(leaf, e->d_name)) continue;
        if (st->argc >= MAX_ARGV) break;

        /* Build full path back into the static buffer. */
        size_t dl = 0; while (dir[dl]) dl++;
        size_t nl = 0; while (e->d_name[nl]) nl++;
        if (glob_pos + dl + 1 + nl + 1 > sizeof(glob_buf)) break;

        char *out = glob_buf + glob_pos;
        if (!(dir[0] == '.' && dir[1] == 0)) {
            for (size_t k = 0; k < dl; k++) glob_buf[glob_pos++] = dir[k];
            if (dl == 0 || dir[dl - 1] != '/') glob_buf[glob_pos++] = '/';
        }
        for (size_t k = 0; k < nl; k++) glob_buf[glob_pos++] = e->d_name[k];
        glob_buf[glob_pos++] = 0;

        st->argv[st->argc++] = out;
        matched++;
    }
    closedir(d);
    return matched;
}

static void stage_parse(char *raw, stage_t *st) {
    st->argc = 0;
    st->stdin_file = 0;
    st->stdout_file = 0;
    st->stdout_append = 0;
    char *toks[MAX_ARGV * 2 + 4];
    int   tn = split_args(raw, toks, (int)(sizeof(toks)/sizeof(toks[0])));

    for (int i = 0; i < tn; i++) {
        if (strcmp(toks[i], "<") == 0 && i + 1 < tn) {
            st->stdin_file = toks[++i];
        } else if (strcmp(toks[i], ">") == 0 && i + 1 < tn) {
            st->stdout_file   = toks[++i];
            st->stdout_append = 0;
        } else if (strcmp(toks[i], ">>") == 0 && i + 1 < tn) {
            st->stdout_file   = toks[++i];
            st->stdout_append = 1;
        } else {
            /* Try glob expansion first — pushes matches into argv. */
            int n = expand_glob_into(toks[i], st);
            if (n == 0) {
                if (st->argc < MAX_ARGV) st->argv[st->argc++] = toks[i];
            }
        }
    }
    st->argv[st->argc] = 0;
}

/* Wait for `pid` to leave the running set. Returns:
 *   0  — task is DEAD (normal exit / killed)
 *   1  — task is STOPPED (Ctrl+Z) — caller should drop the prompt
 *        without harvesting; `fg pid` / `bg pid` later resumes it.
 */
/* Capture variant: returns the exit_code of the dead task. If
 * `stopped_out` is non-NULL, gets set to 1 when the child is stopped
 * (Ctrl+Z / SIGSTOP) instead of exited.
 *
 * Migrated from sys_taskinfo polling to POSIX waitpid(2) with
 * WUNTRACED. The kernel now wakes shellsrv as soon as the child
 * transitions to ZOMBIE / STOPPED / CONTINUED — no busy poll, no
 * race against the reaper, exit_code preserved by TASK_ZOMBIE
 * state until consumed. */
static int wait_pid_capture(long pid, int *stopped_out) {
    if (stopped_out) *stopped_out = 0;
    int status = 0;
    for (;;) {
        pid_t r = waitpid((pid_t)pid, &status, WUNTRACED);
        if (r == (pid_t)pid) {
            if (WIFSTOPPED(status)) {
                if (stopped_out) *stopped_out = 1;
                return 0;
            }
            if (WIFEXITED(status))   return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 0;
        }
        if (r == -1) {
            if (errno == EINTR) continue;       /* signal during wait */
            return 0;                            /* ECHILD or other → done */
        }
        /* r == 0 shouldn't happen without WNOHANG; treat as done. */
        return 0;
    }
}

static int wait_pid_or_stop(long pid) {
    int stopped = 0;
    int ec = wait_pid_capture(pid, &stopped);
    last_status = stopped ? 128 + 19 /* SIGSTOP */ : ec;
    return stopped ? 1 : 0;
}

static void wait_pid(long pid) {
    int stopped = 0;
    last_status = wait_pid_capture(pid, &stopped);
    if (stopped) last_status = 128 + 19;
}

/* Resolve a command name against PATH. Absolute paths pass through;
 * relative names get tried against each colon-separated component of
 * $PATH (defaulting to "/bin" if unset). First file that vfs_stat's
 * as a regular file wins. Falls back to "/bin/<name>" so the kernel
 * fallback ELFs still work when no /bin entry exists on disk yet. */
static void resolve_cmd_path(const char *name, char *out, size_t cap) {
    if (name[0] == '/') { safe_copy(out, name, cap); return; }

    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) path_env = "/bin";

    const char *p = path_env;
    while (*p) {
        char dir[PATH_MAX];
        size_t dn = 0;
        while (*p && *p != ':' && dn + 1 < sizeof(dir)) dir[dn++] = *p++;
        dir[dn] = 0;
        if (*p == ':') p++;
        if (dn == 0) continue;

        char candidate[PATH_MAX];
        safe_copy(candidate, dir,  sizeof(candidate));
        if (dn > 0 && dir[dn - 1] != '/') safe_cat(candidate, "/", sizeof(candidate));
        safe_cat(candidate, name, sizeof(candidate));

        struct stat st;
        if (stat(candidate, &st) == 0) {
            safe_copy(out, candidate, cap);
            return;
        }
    }
    /* Not found anywhere — return "/bin/<name>" so the kernel-side
     * builtin fallback still gets a chance. */
    safe_copy(out, "/bin/", cap);
    safe_cat (out, name,    cap);
}

/* Pack argv[1..argc-1] into a single space-separated args string
 * (proc_execve convention — single args blob). */
/* Pack argv[1..argc-1] into a single space-separated string that the
 * kernel's build_argv_block (src/proc/exec.c) re-tokenises back into
 * argv on the child side.
 *
 * Each token gets wrapped in double quotes — and any literal `"` or
 * `\` inside the token gets backslash-escaped — so the kernel's
 * quote-aware tokeniser reconstructs the exact same boundaries the
 * user typed. Without this, `jq '.tools | length'` arrives as the
 * flat string `.tools | length`, which the kernel splits on
 * whitespace into 3 args. After this we send `".tools | length"`
 * which the kernel re-tokenises as a single arg.
 *
 * Tokens with no spaces / no special chars could skip the quotes,
 * but always-quoting is simpler and the kernel handles both shapes.
 */
static void pack_args(stage_t *st, char *out, size_t cap) {
    out[0] = 0;
    size_t pos = 0;
    for (int i = 1; i < st->argc; i++) {
        if (pos + 1 >= cap) break;
        if (i > 1) out[pos++] = ' ';
        if (pos + 1 >= cap) break;
        out[pos++] = '"';
        const char *s = st->argv[i];
        while (*s && pos + 2 < cap) {
            if (*s == '"' || *s == '\\') {
                out[pos++] = '\\';
                if (pos + 1 >= cap) break;
            }
            out[pos++] = *s++;
        }
        if (pos + 1 >= cap) break;
        out[pos++] = '"';
    }
    out[pos] = 0;
}

/* Build a flat envp blob ("KEY=VAL\0KEY=VAL\0\0") from the live
 * `environ` array. SYS_SPAWN expects this format. */
static const char *pack_envp(char *buf, size_t cap) {
    extern char **environ;
    size_t pos = 0;
    if (environ) {
        for (int i = 0; environ[i]; i++) {
            const char *e = environ[i];
            size_t l = strlen(e);
            if (pos + l + 2 > cap) break;     /* leave room for final NUL */
            for (size_t k = 0; k < l; k++) buf[pos++] = e[k];
            buf[pos++] = 0;
        }
    }
    buf[pos] = 0;
    return pos > 0 ? buf : 0;
}

static void run_pipeline(stage_t *stages, int n, int background,
                          const char *line_for_label) {
    long pids[MAX_STAGES];
    int  npids = 0;
    int  pipe_in_fd = -1;            /* read end of pipe FROM previous stage */

    for (int i = 0; i < n; i++) {
        stage_t *st = &stages[i];
        if (st->argc == 0) {
            fprintf(stderr, "shellsrv: empty stage in pipeline\n");
            goto cleanup;
        }

        /* Stdin: previous pipe (if any) overrides any < redirect on
         * a non-first stage — same semantics as bash. */
        int stdin_fd = pipe_in_fd;
        if (i == 0 && st->stdin_file) {
            stdin_fd = open(st->stdin_file, O_RDONLY);
            if (stdin_fd < 0) {
                fprintf(stderr, "shellsrv: %s: %s\n",
                        st->stdin_file, strerror(errno));
                goto cleanup;
            }
        }

        /* Stdout: pipe to next stage, OR final-stage redirect, OR
         * default (let it flow to TTY). */
        int stdout_fd  = -1;
        int next_pipe_in = -1;
        if (i < n - 1) {
            int p[2];
            if (pipe(p) != 0) {
                fprintf(stderr, "shellsrv: pipe(): %s\n", strerror(errno));
                if (stdin_fd >= 3) close(stdin_fd);
                goto cleanup;
            }
            stdout_fd    = p[1];
            next_pipe_in = p[0];
        } else if (st->stdout_file) {
            int flags = O_WRONLY | O_CREAT |
                        (st->stdout_append ? O_APPEND : O_TRUNC);
            stdout_fd = open(st->stdout_file, flags, 0644);
            if (stdout_fd < 0) {
                fprintf(stderr, "shellsrv: %s: %s\n",
                        st->stdout_file, strerror(errno));
                if (stdin_fd >= 3) close(stdin_fd);
                goto cleanup;
            }
        }

        char path[PATH_MAX];
        char args[256];
        char envbuf[2048];
        resolve_cmd_path(st->argv[0], path, sizeof(path));
        pack_args(st, args, sizeof(args));
        const char *envp_flat = pack_envp(envbuf, sizeof(envbuf));

        leave_raw();
        long pid = osn_spawn(path, args, envp_flat, stdin_fd, stdout_fd);
        enter_raw();
        if (pid <= 0) {
            fprintf(stderr, "shellsrv: %s: %s\n",
                    st->argv[0],
                    pid < 0 ? strerror(errno) : "no such command");
            /* osn_spawn left fds untouched on failure → free what we
             * opened ourselves. */
            if (stdin_fd  >= 3) close(stdin_fd);
            if (stdout_fd >= 3) close(stdout_fd);
            if (next_pipe_in >= 0) close(next_pipe_in);
            goto cleanup;
        }
        pids[npids++] = pid;
        pipe_in_fd = next_pipe_in;
    }

    if (background) {
        /* Don't wait — register the LAST stage's pid as the job
         * leader and return to the prompt immediately. */
        if (npids > 0) {
            long lead = pids[npids - 1];
            bg_jobs_remember(lead, line_for_label ? line_for_label : "");
            printf("[%d]  pid=%ld  &\n", n_bg_jobs, lead);
        }
        if (pipe_in_fd >= 0) close(pipe_in_fd);
        return;
    }
    /* Foreground: tell the kernel "Ctrl+C/Ctrl+Z should target the
     * LAST stage's child" — Ctrl+Z on a pipeline stops the leader,
     * which matches user intuition (the pipeline as a whole pauses).
     * If any task stops (rc==1), bookkeep it as a background-style
     * job so `fg pid` / `bg pid` can resume. */
    if (npids > 0) osn_set_fg(pids[npids - 1]);
    int stopped = 0;
    long stopped_pid = 0;
    for (int k = 0; k < npids; k++) {
        if (wait_pid_or_stop(pids[k]) == 1) {
            stopped = 1;
            stopped_pid = pids[k];
        }
    }
    osn_set_fg(0);
    if (stopped) {
        bg_jobs_remember(stopped_pid,
                         line_for_label ? line_for_label : "(stopped)");
        printf("\n[%d]  pid=%ld  stopped\n", n_bg_jobs, stopped_pid);
    }
    if (pipe_in_fd >= 0) close(pipe_in_fd);
    return;

cleanup:
    if (pipe_in_fd >= 0) close(pipe_in_fd);
    for (int k = 0; k < npids; k++) wait_pid(pids[k]);
}

/* ---- env-var expansion ---- */

/* Walk `in`, copying to `out` while substituting $VAR / ${VAR}
 * with getenv("VAR") (empty if unset). Single-quoted regions are
 * passed through verbatim; double-quoted regions still expand
 * (POSIX). Backslash-escape \$ disables expansion. */
static void expand_vars(const char *in, char *out, size_t cap) {
    size_t op = 0;
    int  in_single = 0;
    while (*in && op + 1 < cap) {
        char c = *in;
        if (c == '\\' && in[1] == '$' && !in_single) {
            out[op++] = '$';
            in += 2;
            continue;
        }
        if (c == '\'') {
            in_single = !in_single;
            out[op++] = c;     /* keep the quote for build_argv_block */
            in++;
            continue;
        }
        if (c == '$' && !in_single) {
            in++;
            /* `$?` — last command exit status. */
            if (*in == '?') {
                char buf[16];
                int n = snprintf(buf, sizeof(buf), "%d", last_status);
                for (int k = 0; k < n && op + 1 < cap; k++) out[op++] = buf[k];
                in++;
                continue;
            }
            int braces = 0;
            if (*in == '{') { braces = 1; in++; }
            char name[64];
            int  nn = 0;
            while (*in &&
                   ((*in >= 'a' && *in <= 'z') ||
                    (*in >= 'A' && *in <= 'Z') ||
                    (*in >= '0' && *in <= '9') ||
                    *in == '_') &&
                   nn + 1 < (int)sizeof(name)) {
                name[nn++] = *in++;
            }
            name[nn] = 0;
            if (braces && *in == '}') in++;
            if (nn == 0) {
                /* Bare `$` — pass through. */
                if (op + 1 < cap) out[op++] = '$';
                continue;
            }
            const char *val = getenv(name);
            if (val) {
                while (*val && op + 1 < cap) out[op++] = *val++;
            }
            continue;
        }
        out[op++] = *in++;
    }
    out[op] = 0;
}

/* ---- dispatch ---- */

/* Run ONE segment of the line (one pipeline). Updates last_status.
 * Called by dispatch() once per ;/&&/|| segment. Variable expansion
 * happens here, AFTER the segment split, so `$?` in segment N sees
 * the status set by segment N-1. */
static void dispatch_segment(char *line_in) {
    char line_buf[LINE_MAX_LEN];
    expand_vars(line_in, line_buf, sizeof(line_buf));
    char *line = line_buf;

    /* Preserve a printable copy of the (expanded) line for the
     * `jobs` builtin (split_args mutates `line` in place). */
    char line_copy[LINE_MAX_LEN];
    safe_copy(line_copy, line, sizeof(line_copy));

    /* Detect a trailing `&` (with optional whitespace before it) so
     * we can spawn the pipeline in the background. */
    int background = 0;
    int last = (int)strlen(line) - 1;
    while (last >= 0 && (line[last] == ' ' || line[last] == '\t')) last--;
    if (last >= 0 && line[last] == '&') {
        background = 1;
        line[last] = ' ';
        int last_label = (int)strlen(line_copy) - 1;
        while (last_label >= 0 && (line_copy[last_label] == ' ' ||
                                    line_copy[last_label] == '\t')) last_label--;
        if (last_label >= 0 && line_copy[last_label] == '&') {
            line_copy[last_label] = 0;
        }
    }

    char *raw[MAX_STAGES];
    int n = line_split_stages(line, raw);
    if (n <= 0) {
        if (n < 0) fprintf(stderr, "shellsrv: too many pipeline stages\n");
        last_status = (n < 0) ? 1 : 0;
        return;
    }

    stage_t stages[MAX_STAGES];
    for (int i = 0; i < n; i++) stage_parse(raw[i], &stages[i]);

    if (n == 1 && !background &&
        !stages[0].stdin_file && !stages[0].stdout_file) {
        stage_t *st = &stages[0];
        if (st->argc == 0) return;
        for (size_t i = 0; i < sizeof(COMMANDS)/sizeof(COMMANDS[0]); i++) {
            if (strcmp(st->argv[0], COMMANDS[i].name) == 0) {
                last_status = COMMANDS[i].fn(st->argc, st->argv);
                return;
            }
        }
    }

    /* Background pipelines always count as success (0) for ;/&&/||
     * — same as bash; the foreground ones get the real exit_code
     * captured inside wait_pid_or_stop. */
    if (background) last_status = 0;
    run_pipeline(stages, n, background, line_copy);
}

/* dispatch — top-level entry. Splits the line into segments on
 * `;`, `&&`, `||` and runs each through dispatch_segment with the
 * conditional logic:
 *   `;`  → always run next
 *   `&&` → run next only if last_status == 0
 *   `||` → run next only if last_status != 0
 * Pipes `|` are NOT split here (they're part of a single segment
 * and handled by line_split_stages / run_pipeline). */
static void dispatch(char *line_in) {
    /* Work on a mutable copy so we can NUL-terminate segments in
     * place. line_in came from read_line or .oshrc reader's stack
     * buffer; mutating it is fine but copying keeps the contract
     * narrow (caller doesn't see operator NULs). */
    char buf[LINE_MAX_LEN];
    safe_copy(buf, line_in, sizeof(buf));

    char *p = buf;
    enum { OP_FIRST, OP_SEMI, OP_AND, OP_OR } prev_op = OP_FIRST;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *seg = p;
        /* Walk to next operator. `|` alone is a pipe (skip past — it
         * gets handled later by line_split_stages), `||` is OR (stop).
         * `&` alone is background, `&&` is AND (stop).
         *
         * Quote-aware: a `;`/`&&`/`||` inside `'...'` or `"..."` is
         * not an operator — same rule as line_split_stages. Matches
         * what the kernel's build_argv_block does when re-tokenising. */
        int in_sq = 0, in_dq = 0;
        while (*p) {
            char c = *p;
            if (!in_sq && !in_dq && c == '\\' && p[1]) {
                p += 2;
                continue;
            }
            if (!in_dq && c == '\'') { in_sq = !in_sq; p++; continue; }
            if (!in_sq && c == '"')  { in_dq = !in_dq; p++; continue; }
            if (in_sq || in_dq) { p++; continue; }
            if (c == ';') break;
            if (p[0] == '&' && p[1] == '&') break;
            if (p[0] == '|' && p[1] == '|') break;
            p++;
        }
        int next_op;
        if (*p == ';') {
            *p++ = 0;
            next_op = OP_SEMI;
        } else if (p[0] == '&' && p[1] == '&') {
            *p++ = 0; *p++ = 0;
            next_op = OP_AND;
        } else if (p[0] == '|' && p[1] == '|') {
            *p++ = 0; *p++ = 0;
            next_op = OP_OR;
        } else {
            /* End of line. */
            next_op = -1;
        }

        /* Right-trim segment. */
        size_t l = strlen(seg);
        while (l > 0 && (seg[l-1] == ' ' || seg[l-1] == '\t')) seg[--l] = 0;

        int should_run = 0;
        switch (prev_op) {
            case OP_FIRST: should_run = 1; break;
            case OP_SEMI:  should_run = 1; break;
            case OP_AND:   should_run = (last_status == 0); break;
            case OP_OR:    should_run = (last_status != 0); break;
        }
        if (should_run && *seg) dispatch_segment(seg);

        if (next_op < 0) break;
        prev_op = (next_op == OP_SEMI) ? OP_SEMI :
                  (next_op == OP_AND)  ? OP_AND  : OP_OR;
    }
}

/* ---- main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\nshellsrv: ring-3 shell (FASE 10.4)\n");
    printf("type 'help' for commands\n");

    /* Claim SERVER_SHELL so IPC_PROC_* land here. */
    ipc_service_register(SERVER_SHELL);

    history_load();

    /* Replay .oshrc line-by-line BEFORE entering raw mode — the rc
     * is just a stream of commands; same dispatch path, but we
     * don't echo / accept input, just run. */
    int rc_fd = open("/home/.oshrc", O_RDONLY);
    if (rc_fd >= 0) {
        char rc_line[LINE_MAX_LEN];
        int  pos = 0;
        char chunk[256];
        long n;
        while ((n = read(rc_fd, chunk, sizeof(chunk))) > 0) {
            for (long i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\n' || pos + 1 >= (int)sizeof(rc_line)) {
                    rc_line[pos] = 0;
                    if (pos > 0 && rc_line[0] != '#') {
                        history_save(rc_line);
                        dispatch(rc_line);
                    }
                    pos = 0;
                } else if (c != '\r') {
                    rc_line[pos++] = c;
                }
            }
        }
        if (pos > 0) {
            rc_line[pos] = 0;
            if (rc_line[0] != '#') dispatch(rc_line);
        }
        close(rc_fd);
    }

    enter_raw();

    char line[LINE_MAX_LEN];
    while (!should_exit) {
        build_prompt();
        int rc = read_line(line, sizeof(line));
        if (rc == LINE_EOF) break;
        if (rc == LINE_INT) continue;
        if (rc == 0) continue;
        history_save(line);
        dispatch(line);
    }

    leave_raw();
    printf("shellsrv: bye\n");
    return 0;
}
