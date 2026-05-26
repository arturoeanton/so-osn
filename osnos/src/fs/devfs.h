#pragma once

#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "vfs.h"

/*
 * Device filesystem at "/dev". Exposes synthetic character devices.
 *
 * Today:
 *   /dev/null    -- writes discarded, reads return 0 bytes (EOF)
 *   /dev/zero    -- reads fill buffer with 0x00, writes discarded
 *   /dev/fb0     -- writes go to the framebuffer, reads return EOF
 *   /dev/input0  -- reads return raw keyboard_event_t (4 bytes each)
 *
 * The dir itself is read-only (mkdir / rmdir / unlink / rename -> EROFS).
 * Device read/write semantics are per-device.
 */
extern const vfs_ops_t devfs_vfs_ops;

/*
 * Push a fresh keyboard event into the /dev/input0 ring. Called by
 * the kernel keyboard_server every tick alongside the existing TTY
 * + IPC fan-out, so /dev/input0 readers see every keystroke without
 * stealing it from the legacy path. When FASE 10.2 moves the kbd
 * server to ring 3, this becomes the only feeder.
 */
void devfs_input_push(keyboard_event_t ev);

/* Push one decoded mouse packet into the /dev/mouse0 ring. Same
 * lifecycle as devfs_input_push: kernel mouse_server polls the PS/2
 * controller each tick, decodes into mouse_event_t, calls this. */
void devfs_mouse_push(mouse_event_t ev);

/* Readability peeks — true iff the device's ring has at least one
 * complete event queued. Used by sys_poll so /dev/mouse0 and
 * /dev/input0 don't always report readable (which would busy-spin
 * clients waiting on poll). */
bool devfs_mouse_has_data(void);
bool devfs_input_has_data(void);
