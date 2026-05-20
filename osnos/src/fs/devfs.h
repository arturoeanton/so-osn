#pragma once

#include "vfs.h"

/*
 * Device filesystem at "/dev". Exposes synthetic character devices.
 *
 * Today:
 *   /dev/null   -- writes discarded, reads return 0 bytes (EOF)
 *   /dev/zero   -- reads fill buffer with 0x00, writes discarded
 *
 * The dir itself is read-only (mkdir / rmdir / unlink / rename -> EROFS).
 * Device read/write semantics are per-device.
 */
extern const vfs_ops_t devfs_vfs_ops;
