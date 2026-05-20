#pragma once

#include "vfs.h"

/*
 * VFS adapter over fat.c. Mounted at /sd in bootstrap_fs(). Read-only
 * in FASE 8.3 — every write-side op returns OSNOS_EROFS until 8.4 wires
 * fat_write / fat_create / fat_unlink underneath.
 *
 * The adapter strips the "/sd" mount prefix before delegating to the
 * FAT layer (which works with paths relative to the volume root).
 */
extern const vfs_ops_t fat_vfs_ops;
