/*
 * /bin/mousetest — read /dev/mouse0 and print events.
 *
 * Quick interactive smoke test for FASE 11.4. Move the host mouse
 * over the QEMU window, click buttons, and watch dx/dy/buttons.
 * Maintains an accumulated absolute (x, y) for convenience but
 * doesn't clamp to screen — that's the eventual cursor demo's job.
 *
 * Exit with Ctrl+C (default SIGINT terminates the task).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mouse.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("/dev/mouse0", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "mousetest: open /dev/mouse0: errno=%d\n", errno);
        return 1;
    }

    printf("mousetest: move the mouse, press buttons, Ctrl+C to exit.\n");
    printf("           (QEMU usually grabs the cursor on first click —\n");
    printf("            Ctrl+Alt+G to release on Linux, or use cocoa display on mac.)\n");

    long x = 0, y = 0;
    int prev_buttons = -1;
    for (;;) {
        mouse_event_t ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (errno == EAGAIN) {
                /* Block briefly so we don't spin the CPU when idle. */
                struct timespec ts = { 0, 20 * 1000000 };  /* 20 ms */
                nanosleep(&ts, 0);
                continue;
            }
            fprintf(stderr, "mousetest: read: errno=%d\n", errno);
            return 1;
        }
        x += ev.dx;
        y += ev.dy;
        int show = (ev.dx || ev.dy || ev.buttons != prev_buttons);
        if (show) {
            printf("dx=%+4d dy=%+4d  abs=(%5ld,%5ld)  L=%d M=%d R=%d\n",
                   ev.dx, ev.dy, x, y,
                   (ev.buttons & MOUSE_BTN_LEFT)   ? 1 : 0,
                   (ev.buttons & MOUSE_BTN_MIDDLE) ? 1 : 0,
                   (ev.buttons & MOUSE_BTN_RIGHT)  ? 1 : 0);
        }
        prev_buttons = ev.buttons;
    }
    /* unreachable */
}
