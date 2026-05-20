#include "devfs.h"

#include "../lib/string.h"
#include "vfs.h"

/*
 * Each character device has its own read/write semantics. Stat / readdir /
 * the dir-level ops live in this file once; per-device behavior is in
 * the function pointers below.
 */
typedef osnos_status_t (*dev_read_fn)(char *buf, size_t buf_size, size_t *out);
typedef osnos_status_t (*dev_write_fn)(const char *buf, size_t buf_size);

typedef struct {
    const char  *name;
    dev_read_fn  read;
    dev_write_fn write;
} devfs_dev_t;

static osnos_status_t null_read(char *buf, size_t buf_size, size_t *out) {
    (void)buf; (void)buf_size;
    *out = 0;
    return OSNOS_OK;
}

static osnos_status_t null_write(const char *buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return OSNOS_OK;
}

static osnos_status_t zero_read(char *buf, size_t buf_size, size_t *out) {
    for (size_t i = 0; i < buf_size; i++) buf[i] = 0;
    *out = buf_size;
    return OSNOS_OK;
}

static osnos_status_t zero_write(const char *buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return OSNOS_OK;
}

static const devfs_dev_t devices[] = {
    { "null", null_read, null_write },
    { "zero", zero_read, zero_write }
};

#define DEVFS_DEV_COUNT (sizeof(devices) / sizeof(devices[0]))

static const char *entry_name(const char *path) {
    if (!os_strstarts(path, "/dev")) return 0;
    if (path[4] == 0) return "";
    if (path[4] != '/') return 0;
    return path + 5;
}

static const devfs_dev_t *lookup_dev(const char *name) {
    for (size_t i = 0; i < DEVFS_DEV_COUNT; i++) {
        if (os_streq(devices[i].name, name)) return &devices[i];
    }
    return 0;
}

static osnos_status_t devfs_stat(
    void *priv, const char *path, vfs_stat_t *out
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;

    if (name[0] == 0) {
        out->type  = VFS_NODE_DIR;
        out->size  = 0;
        out->inode = 0;
        out->mode  = 0555;
        return OSNOS_OK;
    }

    for (size_t i = 0; i < DEVFS_DEV_COUNT; i++) {
        if (!os_streq(devices[i].name, name)) continue;
        out->type  = VFS_NODE_CHR;
        out->size  = 0;
        out->inode = i + 1;
        out->mode  = 0666;
        return OSNOS_OK;
    }

    return OSNOS_ENOENT;
}

static osnos_status_t devfs_readdir(
    void *priv, const char *path, size_t cursor,
    vfs_dirent_t *out, size_t *next_cursor
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name || name[0] != 0) return OSNOS_ENOENT;

    if (cursor >= DEVFS_DEV_COUNT) return OSNOS_ENOENT;

    os_strlcpy(out->name, devices[cursor].name, OSNOS_NAME_MAX);
    out->type = VFS_NODE_CHR;
    *next_cursor = cursor + 1;
    return OSNOS_OK;
}

static osnos_status_t devfs_read(
    void *priv, const char *path,
    char *buf, size_t buf_size, size_t *out_size
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;
    if (name[0] == 0) return OSNOS_EISDIR;

    const devfs_dev_t *d = lookup_dev(name);
    if (!d) return OSNOS_ENOENT;

    return d->read(buf, buf_size, out_size);
}

static osnos_status_t devfs_write(
    void *priv, const char *path, const char *buf, size_t buf_size
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;
    if (name[0] == 0) return OSNOS_EISDIR;

    const devfs_dev_t *d = lookup_dev(name);
    if (!d) return OSNOS_ENOENT;

    return d->write(buf, buf_size);
}

static osnos_status_t devfs_rofs(void *priv, const char *path) {
    (void)priv; (void)path;
    return OSNOS_EROFS;
}

static osnos_status_t devfs_rename(
    void *priv, const char *src, const char *dst
) {
    (void)priv; (void)src; (void)dst;
    return OSNOS_EROFS;
}

const vfs_ops_t devfs_vfs_ops = {
    .stat    = devfs_stat,
    .readdir = devfs_readdir,
    .read    = devfs_read,
    .write   = devfs_write,
    .append  = devfs_write,
    .mkdir   = devfs_rofs,
    .rmdir   = devfs_rofs,
    .unlink  = devfs_rofs,
    .rename  = devfs_rename
};
