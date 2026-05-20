#include "keyboard_server.h"

#include "../drivers/keyboard.h"
#include "../micro/fd.h"
#include "../micro/ipc.h"

void keyboard_server_init(void) {
    keyboard_init();
}

void keyboard_server_tick(void) {
    keyboard_event_t ev;

    if (!keyboard_poll(&ev)) {
        return;
    }

    /*
     * Push printable chars + newline + backspace into the stdin ring
     * buffer for sys_read(0). Special keys (arrows, ctrl combos) skip
     * stdin since their semantics aren't ASCII.
     */
    if (ev.ascii != 0 && ev.keycode == 0) {
        stdin_push(ev.ascii);
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
