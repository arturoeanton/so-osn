#pragma once

#include "vfs.h"

/*
 * Alias filesystem ("bind mount" in Linux terms).
 *
 * Translates every operation on its mountpoint into an equivalent
 * operation against a different absolute path, then dispatches it
 * back through the VFS. The target path can live in any other mount
 * (e.g. /home → /sd/home dispatches to the FAT backend, but the
 * caller's namespace shows it as /home).
 *
 *   aliasfs_init("/home", "/sd/home", &slot);
 *   vfs_mount("/home", &aliasfs_ops, &slot);
 *
 * After that vfs_read("/home/README.TXT") behaves identically to
 * vfs_read("/sd/home/README.TXT") for any backend living under /sd.
 *
 * Restrictions:
 *   - The alias is set up once at bootstrap_fs() time. No runtime
 *     re-binding.
 *   - The target must not point back at this mount (no cycles); the
 *     bootstrap is responsible for picking sane pairs.
 */

typedef struct {
    char mount_prefix[OSNOS_PATH_MAX];  /* e.g. "/home"     */
    char target_prefix[OSNOS_PATH_MAX]; /* e.g. "/sd/home"  */
    size_t mount_len;
    size_t target_len;
} aliasfs_t;

extern const vfs_ops_t aliasfs_ops;

/* Configure `slot` with the (mountpoint, target) pair. Returns true on
 * success, false if either path is empty or too long. */
bool aliasfs_init(aliasfs_t *slot,
                   const char *mount_prefix,
                   const char *target_prefix);
