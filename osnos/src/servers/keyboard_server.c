#include "keyboard_server.h"

#include "../drivers/keyboard.h"
#include "../fs/devfs.h"

/*
 * Kernel-side keyboard tick — minimal hardware feeder (FASE 10.2+).
 *
 * Before FASE 10.2 this task also performed TTY line-discipline feed
 * (`tty_input`) and IPC_KEY_EVENT fan-out to the shell. Both of those
 * responsibilities moved to the ring-3 `kbdsrv` ELF. The kernel keeps
 * only the bits that touch hardware:
 *   1. keyboard_init at boot.
 *   2. Each tick: drain the PS/2 driver via `keyboard_poll` and push
 *      events into the /dev/input0 ring buffer (devfs_input_push).
 *
 * kbdsrv reads /dev/input0, calls sys_tty_input + ipc_send to do the
 * dispatch policy in user-mode. When FASE 11 makes the PS/2 driver
 * IRQ-driven from ring 3, this kernel feeder finally goes away.
 */
void keyboard_server_init(void) {
    keyboard_init();
}

void keyboard_server_tick(void) {
    keyboard_event_t ev;
    if (!keyboard_poll(&ev)) return;
    devfs_input_push(ev);
}
