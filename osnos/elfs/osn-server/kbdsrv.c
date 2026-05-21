/*
 * elfs/osn-server/kbdsrv.c — Keyboard policy server, ring 3 (FASE 10.2).
 *
 * The kernel keeps a thin PS/2 driver task (src/servers/keyboard_server.c)
 * that polls hardware each tick and pushes raw `keyboard_event_t`
 * records into the /dev/input0 ring. This ELF is the *policy* layer:
 *
 *   1. Drains /dev/input0 in a tight read loop (libc loops on EAGAIN
 *      via nanosleep so we yield to other tasks when idle).
 *   2. For printable chars + ENTER + BACKSPACE: feeds the kernel TTY
 *      line discipline via SYS_TTY_INPUT so canonical mode + ISIG
 *      (Ctrl+C / Ctrl+Z) keep working.
 *   3. For arrow keys: synthesises the VT100 CSI sequences
 *      `ESC [ A/B/C/D` and feeds those — TUI programs in raw mode
 *      (e.g. /bin/ovi) see arrow keys exactly as before.
 *   4. Forwards a copy of every event to the shell as
 *      IPC_KEY_EVENT so the shell's line editor + history-nav
 *      machinery still works.
 *
 * Both paths (TTY + IPC) are fed independently — that's how the old
 * in-kernel keyboard_server worked, so the user-visible behaviour
 * is unchanged.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osnos_ipc.h"
#include "osnos_keys.h"

/* Mirror of the kernel `keyboard_event_t` layout. Default alignment:
 * 1 byte ascii + 1 byte padding + 2 bytes keycode = 4 bytes total.
 * Do NOT mark packed (devfs writes 4 bytes per event). */
typedef struct {
    char     ascii;
    uint16_t keycode;
} kbd_event_t;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int kbd = open("/dev/input0", O_RDONLY);
    if (kbd < 0) return 1;

    if (ipc_service_register(SERVER_KEYBOARD) != 0) {
        close(kbd);
        return 1;
    }

    for (;;) {
        kbd_event_t ev;
        long n = read(kbd, &ev, sizeof(ev));
        if (n != (long)sizeof(ev)) continue;     /* short read — skip */

        /* TTY feed: plain ASCII goes straight through. Arrow keys
         * synthesise CSI sequences so canonical apps + raw-mode TUIs
         * both see them in the standard form. */
        if (ev.ascii != 0 && ev.keycode == 0) {
            ipc_tty_input((unsigned char)ev.ascii);
        } else if (ev.keycode != 0) {
            char final = 0;
            switch (ev.keycode) {
                case OSNOS_KEY_UP:    final = 'A'; break;
                case OSNOS_KEY_DOWN:  final = 'B'; break;
                case OSNOS_KEY_RIGHT: final = 'C'; break;
                case OSNOS_KEY_LEFT:  final = 'D'; break;
            }
            if (final) {
                ipc_tty_input(0x1B);   /* ESC */
                ipc_tty_input('[');
                ipc_tty_input(final);
            }
        }

        /* Shell side: forward as IPC_KEY_EVENT so the line editor +
         * history-nav still see arrow keys / non-canonical input. */
        ipc_msg_t msg;
        msg.from    = 0;             /* kernel overwrites with our pid */
        msg.to      = SERVER_SHELL;
        msg.type    = IPC_KEY_EVENT;
        msg.arg0    = ev.keycode;
        msg.arg1    = 0;
        msg.data[0] = ev.ascii;
        msg.data[1] = 0;
        ipc_send(&msg);
    }
}
