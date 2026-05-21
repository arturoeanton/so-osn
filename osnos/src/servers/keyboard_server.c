#include "keyboard_server.h"

#include "../drivers/keyboard.h"
#include "../fs/devfs.h"
#include "../include/osnos_keys.h"
#include "../micro/ipc.h"
#include "../micro/tty.h"

void keyboard_server_init(void) {
    keyboard_init();
}

void keyboard_server_tick(void) {
    keyboard_event_t ev;

    if (!keyboard_poll(&ev)) {
        return;
    }

    /* Fan out the raw event to the /dev/input0 ring so user tasks
     * reading from that fd see every keystroke too. The TTY +
     * IPC_KEY_EVENT fan-out below is unchanged — both paths are
     * fed independently. */
    devfs_input_push(ev);

    /*
     * Feed printable chars + newline + backspace into the TTY line
     * discipline (canonical edit / signals / echo per termios).
     * Arrow keys become 3-byte VT100 sequences (ESC [ A/B/C/D) so
     * TUI programs in raw mode (e.g. /bin/ovi) receive them.
     */
    if (ev.ascii != 0 && ev.keycode == 0) {
        tty_input((char)ev.ascii);
    } else if (ev.keycode != 0) {
        char final = 0;
        switch (ev.keycode) {
            case OSNOS_KEY_UP:    final = 'A'; break;
            case OSNOS_KEY_DOWN:  final = 'B'; break;
            case OSNOS_KEY_RIGHT: final = 'C'; break;
            case OSNOS_KEY_LEFT:  final = 'D'; break;
        }
        if (final) {
            tty_input(0x1B);
            tty_input('[');
            tty_input(final);
        }
    }

    ipc_msg_t msg;
    msg.from = SERVER_KEYBOARD;
    msg.to = SERVER_SHELL;
    msg.type = IPC_KEY_EVENT;
    msg.arg0 = ev.keycode;
    msg.arg1 = 0;
    msg.data[0] = ev.ascii;
    msg.data[1] = 0;

    ipc_send(&msg);
}
