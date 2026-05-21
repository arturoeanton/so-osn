/*
 * tests/top.c — live task viewer.
 *
 *   top
 *
 * Every second:
 *   - clear screen (ANSI \033[2J\033[H)
 *   - render a header (own pid + uptime)
 *   - dump /sys/tasks
 *   - dump /sys/mem (compact)
 * Exit with Ctrl+C (shell forwards kill_pending; the kernel terminates
 * us at the next return-to-user boundary — usually inside sleep()).
 *
 * This is meant as the visible demo of FASE 9: timer-driven preemption
 * keeps the shell responsive even while a runaway loop runs in another
 * background task. Launch e.g.
 *
 *     exec /bin/osh -e "while 1==1 { x = 1 }" &
 *     exec /bin/top
 *
 * and watch the runaway osh's `dispatches` counter climb while the rest
 * of the system stays alive.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int dump_file(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    char buf[2048];
    for (;;) {
        long n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        write(STDOUT_FILENO, buf, (size_t)n);
        if (n < (long)(sizeof(buf) - 1)) break;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    pid_t self = getpid();

    for (;;) {
        /* ANSI clear + home. Framebuffer driver knows ESC[2J / ESC[H. */
        write(STDOUT_FILENO, "\033[2J\033[H", 7);

        printf("osnos top  (pid=%d, Ctrl+C to exit)\n", (int)self);
        printf("---------------------------------------\n");

        if (dump_file("/sys/uptime") < 0) {
            printf("(uptime unavailable)\n");
        }
        printf("\n");

        if (dump_file("/sys/tasks") < 0) {
            printf("(tasks unavailable)\n");
        }
        printf("\n");

        if (dump_file("/sys/mem") < 0) {
            printf("(mem unavailable)\n");
        }

        sleep(1);
    }
    return 0;
}
