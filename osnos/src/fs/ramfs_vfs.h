#pragma once

#include "vfs.h"

/*
 * VFS adapter for the ramfs backend.
 *
 * Wraps the existing ramfs_* path-based API into the vfs_ops_t contract so
 * that vfs_mount(..., &ramfs_vfs_ops, ...) can plug ramfs into the VFS
 * layer. The original ramfs interface is unchanged — fs_server can still
 * call it directly during the migration window.
 *
 * `priv` is unused (passed as 0 to vfs_mount). Ramfs has a single global
 * slot table; a per-mount instance would require a refactor that's not
 * needed yet.
 */
extern const vfs_ops_t ramfs_vfs_ops;
