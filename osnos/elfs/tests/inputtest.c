/*
 * tests/inputtest.c — read raw keyboard events from /dev/input0
 * (FASE 10.0.c). Used to prove that a ring-3 task can consume
 * keystrokes via an fd interface (precursor to the ring-3 kbdsrv).
 *
 * Reads N=5 events then exits so the shell prompt comes back without
 * Ctrl+C. Each event is the kernel-side `keyboard_event_t` layout:
 *   byte 0     = ascii
 *   bytes 1-2  = keycode (uint16_t LE, Linux KEY_*)
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Layout MUST mirror kernel-side `keyboard_event_t` exactly. The
 * kernel struct uses default x86_64 alignment (char + 1-byte pad +
 * uint16_t = 4 bytes). Do NOT add `packed` here — devfs writes 4
 * bytes per event and a 3-byte read drifts on every iteration. */
typedef struct {
    char     ascii;
    uint16_t keycode;
} kbd_event_t;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("/dev/input0", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "inputtest: cannot open /dev/input0\n");
        return 1;
    }

    printf("inputtest: press 5 keys (events read from /dev/input0)\n");

    for (int i = 0; i < 5; i++) {
        kbd_event_t ev;
        long n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) {
            fprintf(stderr, "inputtest: short read (%ld)\n", n);
            close(fd);
            return 1;
        }
        printf("  ev %d: ascii=%d (0x%02x) keycode=%d\n",
               i + 1, (int)(unsigned char)ev.ascii,
               (int)(unsigned char)ev.ascii,
               (int)ev.keycode);
    }

    close(fd);
    printf("inputtest: done\n");
    return 0;
}
