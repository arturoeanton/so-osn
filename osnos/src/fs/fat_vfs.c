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

static osnos_status_t fat_vfs_stat(void *priv, const char *path,
                                    vfs_stat_t *out) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;

    fat_dirent_t de;
    if (fat_lookup(rel, &de) != 0) return OSNOS_ENOENT;

    out->type  = de.is_dir ? VFS_NODE_DIR : VFS_NODE_REG;
    out->size  = de.is_dir ? 0 : de.size;
    /* dirent_lba<<16 | offset is a stable id per file lifetime. The
     * root sentinel hits 0 here, which matches sysfs/ramfs convention. */
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
    if (fat_readdir(&dir, (uint32_t)cursor, &ent, &next) != 0) {
        return OSNOS_ENOENT;
    }

    os_strlcpy(out->name, ent.name, OSNOS_NAME_MAX);
    out->type = ent.is_dir ? VFS_NODE_DIR : VFS_NODE_REG;
    *next_cursor = next;
    return OSNOS_OK;
}

static osnos_status_t fat_vfs_read(void *priv, const char *path,
                                    char *buf, size_t buf_size,
                                    size_t *out_size) {
    (void)priv;
    const char *rel = strip_mount(path);
    if (!rel) return OSNOS_ENOENT;

    fat_dirent_t de;
    if (fat_lookup(rel, &de) != 0) return OSNOS_ENOENT;
    if (de.is_dir) return OSNOS_EISDIR;

    uint32_t len = (buf_size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)buf_size;
    int n = fat_read_file(&de, 0, buf, len);
    if (n < 0) return OSNOS_EIO;
    *out_size = (size_t)n;
    return OSNOS_OK;
}

static osnos_status_t fat_vfs_rofs(void *priv, const char *path) {
    (void)priv; (void)path;
    return OSNOS_EROFS;
}

static osnos_status_t fat_vfs_rofs_write(void *priv, const char *path,
                                          const char *buf, size_t n) {
    (void)priv; (void)path; (void)buf; (void)n;
    return OSNOS_EROFS;
}

static osnos_status_t fat_vfs_rofs_rename(void *priv, const char *a,
                                           const char *b) {
    (void)priv; (void)a; (void)b;
    return OSNOS_EROFS;
}

const vfs_ops_t fat_vfs_ops = {
    .stat    = fat_vfs_stat,
    .readdir = fat_vfs_readdir,
    .read    = fat_vfs_read,
    .write   = fat_vfs_rofs_write,
    .append  = fat_vfs_rofs_write,
    .mkdir   = fat_vfs_rofs,
    .rmdir   = fat_vfs_rofs,
    .unlink  = fat_vfs_rofs,
    .rename  = fat_vfs_rofs_rename
};
