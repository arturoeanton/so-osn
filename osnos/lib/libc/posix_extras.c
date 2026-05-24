/*
 * posix_extras.c — getopt + stpcpy + popen/pclose.
 *
 * Mini-libc started without estos POSIX/legacy helpers porque ningún
 * ELF antiguo los pedía. Llegan ahora porque pdpmake los usa (getopt
 * para parsing de flags, stpcpy para string building, popen opcional
 * para `$(shell ...)`). Implementación mínima pero correcta.
 */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

/* ----- getopt ----- */

char *optarg = 0;
int optind   = 1;
int opterr   = 1;
int optopt   = 0;

static int getopt_sp = 1;   /* Index dentro de argv[optind] cuando hay
                             * cluster de flags chiquitos (-abc). */

int getopt(int argc, char *const argv[], const char *optstring) {
    /* GNU libc convention: `optind = 0` significa "reset + arranca en
     * argv[1]". Sin esto, pdpmake's GETOPT_RESET() rompe porque
     * dejaría optind apuntando a argv[0] (el program name) y lo
     * tomaríamos como argumento. */
    if (optind == 0) {
        optind = 1;
        getopt_sp = 1;
    }
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' ||
        argv[optind][1] == '\0') {
        getopt_sp = 1;
        return -1;
    }
    if (argv[optind][1] == '-' && argv[optind][2] == '\0') {
        /* `--` separa opciones de positional args. */
        optind++;
        getopt_sp = 1;
        return -1;
    }

    int c = argv[optind][getopt_sp];
    const char *spec = strchr(optstring, c);
    if (c == ':' || !spec) {
        if (opterr) {
            fputs(argv[0], stderr);
            fputs(": illegal option -- ", stderr);
            char buf[2] = { (char)c, 0 };
            fputs(buf, stderr);
            fputc('\n', stderr);
        }
        optopt = c;
        if (argv[optind][++getopt_sp] == '\0') {
            optind++;
            getopt_sp = 1;
        }
        return '?';
    }

    if (spec[1] == ':') {
        /* Esta opción requiere argumento. */
        if (argv[optind][getopt_sp + 1] != '\0') {
            optarg = &argv[optind][getopt_sp + 1];
            optind++;
        } else if (++optind < argc) {
            optarg = argv[optind++];
        } else {
            if (opterr) {
                fputs(argv[0], stderr);
                fputs(": option requires an argument -- ", stderr);
                char buf[2] = { (char)c, 0 };
                fputs(buf, stderr);
                fputc('\n', stderr);
            }
            optopt = c;
            getopt_sp = 1;
            return (optstring[0] == ':') ? ':' : '?';
        }
        getopt_sp = 1;
    } else {
        if (argv[optind][++getopt_sp] == '\0') {
            optind++;
            getopt_sp = 1;
        }
        optarg = 0;
    }
    return c;
}

/* ----- utimensat (stub) ----- */
/* osnos no implementa actualizar atime/mtime de archivos. Apps que
 * usen utimensat (pdpmake `-t` touch mode) reciben 0 sin efecto. */
int utimensat(int dirfd, const char *path, const void *times, int flags) {
    (void)dirfd; (void)path; (void)times; (void)flags;
    return 0;
}

/* ----- stpcpy ----- */
/* `restrict` qualifiers exactly match the standard signature so that
 * clang's __builtin___stpcpy_chk fortify check doesn't flag a
 * "conflicting types" warning. */
char *stpcpy(char *__restrict dst, const char *__restrict src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

/* ----- popen / pclose ----- */

/* Tracking del child pid para que pclose pueda waitpid. Limit chiquito;
 * apps reales usan pocos popen concurrentes. */
#define POPEN_MAX 16
static struct { FILE *f; int pid; } popen_table[POPEN_MAX];

FILE *popen(const char *cmd, const char *mode) {
    extern int fork(void);
    extern int execve(const char *path, char *const argv[], char *const envp[]);
    extern int dup2(int oldfd, int newfd);
    extern int close(int fd);
    extern int pipe(int fds[2]);
    extern char **environ;
    extern void _exit(int code);

    if (!cmd || !mode) return 0;
    int read_end = (mode[0] == 'r');
    if (!read_end && mode[0] != 'w') return 0;

    int fds[2];
    if (pipe(fds) < 0) return 0;
    int pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        /* Child. */
        if (read_end) {
            /* Parent leerá; child escribe a pipe en stdout. */
            dup2(fds[1], 1);
        } else {
            /* Parent escribirá; child lee de pipe en stdin. */
            dup2(fds[0], 0);
        }
        close(fds[0]);
        close(fds[1]);
        char *argv[4] = { (char *)"sh", (char *)"-c", (char *)cmd, 0 };
        execve("/bin/sh", argv, environ);
        _exit(127);
    }
    /* Parent. */
    int my_fd = read_end ? fds[0] : fds[1];
    close(read_end ? fds[1] : fds[0]);
    FILE *f = fdopen(my_fd, read_end ? "r" : "w");
    if (!f) { close(my_fd); return 0; }
    /* Registrar para pclose. */
    for (int i = 0; i < POPEN_MAX; i++) {
        if (!popen_table[i].f) {
            popen_table[i].f = f;
            popen_table[i].pid = pid;
            break;
        }
    }
    return f;
}

int pclose(FILE *f) {
    extern int waitpid(int pid, int *status, int options);
    if (!f) return -1;
    int pid = -1;
    for (int i = 0; i < POPEN_MAX; i++) {
        if (popen_table[i].f == f) {
            pid = popen_table[i].pid;
            popen_table[i].f = 0;
            popen_table[i].pid = 0;
            break;
        }
    }
    fclose(f);
    if (pid < 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return status;
}
