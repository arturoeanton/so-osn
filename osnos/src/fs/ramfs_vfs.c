#include "ramfs_vfs.h"

#include "../lib/string.h"
#include "ramfs.h"

/*
 * Each wrapper converts a ramfs_* bool return into osnos_status_t.
 *
 * Limitation: the ramfs core is null-term-based (it stores files as
 * C strings with a trailing '\0' bounded by RAMFS_DATA_SIZE). VFS write
 * and append carry an explicit `buf_size`, so this adapter copies the
 * incoming buffer into a stack-local buffer and null-terminates it
 * before delegating. When ramfs grows binary-safe APIs (or when a
 * disk-backed FS arrives), this layer can pass through unchanged.
 */

static osnos_status_t ramfs_vfs_stat(
    void *priv,
    const char *path,
    vfs_stat_t *out
) {
    (void)priv;

    if (os_streq(path, "/")) {
        out->type  = VFS_NODE_DIR;
        out->size  = 0;
        out->inode = 0;
        out->mode  = 0755;
        return OSNOS_OK;
    }

    const ramfs_file_t *f = ramfs_find(path);
    if (!f) return OSNOS_ENOENT;

    out->type  = f->is_dir ? VFS_NODE_DIR : VFS_NODE_REG;
    out->size  = f->size;
    out->inode = (uint64_t)ramfs_slot_index(f);
    out->mode  = f->is_dir ? 0755 : 0644;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_readdir(
    void *priv,
    const char *path,
    size_t cursor,
    vfs_dirent_t *out,
    size_t *next_cursor
) {
    (void)priv;

    size_t c = cursor;
    const ramfs_file_t *f = ramfs_iter_child(path, &c);
    if (!f) return OSNOS_ENOENT;

    const char *slash = os_strrchr(f->name, '/');
    const char *base = slash ? slash + 1 : f->name;

    os_strlcpy(out->name, base, OSNOS_NAME_MAX);
    out->type = f->is_dir ? VFS_NODE_DIR : VFS_NODE_REG;
    *next_cursor = c;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_read(
    void *priv,
    const char *path,
    char *buf,
    size_t buf_size,
    size_t *out_size
) {
    (void)priv;

    const ramfs_file_t *f = ramfs_find(path);
    if (!f) return OSNOS_ENOENT;
    if (f->is_dir) return OSNOS_EISDIR;

    size_t n = f->size;
    if (n > buf_size) n = buf_size;

    for (size_t i = 0; i < n; i++) {
        buf[i] = f->data[i];
    }

    *out_size = n;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_write(
    void *priv,
    const char *path,
    const char *buf,
    size_t buf_size
) {
    (void)priv;
    /* Binary-safe: keep embedded NULs (sparse-hole writes, ELF
     * blobs) intact instead of letting strlen truncate them. */
    if (!ramfs_write_file_bin(path, buf, buf_size)) return OSNOS_ENOSPC;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_append(
    void *priv,
    const char *path,
    const char *buf,
    size_t buf_size
) {
    (void)priv;
    if (!ramfs_append_file_bin(path, buf, buf_size)) return OSNOS_ENOSPC;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_mkdir(void *priv, const char *path) {
    (void)priv;
    if (ramfs_find(path)) return OSNOS_EEXIST;
    if (!ramfs_mkdir(path)) return OSNOS_ENOSPC;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_rmdir(void *priv, const char *path) {
    (void)priv;
    const ramfs_file_t *f = ramfs_find(path);
    if (!f) return OSNOS_ENOENT;
    if (!f->is_dir) return OSNOS_ENOTDIR;
    if (!ramfs_rmdir(path)) return OSNOS_ENOTEMPTY;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_unlink(void *priv, const char *path) {
    (void)priv;
    const ramfs_file_t *f = ramfs_find(path);
    if (!f) return OSNOS_ENOENT;
    if (f->is_dir) return OSNOS_EISDIR;
    if (!ramfs_delete_file(path)) return OSNOS_EIO;
    return OSNOS_OK;
}

static osnos_status_t ramfs_vfs_rename(
    void *priv,
    const char *src,
    const char *dst
) {
    (void)priv;
    if (!ramfs_find(src)) return OSNOS_ENOENT;
    if (ramfs_find(dst)) return OSNOS_EEXIST;
    if (!ramfs_move(src, dst)) return OSNOS_ENAMETOOLONG;
    return OSNOS_OK;
}

const vfs_ops_t ramfs_vfs_ops = {
    .stat    = ramfs_vfs_stat,
    .readdir = ramfs_vfs_readdir,
    .read    = ramfs_vfs_read,
    .write   = ramfs_vfs_write,
    .append  = ramfs_vfs_append,
    .mkdir   = ramfs_vfs_mkdir,
    .rmdir   = ramfs_vfs_rmdir,
    .unlink  = ramfs_vfs_unlink,
    .rename  = ramfs_vfs_rename
};
