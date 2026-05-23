/*
 * /bin/tcctest — automated TCC self-hosting smoke test.
 *
 * Steps:
 *   1. Write a minimal C program to /tmp/tcctest.c.
 *   2. Spawn /bin/tcc to compile it into /tmp/tcctest.out.
 *   3. Spawn /tmp/tcctest.out with stdout piped back.
 *   4. Verify the child printed the expected magic string and
 *      returned exit code 0.
 *
 * Failure on any step → FAIL with the offending stage logged.
 * Exits 0 if every check passes.
 *
 * Note: this is the end-to-end happy-path test. TCC's correctness
 * on edge cases (FP literals, structs, includes, etc.) is upstream
 * and not our concern — we test that the *port* works.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails = 0;

#define CHECK(cond, msg)                                            \
    do {                                                             \
        if (cond) { printf("  PASS %s\n", msg); }                    \
        else { printf("  FAIL %s (errno=%d)\n", msg, errno); fails++; } \
    } while (0)

extern char **environ;

#define SRC_PATH "/tmp/tcctest.c"
#define BIN_PATH "/tmp/tcctest.out"
#define MAGIC    "tcctest:42"

static const char *PROGRAM_SRC =
    "#include <stdio.h>\n"
    "int main(void) {\n"
    "    printf(\"" MAGIC "\\n\");\n"
    "    return 0;\n"
    "}\n";

/* Spawn `path argv...` synchronously with the child's stdout
 * connected to a pipe; capture into `out_buf` (cap-1 bytes, NUL-
 * terminated). Returns child exit code, or -1 on spawn failure. */
static int run_capture(const char *path, char *const argv[],
                        char *out_buf, size_t out_cap) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    int pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        execve(path, argv, environ);
        _exit(127);
    }
    close(fds[1]);

    size_t n = 0;
    while (n + 1 < out_cap) {
        ssize_t r = read(fds[0], out_buf + n, out_cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out_buf[n] = 0;
    close(fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("== tcctest: TCC self-host end-to-end ==\n");

    /* 1. Write the source. */
    mkdir("/tmp", 0755);                /* idempotent */
    int fd = open(SRC_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create /tmp/tcctest.c");
    if (fd >= 0) {
        ssize_t w = write(fd, PROGRAM_SRC, strlen(PROGRAM_SRC));
        CHECK(w == (ssize_t)strlen(PROGRAM_SRC), "write source");
        close(fd);
    }

    /* 2. tcc /tmp/tcctest.c -o /tmp/tcctest.out  */
    {
        char *tcc_argv[] = {
            "tcc", (char *)SRC_PATH, "-o", (char *)BIN_PATH, 0
        };
        char dummy[64];
        int rc = run_capture("/bin/tcc", tcc_argv, dummy, sizeof(dummy));
        CHECK(rc == 0, "tcc compiles cleanly");
    }

    /* 3. /tmp/tcctest.out  — capture stdout. */
    char buf[128];
    {
        char *bin_argv[] = { (char *)BIN_PATH, 0 };
        int rc = run_capture(BIN_PATH, bin_argv, buf, sizeof(buf));
        CHECK(rc == 0, "tcc-built binary exit 0");
    }

    /* 4. The binary's stdout must contain our magic line. */
    CHECK(strstr(buf, MAGIC) != 0, "tcc-built binary printed magic");

    /* Cleanup (best-effort — don't fail the test if unlink misses). */
    unlink(SRC_PATH);
    unlink(BIN_PATH);

    printf("tcctest: %d fail(s)\n", fails);
    return fails ? 1 : 0;
}
