/*
 * tests/spawntest.c — exercise SYS_SPAWN (FASE 10.4 prereq).
 *
 * Verifies that a ring-3 task can:
 *   1. Create a pipe.
 *   2. Spawn a child with the pipe write end inherited as stdout.
 *   3. Read the child's output back through the pipe.
 *   4. Observe the IPC roundtrip works without any kernel-resident
 *      shell or fs server (just pure syscalls).
 *
 * This is the proof that the future ring-3 shell can wire its own
 * pipelines via libc + syscalls before we tackle the full shell
 * migration in 10.4.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "osnos_ipc.h"

static int fails = 0;

#define CHECK(cond, name) do {                              \
    if (cond) printf("PASS %s\n", name);                    \
    else     { printf("FAIL %s\n", name); fails++; }        \
} while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Pipe in the parent. */
    int p[2];
    int r = pipe(p);
    CHECK(r == 0, "spawntest: pipe() returns 0");
    if (r != 0) return 1;

    /* Spawn /bin/hello with its stdout wired to the pipe write end.
     * After osn_spawn, p[1] in OUR table is cleared (moved to child).
     * The child writes "hola, mundo\n" and exits. */
    long pid = osn_spawn("/bin/hello", "", 0, -1, p[1]);
    CHECK(pid > 0, "spawntest: osn_spawn(/bin/hello) returns pid");
    if (pid <= 0) { close(p[0]); return 1; }

    /* Read the child's output. read() loops on EAGAIN — and on EOF
     * (writer closed) returns 0. Drain everything until EOF. */
    char buf[64];
    long total = 0;
    for (;;) {
        long n = read(p[0], buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    buf[total] = 0;
    close(p[0]);

    CHECK(total > 0, "spawntest: read child output");
    CHECK(strstr(buf, "hello") != 0, "spawntest: payload contains 'hello'");
    CHECK(strstr(buf, "world") != 0, "spawntest: payload contains 'world'");

    if (fails == 0) {
        printf("\nspawntest: ALL PASS\n");
        return 0;
    }
    printf("\nspawntest: %d FAIL\n", fails);
    return 1;
}
