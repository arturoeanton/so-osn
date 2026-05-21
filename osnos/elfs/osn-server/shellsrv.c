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

static int split_args(char *line, char **argv, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
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
static void wait_pid(long pid);
static int  wait_pid_or_stop(long pid);

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
static int do_cd(int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : "/home";
    if (chdir(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}
static int do_ls(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : ".";
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
 * stage_parse. Returns N or -1 on too-many-stages. */
static int line_split_stages(char *line, char *stage_raw[MAX_STAGES]) {
    int n = 0;
    stage_raw[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '|') {
            *p = 0;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (n >= MAX_STAGES) return -1;
            stage_raw[n++] = p;
            /* `p` will be incremented by the for-loop again, so step
             * back one. */
            p--;
        }
    }
    return n;
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
            if (st->argc < MAX_ARGV) st->argv[st->argc++] = toks[i];
        }
    }
    st->argv[st->argc] = 0;
}

/* Wait for `pid` to leave the running set. Returns:
 *   0  — task is DEAD (normal exit / killed)
 *   1  — task is STOPPED (Ctrl+Z) — caller should drop the prompt
 *        without harvesting; `fg pid` / `bg pid` later resumes it.
 */
static int wait_pid_or_stop(long pid) {
    for (;;) {
        int found = 0;
        uint8_t state = 0;
        for (size_t i = 0; i < 16; i++) {
            osnos_taskinfo_t info;
            long r = osnos_syscall2(265, (long)i, (long)&info);
            if (r < 0) continue;
            if ((long)info.pid != pid) continue;
            found = 1;
            state = info.state;
            break;
        }
        if (!found || state == OSNOS_TASK_DEAD) return 0;
        if (state == OSNOS_TASK_STOPPED)         return 1;
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, 0);
    }
}

static void wait_pid(long pid) {
    (void)wait_pid_or_stop(pid);
}

/* Build "/bin/<name>" or pass absolute. */
static void resolve_cmd_path(const char *name, char *out, size_t cap) {
    if (name[0] == '/') {
        safe_copy(out, name, cap);
    } else {
        safe_copy(out, "/bin/", cap);
        safe_cat (out, name,    cap);
    }
}

/* Pack argv[1..argc-1] into a single space-separated args string
 * (proc_execve convention — single args blob). */
static void pack_args(stage_t *st, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 1; i < st->argc; i++) {
        if (i > 1) safe_cat(out, " ", cap);
        safe_cat(out, st->argv[i], cap);
    }
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

static void dispatch(char *line_in) {
    /* Expand $VAR / ${VAR} BEFORE parsing, so children see the
     * resolved arguments and pipes/redirects parse correctly. */
    char line_buf[LINE_MAX_LEN];
    expand_vars(line_in, line_buf, sizeof(line_buf));
    char *line = line_buf;

    /* Preserve a printable copy of the (expanded) line for the
     * `jobs` builtin (split_args mutates `line` in place). */
    char line_copy[LINE_MAX_LEN];
    safe_copy(line_copy, line, sizeof(line_copy));

    /* Detect a trailing `&` (with optional whitespace before it) so
     * we can spawn the pipeline in the background. The flag is
     * recognised across the whole line, not per-stage. */
    int background = 0;
    int last = (int)strlen(line) - 1;
    while (last >= 0 && (line[last] == ' ' || line[last] == '\t')) last--;
    if (last >= 0 && line[last] == '&') {
        background = 1;
        line[last] = ' ';   /* let the parser see it as plain whitespace */
        /* Also strip the trailing '&' from the label so `jobs` shows
         * a clean command. */
        int last_label = (int)strlen(line_copy) - 1;
        while (last_label >= 0 && (line_copy[last_label] == ' ' ||
                                    line_copy[last_label] == '\t')) last_label--;
        if (last_label >= 0 && line_copy[last_label] == '&') {
            line_copy[last_label] = 0;
        }
    }

    /* Pipe-aware path: split into stages, parse each, run pipeline.
     * Builtins only run when there's exactly one stage AND no
     * redirects AND no background — otherwise we fall through to
     * /bin/<name> ELF so redirects + pipes + bg can wire its fds. */
    char *raw[MAX_STAGES];
    int n = line_split_stages(line, raw);
    if (n <= 0) {
        if (n < 0) fprintf(stderr, "shellsrv: too many pipeline stages\n");
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
                (void)COMMANDS[i].fn(st->argc, st->argv);
                return;
            }
        }
    }

    run_pipeline(stages, n, background, line_copy);
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
