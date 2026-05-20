#pragma once

#include "vfs.h"

/*
 * Synthetic read-only filesystem exposing kernel state.
 *
 * Mount at "/sys". Reads materialize their content on demand from kernel
 * tables (task list, mount list, version). No storage. All writes /
 * mkdir / unlink return OSNOS_EROFS.
 *
 * Files exposed today:
 *   /sys/version  -- kernel version string
 *   /sys/tasks    -- task table (pid, name, state)
 *   /sys/mounts   -- vfs mount table
 */
extern const vfs_ops_t sysfs_vfs_ops;
