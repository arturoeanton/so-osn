/*
 * exectest — verify Linux execve(2) semantics on osnos.
 *
 * Flow:
 *   1. Print "before" + remember our pid.
 *   2. Open a file we'll keep across the exec to prove fds survive.
 *   3. execve("/bin/exectest2", ["exectest2", <our_pid>, <fd_str>]).
 *      Since /bin/exectest2 doesn't exist, the exec FAILS — we land
 *      after the call with errno set. Print the error.
 *   4. Then execve("/bin/echo", ["echo", "exec ok pid=...", ...]).
 *      Success: never returns. /bin/echo runs in the same pid+fds.
 *
 * Manual verification: see "exec ok pid=N" in shell, where N matches
 * what was printed BEFORE the exec. Same pid = execve worked.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    long before_pid = (long)getpid();
    printf("exectest: before exec, pid=%ld\n", before_pid);

    /* Open a sentinel file — execve(2) preserves fds. After execve
     * succeeds, the new program inherits this fd open. We pass its
     * number as argv to the next program so it can dup/read it. */
    int sentinel = open("/home/HELLO.TXT", O_RDONLY);
    printf("exectest: opened /home/HELLO.TXT as fd=%d\n", sentinel);

    /* First exec: a nonexistent program — should fail with ENOENT
     * and we recover. Tests the failure path of execve preserves
     * the old image. */
    char *fail_argv[] = { "noexisto", 0 };
    execve("/bin/__does_not_exist__", fail_argv, environ);
    printf("exectest: execve missing → errno=%d (expected ENOENT=2)\n", errno);

    /* Second exec: hand off to /bin/echo with a message embedding our
     * pid. If execve succeeds, /bin/echo prints the message and exits;
     * we never return. */
    char pid_msg[64];
    snprintf(pid_msg, sizeof(pid_msg),
             "exec ok pid=%ld (same as before? compare with %ld)",
             before_pid, before_pid);

    char *echo_argv[] = { "echo", pid_msg, 0 };
    execve("/bin/echo", echo_argv, environ);

    /* If we get here, the second exec also failed. */
    fprintf(stderr, "exectest: execve /bin/echo failed, errno=%d\n", errno);
    if (sentinel >= 0) close(sentinel);
    return 1;
}
