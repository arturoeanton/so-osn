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
    printf("\r\x1b[K\x1b[38;2;0;255;102m%s\x1b[39m", prompt_buf);
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

static const cmd_t COMMANDS[] = {
    { "help",    do_help, "list available commands"     },
    { "exit",    do_exit, "leave shellsrv"              },
    { "pwd",     do_pwd,  "print working directory"     },
    { "cd",      do_cd,   "change working directory"    },
    { "ls",      do_ls,   "list directory contents"     },
    { "cat",     do_cat,  "print file contents"         },
    { "echo",    do_echo, "echo arguments (no escapes)" },
    { "history", do_hist, "show command history"        },
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

/* ---- dispatch ---- */

static void dispatch(char *line) {
    char *argv[16];
    int argc = split_args(line, argv, 16);
    if (argc == 0) return;

    for (size_t i = 0; i < sizeof(COMMANDS)/sizeof(COMMANDS[0]); i++) {
        if (strcmp(argv[0], COMMANDS[i].name) == 0) {
            (void)COMMANDS[i].fn(argc, argv);
            return;
        }
    }

    /* Not a builtin — spawn /bin/<argv[0]> via osn_spawn. We must
     * restore the user's normal TTY before the child runs so its
     * canonical-mode reads still work; the child's `read()` would
     * otherwise see byte-at-a-time + no echo. */
    char path[PATH_MAX];
    if (argv[0][0] == '/') {
        safe_copy(path, argv[0], sizeof(path));
    } else {
        safe_copy(path, "/bin/", sizeof(path));
        safe_cat (path, argv[0],  sizeof(path));
    }
    char args[256];
    args[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) safe_cat(args, " ", sizeof(args));
        safe_cat(args, argv[i], sizeof(args));
    }

    leave_raw();
    long pid = osn_spawn(path, args, 0, -1, -1);
    if (pid <= 0) {
        fprintf(stderr, "shellsrv: %s: %s\n",
                argv[0], pid < 0 ? strerror(errno) : "no such command");
        enter_raw();
        return;
    }
    /* Wait for the child to exit. */
    for (;;) {
        int alive = 0;
        for (size_t i = 0; i < 16; i++) {
            osnos_taskinfo_t info;
            long r = osnos_syscall2(265, (long)i, (long)&info);
            if (r < 0) continue;
            if ((long)info.pid == pid && info.state != OSNOS_TASK_DEAD) {
                alive = 1; break;
            }
        }
        if (!alive) break;
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, 0);
    }
    enter_raw();
}

/* ---- main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\nshellsrv: ring-3 shell (FASE 10.4 chunk 2 — raw-mode + history)\n");
    printf("type 'help', or 'exit' to return to the parent shell\n");

    history_load();
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
