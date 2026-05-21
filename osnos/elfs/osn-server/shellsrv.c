/*
 * elfs/osn-server/shellsrv.c — Shell, ring 3 (FASE 10.4, chunk 1).
 *
 * Initial sub-shell skeleton: pure userland prompt loop driven by
 * read(0) + canonical TTY line discipline + a small command table.
 * Runs SIDE BY SIDE with the legacy kernel shell — the user invokes
 * it manually via `shellsrv` from the kernel shell, gets a ring-3
 * prompt, runs commands, `exit`s back. No SERVER_SHELL registration
 * yet, no IPC_KEY_EVENT handling — that lands when this server fully
 * replaces shell_server.c.
 *
 * Goals for this chunk:
 *   - Validate that a ring-3 ELF can drive an interactive prompt
 *     using only the syscall API we've built so far (read/write/
 *     getcwd/chdir/open/getdents/spawn).
 *   - Stay completely self-contained — every dependency is in libc
 *     or osnos_ipc.h. Zero kernel-internal calls.
 *   - Be small enough to read in one go (~250 LOC).
 *
 * Future chunks (next sessions):
 *   - IPC_KEY_EVENT + non-canonical line editor + history.
 *   - SYS_TASKINFO-backed ps/top.
 *   - Pipelines + redirects via pipe(2) + osn_spawn(fd-inherit).
 *   - Job control (Ctrl+Z handling via IPC_PROC_STOPPED).
 *   - Final: kmain spawns this in place of shell_server_tick.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "osnos_ipc.h"
#include "osnos_taskinfo.h"

#define LINE_MAX_LEN 256

/* Simple length-safe copy: same shape as strlcpy but defined inline
 * so we don't depend on a kernel-internal helper. */
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

/* ---- prompt ---- */

static void print_prompt(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == 0) {
        safe_copy(cwd, "?", sizeof(cwd));
    }
    /* osnos:<cwd>$ — `$` (instead of `>`) to make it visually
     * distinct from the kernel shell while both coexist. */
    printf("\x1b[38;2;0;255;102mshellsrv:%s$\x1b[39m ", cwd);
    fflush(stdout);
}

/* ---- line read ---- */

/* Read one canonical line from stdin into `out`. Returns the line
 * length (>= 0) or -1 on EOF / error. Lines come pre-cooked by the
 * TTY layer: read() blocks until newline, then returns the whole
 * line including '\n'. */
static long read_line(char *out, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        long n = read(0, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') {
            out[pos] = 0;
            return (long)pos;
        }
        if (c == '\b' || c == 0x7f) {
            if (pos > 0) pos--;
            continue;
        }
        out[pos++] = c;
    }
    out[pos] = 0;
    return (long)pos;
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

/* ---- builtins ---- */

static int do_help(int argc, char **argv);
static int do_exit(int argc, char **argv);
static int do_pwd (int argc, char **argv);
static int do_cd  (int argc, char **argv);
static int do_ls  (int argc, char **argv);
static int do_cat (int argc, char **argv);
static int do_echo(int argc, char **argv);

typedef int (*cmd_fn)(int, char **);
typedef struct {
    const char *name;
    cmd_fn      fn;
    const char *help;
} cmd_t;

static const cmd_t COMMANDS[] = {
    { "help", do_help, "list available commands"     },
    { "exit", do_exit, "leave shellsrv"              },
    { "pwd",  do_pwd,  "print working directory"     },
    { "cd",   do_cd,   "change working directory"    },
    { "ls",   do_ls,   "list directory contents"     },
    { "cat",  do_cat,  "print file contents"         },
    { "echo", do_echo, "echo arguments (no escapes)" },
};

static int should_exit = 0;

static int do_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("shellsrv — ring-3 shell skeleton (FASE 10.4 chunk 1)\n");
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        printf("  %-6s — %s\n", COMMANDS[i].name, COMMANDS[i].help);
    }
    return 0;
}

static int do_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    should_exit = 1;
    return 0;
}

static int do_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == 0) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    printf("%s\n", buf);
    return 0;
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
    /* opendir resolves relative paths via libc's cwd cache. */
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
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
    if (argc < 2) {
        fprintf(stderr, "cat: missing argument\n");
        return 1;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cat: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
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

/* ---- dispatch ---- */

static void dispatch(char *line) {
    char *argv[16];
    int argc = split_args(line, argv, 16);
    if (argc == 0) return;

    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(argv[0], COMMANDS[i].name) == 0) {
            (void)COMMANDS[i].fn(argc, argv);
            return;
        }
    }

    /* Not a builtin — try to spawn /bin/<argv[0]> via osn_spawn.
     * For chunk 1, no pipes / redirects. */
    char path[PATH_MAX];
    if (argv[0][0] == '/') {
        safe_copy(path, argv[0], sizeof(path));
    } else {
        safe_copy(path, "/bin/", sizeof(path));
        safe_cat(path, argv[0], sizeof(path));
    }

    /* Reassemble argv into a single args string for proc_execve's
     * "single string of args" convention. */
    char args[256];
    args[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) safe_cat(args, " ", sizeof(args));
        safe_cat(args, argv[i], sizeof(args));
    }

    long pid = osn_spawn(path, args, 0, -1, -1);
    if (pid <= 0) {
        fprintf(stderr, "shellsrv: %s: %s\n",
                argv[0], pid < 0 ? strerror(errno) : "no such command");
        return;
    }
    /* Naive wait: spin yielding via nanosleep until pid disappears
     * from sys_taskinfo. Good enough for chunk 1 — wait()/waitpid()
     * arrive when we add IPC_PROC_EXITED handling in chunk 2+. */
    for (;;) {
        int alive = 0;
        for (size_t i = 0; i < 16; i++) {
            osnos_taskinfo_t info;
            long r = osnos_syscall2(265 /* SYS_TASKINFO */,
                                    (long)i, (long)&info);
            if (r < 0) continue;
            if ((long)info.pid == pid &&
                info.state != OSNOS_TASK_DEAD) {
                alive = 1;
                break;
            }
        }
        if (!alive) break;
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, 0);
    }
}

/* ---- main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\nshellsrv: ring-3 shell skeleton (FASE 10.4 chunk 1)\n");
    printf("type 'help' for commands, 'exit' to return to the parent shell\n");

    char line[LINE_MAX_LEN];
    while (!should_exit) {
        print_prompt();
        long n = read_line(line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;
        dispatch(line);
    }
    printf("shellsrv: bye\n");
    return 0;
}
