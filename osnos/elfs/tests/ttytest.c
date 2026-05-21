/*
 * /bin/ttytest — demo FASE TTY 1+2: cycle between canonical and raw
 * mode, read input both ways, restore the original termios on exit.
 *
 *   exec /bin/ttytest
 *
 * Stage 1 (canonical, default): asks for a line. Echo handled by the
 * kernel TTY since the shell is blocked. read(0) returns once you
 * press Enter.
 *
 * Stage 2 (raw): same prompt, but each keystroke comes through as
 * one byte. We loop reading single chars until 'q'. ECHO is off so
 * the keys don't appear on screen unless you press a printable one
 * and we echo it ourselves.
 *
 * Stage 3: restore termios. Ctrl+C while in raw works because ISIG
 * stayed enabled (and we set kill_pending → bye).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct termios saved, t;
    if (tcgetattr(0, &saved) != 0) {
        printf("tcgetattr failed: errno=%d\n", errno);
        return 1;
    }

    printf("== ttytest ==\n");
    printf("c_lflag=0x%x  c_iflag=0x%x\n",
           (unsigned)saved.c_lflag, (unsigned)saved.c_iflag);

    /* Stage 1: canonical (already on by default). */
    printf("\n[1] canonical mode. Type a line + Enter:\n> ");
    char line[128];
    ssize_t n = read(0, line, sizeof(line) - 1);
    if (n < 0) { printf("read: errno=%d\n", errno); return 1; }
    line[n] = 0;
    /* Trim trailing newline for the echo. */
    if (n > 0 && line[n - 1] == '\n') { line[n - 1] = 0; n--; }
    printf("got %zd bytes: '%s'\n", n, line);

    /* Stage 2: raw — ICANON off, ECHO off. */
    t = saved;
    t.c_lflag &= ~(ICANON | ECHO);
    /* VMIN=1, VTIME=0 → read returns as soon as one byte arrives. */
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) {
        printf("tcsetattr raw failed: errno=%d\n", errno);
        return 1;
    }

    printf("\n[2] raw mode. press keys; 'q' to exit, Ctrl+C kills:\n");
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0) { printf("read raw: errno=%d\n", errno); break; }
        if (r == 0) continue;
        if (c == 'q' || c == 'Q') {
            printf("\nquit key.\n");
            break;
        }
        /* Show what arrived, with control bytes hexed. */
        if (c >= ' ' && c < 0x7f) {
            printf("[%c]", c);
        } else {
            printf("<0x%02x>", (unsigned)(unsigned char)c);
        }
    }

    /* Stage 3: restore. */
    if (tcsetattr(0, TCSANOW, &saved) != 0) {
        printf("tcsetattr restore failed: errno=%d\n", errno);
    }
    printf("\nttytest: termios restored. bye.\n");
    return 0;
}
