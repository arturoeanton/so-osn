#include "fat_vfs.h"

#include "../lib/string.h"
#include "fat.h"

#define FAT_MOUNT_PREFIX     "/sd"
#define FAT_MOUNT_PREFIX_LEN 3

/*
 * Strip the mount prefix from a VFS path and hand back the relative
 * path the FAT layer expects.
 *   "/sd"              -> "/"
 *   "/sd/README.TXT"   -> "/README.TXT"
 *   "/sd/sub/file"     -> "/sub/file"
 * Returns NULL if the path doesn't belong to this mount.
 */
static const char *strip_mount(const char *path) {
    if (!os_strstarts(path, FAT_MOUNT_PREFIX)) return 0;
    if (path[FAT_MOUNT_PREFIX_LEN] == 0)       return "/";
    if (path[FAT_MOUNT_PREFIX_LEN] != '/')     return 0;
    return path + FAT_MOUNT_PREFIX_LEN;
}

/* fat_*_path returns negative errno-style ints whose absolute values
 * match osnos_status_t. Convert with one negation. */
static osnos_status_t fat_rc_to_status(int rc) {
    if (rc >= 0) return OSNOS_OK;
    return (osnos_status_t)(-rc);
}

static osnos_status_t fat_vfs_stat(void *priv, const char *path,
                                    vfs_stat_t *out) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;

    fat_dirent_t de;
    if (fat_lookup(rel, &de) != 0) return OSNOS_ENOENT;

    out->type  = de.is_dir ? VFS_NODE_DIR : VFS_NODE_REG;
    out->size  = de.is_dir ? 0 : de.size;
    out->inode = ((uint64_t)de.dirent_lba << 16) | de.dirent_offset;
    out->mode  = de.is_dir ? 0755 : 0644;
    return OSNOS_OK;
}

static osnos_status_t fat_vfs_readdir(void *priv, const char *path,
                                       size_t cursor,
                                       vfs_dirent_t *out,
                                       size_t *next_cursor) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;

    fat_dirent_t dir;
    if (fat_lookup(rel, &dir) != 0) return OSNOS_ENOENT;
    if (!dir.is_dir) return OSNOS_ENOTDIR;

    fat_dirent_t ent;
    uint32_t next = (uint32_t)cursor;
    for (;;) {
        if (fat_readdir(&dir, (uint32_t)cursor, &ent, &next) != 0) {
            return OSNOS_ENOENT;
        }
        /* Hide "." and ".." — VFS convention. */
        if (ent.name[0] == '.' &&
            (ent.name[1] == 0 || (ent.name[1] == '.' && ent.name[2] == 0))) {
            cursor = next;
            continue;
        }
        break;
    }

    os_strlcpy(out->name, ent.name, OSNOS_NAME_MAX);
    out->type = ent.is_dir ? VFS_NODE_DIR : VFS_NODE_REG;
    *next_cursor = next;
    return OSNOS_OK;
}

static osnos_status_t fat_vfs_read(void *priv, const char *path,
                                    size_t off,
                                    char *buf, size_t buf_size,
                                    size_t *out_size) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;

    fat_dirent_t de;
    if (fat_lookup(rel, &de) != 0) return OSNOS_ENOENT;
    if (de.is_dir) return OSNOS_EISDIR;

    /* Past EOF → 0 bytes (no error). */
    if (off >= de.size) { *out_size = 0; return OSNOS_OK; }
    uint32_t avail = (uint32_t)(de.size - off);
    uint32_t len   = (buf_size > avail) ? avail : (uint32_t)buf_size;

    int n = fat_read_file(&de, (uint32_t)off, buf, len);
    if (n < 0) return OSNOS_EIO;
    *out_size = (size_t)n;
    return OSNOS_OK;
}

static osnos_status_t fat_vfs_write(void *priv, const char *path,
                                     const char *buf, size_t buf_size) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;
    if (buf_size > 0xFFFFFFFFu) return OSNOS_EINVAL;
    return fat_rc_to_status(fat_write_path(rel, buf, (uint32_t)buf_size));
}

static osnos_status_t fat_vfs_append(void *priv, const char *path,
                                      const char *buf, size_t buf_size) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;
    if (buf_size > 0xFFFFFFFFu) return OSNOS_EINVAL;
    return fat_rc_to_status(fat_append_path(rel, buf, (uint32_t)buf_size));
}

static osnos_status_t fat_vfs_mkdir(void *priv, const char *path) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;
    return fat_rc_to_status(fat_mkdir_path(rel));
}

static osnos_status_t fat_vfs_rmdir(void *priv, const char *path) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;
    return fat_rc_to_status(fat_rmdir_path(rel));
}

static osnos_status_t fat_vfs_unlink(void *priv, const char *path) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;
    return fat_rc_to_status(fat_unlink_path(rel));
}

static osnos_status_t fat_vfs_rename(void *priv, const char *src,
                                      const char *dst) {
    (void)priv;
    const char *rel_src = strip_mount(src);
    const char *rel_dst = strip_mount(dst);
    /* vfs_move only invokes .rename when src and dst share a mount,
     * so both strip_mount calls must succeed. */
    if (!rel_src || !rel_dst) return OSNOS_EINVAL;
    return fat_rc_to_status(fat_rename_path(rel_src, rel_dst));
}

const vfs_ops_t fat_vfs_ops = {
    .stat    = fat_vfs_stat,
    .readdir = fat_vfs_readdir,
    .read    = fat_vfs_read,
    .write   = fat_vfs_write,
    .append  = fat_vfs_append,
    .mkdir   = fat_vfs_mkdir,
    .rmdir   = fat_vfs_rmdir,
    .unlink  = fat_vfs_unlink,
    .rename  = fat_vfs_rename
};
