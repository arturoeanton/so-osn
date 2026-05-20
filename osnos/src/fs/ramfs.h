#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../include/osnos_limits.h"

/*
 * Tiny RAM filesystem. Flat array of slots; each slot is either a file or a
 * directory and stores its full path in `name` (e.g. "/home/README.TXT").
 * Designed as a backend for the future VFS layer (FASE 2 of ROADMAP).
 *
 * Invariants:
 *   - A slot's index never changes for the life of its entry. Deletion marks
 *     `used = false` and clears fields; the slot is reusable but pointers
 *     returned by ramfs_find() to a different slot remain valid.
 *   - ramfs_find() / ramfs_list_dir() / etc. return pointers borrowed from
 *     the internal `files[]` array. They are invalidated only when the
 *     specific entry is deleted.
 *   - Paths are absolute, null-terminated, no trailing slash, max
 *     RAMFS_NAME_SIZE-1 bytes including '\0'. The root "/" itself is
 *     implicit — there is no slot for it.
 *   - Directory entries are explicit (`is_dir = true`). A directory cannot
 *     be rmdir'd while children exist.
 *   - File data is bounded by RAMFS_DATA_SIZE-1 bytes plus '\0'.
 *
 * Non-goals:
 *   - No symbolic links, no permissions, no atime/mtime, no inode numbers.
 *     Those belong to the VFS layer above.
 *   - No defrag. file_count is intentionally absent; deletion leaves holes.
 */

#define RAMFS_MAX_FILES 32
#define RAMFS_NAME_SIZE OSNOS_PATH_MAX
#define RAMFS_DATA_SIZE 512

typedef struct {
    bool used;
    bool is_dir;
    char name[RAMFS_NAME_SIZE];
    char data[RAMFS_DATA_SIZE];
    uint64_t size;
} ramfs_file_t;

void ramfs_init(void);

bool ramfs_create_file(const char *name, const char *data);
bool ramfs_write_file(const char *name, const char *data);
bool ramfs_append_file(const char *name, const char *data);
bool ramfs_touch(const char *name);
bool ramfs_delete_file(const char *name);

bool ramfs_mkdir(const char *name);
bool ramfs_rmdir(const char *name);

bool ramfs_copy_file(const char *src, const char *dst);
bool ramfs_move(const char *src, const char *dst);

const ramfs_file_t *ramfs_find(const char *name);

size_t ramfs_list_dir(
    const char *path,
    char *out,
    size_t out_size
);

size_t ramfs_tree(
    const char *path,
    char *out,
    size_t out_size
);

/*
 * Iterator over direct children of `parent`. `*cursor` is the next slot
 * index to examine; the caller starts at 0 and passes the same `cursor`
 * unmodified across calls. Returns the next matching entry and updates
 * `*cursor`, or returns NULL when no more children at or after the cursor.
 *
 * Used by ramfs_vfs.c to implement vfs_readdir without exposing slot
 * internals.
 */
const ramfs_file_t *ramfs_iter_child(const char *parent, size_t *cursor);

/* Returns the index of a slot pointer previously returned by ramfs_find /
 * ramfs_iter_child. Used by the VFS adapter to populate inode. */
size_t ramfs_slot_index(const ramfs_file_t *f);

/* Number of slots currently in use (files + dirs). For sysfs. */
size_t ramfs_used_count(void);

bool ramfs_path_has_wildcard(const char *path);

size_t ramfs_delete_glob(const char *pattern);

size_t ramfs_read_glob(
    const char *pattern,
    char *out,
    size_t out_size
);

size_t ramfs_list_glob(
    const char *pattern,
    char *out,
    size_t out_size
);
