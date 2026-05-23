/*
 * /bin/jqtest — automated jq smoke test.
 *
 * Steps:
 *   1. Write a tiny JSON to /tmp/jqtest.json with known structure.
 *   2. Spawn `jq <filter> /tmp/jqtest.json` for a few filters,
 *      capture stdout via pipe, verify each result.
 *   3. Cleanup.
 *
 * Filter coverage: `.field`, array index, `length`, arithmetic,
 * pipe-chain, `keys`, `string.upper`-equivalent (`ascii_upcase`).
 * That hits the parser, VM, builtins, and JSON output paths.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int fails = 0;

#define CHECK(cond, msg)                                              \
    do {                                                               \
        if (cond) { printf("  PASS %s\n", msg); }                      \
        else { printf("  FAIL %s (errno=%d)\n", msg, errno); fails++; }\
    } while (0)

#define INPUT_PATH "/tmp/jqtest.json"

static const char *INPUT =
    "{\n"
    "  \"name\": \"osnos\",\n"
    "  \"num\": 7,\n"
    "  \"list\": [10, 20, 30, 40]\n"
    "}\n";

/* Spawn `path argv...` with stdout → pipe, capture into out_buf. */
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

/* Trim trailing whitespace/newlines for clean equality checks. */
static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ' ||
                     s[n - 1] == '\r' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static void run_filter(const char *filter, const char *expected,
                        const char *label) {
    char *jq_argv[] = { "jq", "-c", (char *)filter, INPUT_PATH, 0 };
    char out[256];
    int rc = run_capture("/bin/jq", jq_argv, out, sizeof(out));
    rstrip(out);
    if (rc != 0) {
        printf("  FAIL %s: exit=%d output=%s\n", label, rc, out);
        fails++;
        return;
    }
    if (strcmp(out, expected) != 0) {
        printf("  FAIL %s: got=%s expected=%s\n", label, out, expected);
        fails++;
        return;
    }
    printf("  PASS %s\n", label);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("== jqtest: jq 1.7.1 smoke ==\n");

    /* Write the input fixture. */
    mkdir("/tmp", 0755);
    int fd = open(INPUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create /tmp/jqtest.json");
    if (fd >= 0) {
        ssize_t w = write(fd, INPUT, strlen(INPUT));
        CHECK(w == (ssize_t)strlen(INPUT), "write JSON fixture");
        close(fd);
    }

    /* Each call verifies one bit of jq's language + runtime. */
    run_filter(".name",                  "\"osnos\"",         "field access");
    run_filter(".num",                   "7",                 "integer literal");
    run_filter(".list | length",         "4",                 "pipe + length builtin");
    run_filter(".list[2]",               "30",                "array index");
    run_filter(".num * 6",               "42",                "arithmetic");
    run_filter(".name | ascii_upcase",   "\"OSNOS\"",         "ascii_upcase builtin");
    run_filter("[.num, .list[0]] | add", "17",                "ad-hoc array + add");

    unlink(INPUT_PATH);

    printf("jqtest: %d fail(s)\n", fails);
    return fails ? 1 : 0;
}
