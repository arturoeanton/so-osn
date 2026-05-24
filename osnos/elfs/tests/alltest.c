/*
 * alltest — run the entire osnos test battery, summarise at the end.
 *
 * For each test in `tests[]`, alltest forks a child, execves the
 * corresponding /bin/<test> ELF, and waitpids on its exit. The
 * child's exit code becomes the per-test status (0 = PASS, !=0 =
 * FAIL). Children print their normal output to stdout interleaved
 * with alltest's section headers.
 *
 * At the end alltest prints a banner-delimited summary so the
 * results are the last thing on the framebuffer — no scrolling
 * required to see the totals. Failed tests are also listed
 * separately at the bottom for quick triage.
 *
 * Usage:
 *   shellsrv:/$ alltest
 *
 * Returns 0 if every test passed, 1 if anything failed.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    const char *name;       /* short label for the summary */
    const char *path;       /* full /bin/... path */
    char *const *argv;      /* { "name", NULL } typically */
} test_t;

/* Per-test argv arrays (must be mutable arrays of pointers since
 * execve takes char *const *). The strings themselves are literals. */
static char *kerntest_argv   [] = {"kerntest",    0};
static char *forktest_argv   [] = {"forktest",    0};
static char *waittest_argv   [] = {"waittest",    0};
static char *sigtest_argv    [] = {"sigtest",     0};
static char *spawntest_argv  [] = {"spawntest",   0};
static char *sigchldtest_argv[] = {"sigchldtest", 0};
static char *pgrouptest_argv [] = {"pgrouptest",  0};
static char *exectest_argv   [] = {"exectest",    0};
static char *ofdtest_argv    [] = {"ofdtest",     0};
static char *ptytest_argv    [] = {"ptytest",     0};
static char *fdedgetest_argv [] = {"fdedgetest",  0};
static char *jobtest_argv    [] = {"jobtest",     0};
static char *termtest_argv   [] = {"termtest",    0};
static char *serialtest_argv [] = {"serialtest",  0};
static char *tcctest_argv    [] = {"tcctest",     0};
static char *luatest_argv    [] = {"luatest",     0};
static char *jqtest_argv     [] = {"jqtest",      0};
static char *libctest_argv   [] = {"libctest",    0};
/* FASE 14 — infra moderna. */
static char *unixtest_argv   [] = {"unixtest",    0};   /* AF_UNIX sockets */
static char *shmtest_argv    [] = {"shmtest",     0};   /* POSIX SHM + mmap MAP_SHARED */
static char *hello_dyn_argv  [] = {"hello_dyn",   0};   /* Dynamic linking via ld-musl */

static test_t tests[] = {
    /* Order roughly by dependency: kernel ABI first (kerntest),
     * then process lifecycle (fork/wait/spawn), signals, exec,
     * libc surface last. */
    {"kerntest",    "/bin/kerntest",    kerntest_argv   },
    {"forktest",    "/bin/forktest",    forktest_argv   },
    {"waittest",    "/bin/waittest",    waittest_argv   },
    {"sigtest",     "/bin/sigtest",     sigtest_argv    },
    {"sigchldtest", "/bin/sigchldtest", sigchldtest_argv},
    {"pgrouptest",  "/bin/pgrouptest",  pgrouptest_argv },
    {"spawntest",   "/bin/spawntest",   spawntest_argv  },
    {"exectest",    "/bin/exectest",    exectest_argv   },
    {"ofdtest",     "/bin/ofdtest",     ofdtest_argv    },
    {"ptytest",     "/bin/ptytest",     ptytest_argv    },
    {"fdedgetest",  "/bin/fdedgetest",  fdedgetest_argv },
    {"jobtest",     "/bin/jobtest",     jobtest_argv    },
    {"termtest",    "/bin/termtest",    termtest_argv   },
    {"serialtest",  "/bin/serialtest",  serialtest_argv },
    {"tcctest",     "/bin/tcctest",     tcctest_argv    },
    {"luatest",     "/bin/luatest",     luatest_argv    },
    {"jqtest",      "/bin/jqtest",      jqtest_argv     },
    {"libctest",    "/bin/libctest",    libctest_argv   },
    /* FASE 14 — infraestructura moderna (POSIX IPC + dynamic linking). */
    {"unixtest",    "/bin/unixtest",    unixtest_argv   },
    {"shmtest",     "/bin/shmtest",     shmtest_argv    },
    {"hello_dyn",   "/bin/hello_dyn",   hello_dyn_argv  },
};

#define N_TESTS ((int)(sizeof(tests) / sizeof(tests[0])))

static void rule(void) {
    /* 56-char rule for visual separation; matches kerntest banner. */
    printf("========================================================\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int results[N_TESTS];
    for (int i = 0; i < N_TESTS; i++) results[i] = -1;

    rule();
    printf("alltest: running osnos test battery (%d tests)\n", N_TESTS);
    rule();
    fflush(stdout);

    for (int i = 0; i < N_TESTS; i++) {
        printf("\n");
        rule();
        printf("[%d/%d] %s\n", i + 1, N_TESTS, tests[i].name);
        rule();
        fflush(stdout);

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr,
                    "alltest: fork failed for %s (errno=%d)\n",
                    tests[i].name, errno);
            results[i] = -1;
            continue;
        }
        if (pid == 0) {
            /* Child: become a session leader before exec so each
             * test sees a clean job-control state (pgid == sid == pid).
             *
             * Without this, sys_fork inherits alltest's pgid/sid,
             * and tests like /bin/pgrouptest that assume "I'm
             * top-level → my pgid == my pid" would fail.
             *
             * setsid() is allowed because after fork the child is
             * NOT a pgrp leader (it has its own fresh pid, but pgid
             * still points at alltest's pid). POSIX permits setsid
             * from any non-leader. */
            setsid();
            execve(tests[i].path, tests[i].argv, environ);
            fprintf(stderr,
                    "alltest: execve %s failed: errno=%d\n",
                    tests[i].path, errno);
            _exit(127);
        }

        /* Poll-based wait con timeout. Sin esto, un test que se cuelga
         * (read bloqueante sin EOF, loop infinito, etc) bloquea TODA
         * la suite. 60 segundos es suficiente para los tests más
         * lentos (tcctest, jqtest tardan ~10s en QEMU). */
        int status   = 0;
        pid_t r      = -1;
        int   t_max  = 60 * 10;  /* 60s × 100ms ticks */
        for (int t = 0; t < t_max; t++) {
            r = waitpid(pid, &status, 1 /* WNOHANG */);
            if (r == pid) break;
            if (r < 0)    { results[i] = -1; break; }
            usleep(100000); /* 100 ms */
        }
        if (r != pid) {
            /* Timeout — kill and reap. Marker -2 = TIMEOUT en summary. */
            fprintf(stderr,
                    "alltest: %s TIMEOUT after 60s — killing\n",
                    tests[i].name);
            kill(pid, 9);
            waitpid(pid, &status, 0);
            results[i] = -2;
        } else if (r == pid) {
            if (WIFEXITED(status)) {
                results[i] = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                /* Signal-killed → reflect as 128 + signum (POSIX
                 * convention; matches what shellsrv shows for kill 9). */
                results[i] = 128 + WTERMSIG(status);
            } else {
                results[i] = -1;   /* shouldn't happen */
            }
        }
    }

    /* ---- final summary, banner-delimited so it's visually
     *      distinct from per-test output ---- */
    int passed = 0;
    int failed = 0;
    int errored = 0;
    for (int i = 0; i < N_TESTS; i++) {
        if      (results[i] == 0)  passed++;
        else if (results[i] < 0)   errored++;
        else                       failed++;
    }

    printf("\n\n");
    rule();
    printf("ALLTEST SUMMARY\n");
    rule();
    for (int i = 0; i < N_TESTS; i++) {
        const char *tag;
        if      (results[i] ==  0) tag = "PASS   ";
        else if (results[i] == -2) tag = "TIMEOUT";
        else if (results[i] <   0) tag = "ERROR  ";
        else                       tag = "FAIL   ";
        printf("  %s  %-14s  (exit=%d)\n",
               tag, tests[i].name, results[i]);
    }
    rule();
    printf("RESULT: %d/%d passed", passed, N_TESTS);
    if (failed > 0)  printf(", %d failed", failed);
    if (errored > 0) printf(", %d errored", errored);
    printf("\n");
    rule();

    if (failed > 0 || errored > 0) {
        printf("\nFAILS TO INVESTIGATE:\n");
        for (int i = 0; i < N_TESTS; i++) {
            if (results[i] != 0) {
                printf("  - %-14s  exit=%d%s\n",
                       tests[i].name,
                       results[i],
                       results[i] < 0 ? " (couldn't run)" : "");
            }
        }
        printf("\n");
    }

    return (failed > 0 || errored > 0) ? 1 : 0;
}
