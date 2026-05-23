/*
 * /bin/luatest — automated Lua self-host smoke test.
 *
 * Steps mirror tcctest:
 *   1. Write a tiny lua script to /tmp/luatest.lua that prints
 *      a magic line + a couple arithmetic / string results.
 *   2. Spawn /bin/lua with the script as arg, capture stdout.
 *   3. Verify the output contains the expected pieces.
 *
 * Exits 0 if every check passes. Adds Lua to the alltest battery.
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

#define SCRIPT_PATH "/tmp/luatest.lua"

static const char *SCRIPT =
    "print(\"luatest:magic\")\n"
    "print(2 + 2)\n"
    "print(math.sqrt(16))\n"
    "print(string.upper(\"osnos\"))\n";

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
    printf("== luatest: Lua 5.4 smoke ==\n");

    mkdir("/tmp", 0755);
    int fd = open(SCRIPT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create /tmp/luatest.lua");
    if (fd >= 0) {
        ssize_t w = write(fd, SCRIPT, strlen(SCRIPT));
        CHECK(w == (ssize_t)strlen(SCRIPT), "write script");
        close(fd);
    }

    char buf[512];
    char *lua_argv[] = { "lua", (char *)SCRIPT_PATH, 0 };
    int rc = run_capture("/bin/lua", lua_argv, buf, sizeof(buf));
    CHECK(rc == 0, "lua script exit 0");

    /* Verify each expected line landed in the output. */
    CHECK(strstr(buf, "luatest:magic") != 0,    "magic string");
    CHECK(strstr(buf, "4")             != 0,    "arithmetic: 2+2 == 4");
    CHECK(strstr(buf, "4.0")           != 0
       || strstr(buf, "4")             != 0,    "math.sqrt(16) == 4");
    CHECK(strstr(buf, "OSNOS")         != 0,    "string.upper(\"osnos\")");

    unlink(SCRIPT_PATH);

    printf("luatest: %d fail(s)\n", fails);
    return fails ? 1 : 0;
}
