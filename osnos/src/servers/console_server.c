#include "console_server.h"

#include "../drivers/framebuffer.h"
#include "../micro/ipc.h"

#define CONSOLE_COLOR_NORMAL 0xffffff
#define CONSOLE_COLOR_GREEN  0x00ff66

void console_server_init(void) {
    console_clear();
}

void console_clear(void) {
    framebuffer_clear(0x000000);
}

void console_write(const char *s) {
    framebuffer_draw_string(s, CONSOLE_COLOR_NORMAL);
}

void console_write_color(
    const char *s,
    uint32_t color
) {
    framebuffer_draw_string(s, color);
}

void console_server_tick(void) {
    ipc_msg_t msg;

    while (ipc_recv(SERVER_CONSOLE, &msg)) {

        if (msg.type == IPC_CONSOLE_WRITE) {
            framebuffer_draw_string(
                msg.data,
                (uint32_t)msg.arg0
            );
        }

        if (msg.type == IPC_CONSOLE_CLEAR) {
            framebuffer_clear(0x000000);
        }
    }
}

void console_backspace(void) {
    framebuffer_backspace();
}
