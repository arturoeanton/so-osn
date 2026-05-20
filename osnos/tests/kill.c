/*
 * tests/kill.c — minimal kill(1).
 *
 *   kill PID
 *
 * Marks the target task's kill_pending flag via sys_kill. The kernel
 * delivers the kill at the next user-mode return point (syscall or
 * timer IRQ), terminating the task with exit code 130 (128 + SIGINT).
 *
 * Signal number is hardcoded to 9 (SIGKILL on Linux), but osnos
 * ignores it today — every kill is "the same kind". Returns 0 on
 * success, 1 on bad usage or ESRCH.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill PID\n");
        return 1;
    }
    int target = atoi(argv[1]);
    if (target <= 0) {
        fprintf(stderr, "kill: invalid pid\n");
        return 1;
    }
    if (kill((pid_t)target, 9) < 0) {
        fprintf(stderr, "kill: no such pid\n");
        return 1;
    }
    return 0;
}
