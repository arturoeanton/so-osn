#include "mouse_server.h"

#include "../drivers/mouse.h"
#include "../fs/devfs.h"

void mouse_server_init(void) {
    mouse_init();
}

void mouse_server_tick(void) {
    /* Each scheduler tick: drain whatever the AUX FIFO has. The
     * driver poll consumes one byte per call; pull a handful per
     * tick so a busy mouse doesn't lag behind. Bounded so a wedged
     * controller can't starve everyone. */
    for (int i = 0; i < 16; i++) {
        mouse_event_t ev;
        if (!mouse_poll(&ev)) break;
        devfs_mouse_push(ev);
    }
}
