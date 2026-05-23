#include "binfs.h"

#include "../lib/string.h"
#include "../proc/builtin.h"
#include "vfs.h"

static const char *entry_name(const char *path) {
    if (!os_strstarts(path, "/bin")) return 0;
    if (path[4] == 0) return "";
    if (path[4] != '/') return 0;
    return path + 5;
}

static void make_description(const builtin_t *b, char *out, size_t out_size) {
    os_strlcpy(out, "builtin: ", out_size);
    os_strlcat(out, b->name, out_size);
    if (b->desc) {
        os_strlcat(out, " - ", out_size);
        os_strlcat(out, b->desc, out_size);
    }
    os_strlcat(out, "\n", out_size);
}

static osnos_status_t binfs_stat(void *priv, const char *path, vfs_stat_t *out) {
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

    for (size_t i = 0; i < builtin_count(); i++) {
        const builtin_t *b = builtin_at(i);
        if (!os_streq(b->name, name)) continue;

        char tmp[128];
        make_description(b, tmp, sizeof(tmp));
        out->type  = VFS_NODE_REG;
        out->size  = os_strlen(tmp);
        out->inode = i + 1;
        out->mode  = 0555;  /* r-x r-x r-x — executable, not writable */
        return OSNOS_OK;
    }

    return OSNOS_ENOENT;
}

static osnos_status_t binfs_readdir(
    void *priv, const char *path, size_t cursor,
    vfs_dirent_t *out, size_t *next_cursor
) {
    (void)priv;
    const char *name = entry_name(path);
    if (!name || name[0] != 0) return OSNOS_ENOENT;

    if (cursor >= builtin_count()) return OSNOS_ENOENT;

    os_strlcpy(out->name, builtin_at(cursor)->name, OSNOS_NAME_MAX);
    out->type = VFS_NODE_REG;
    *next_cursor = cursor + 1;
    return OSNOS_OK;
}

static osnos_status_t binfs_read(
    void *priv, const char *path,
    size_t off,
    char *buf, size_t buf_size, size_t *out_size
) {
    (void)priv;
    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;
    if (name[0] == 0) return OSNOS_EISDIR;

    for (size_t i = 0; i < builtin_count(); i++) {
        const builtin_t *b = builtin_at(i);
        if (!os_streq(b->name, name)) continue;

        char tmp[128];
        make_description(b, tmp, sizeof(tmp));
        size_t len = os_strlen(tmp);
        if (off >= len) { *out_size = 0; return OSNOS_OK; }
        size_t avail = len - off;
        size_t n = (avail > buf_size) ? buf_size : avail;
        for (size_t k = 0; k < n; k++) buf[k] = tmp[off + k];
        *out_size = n;
        return OSNOS_OK;
    }
    return OSNOS_ENOENT;
}

static osnos_status_t binfs_rofs(void *priv, const char *path) {
    (void)priv; (void)path;
    return OSNOS_EROFS;
}

static osnos_status_t binfs_write(
    void *priv, const char *path, const char *buf, size_t buf_size
) {
    (void)priv; (void)path; (void)buf; (void)buf_size;
    return OSNOS_EROFS;
}

static osnos_status_t binfs_rename(
    void *priv, const char *src, const char *dst
) {
    (void)priv; (void)src; (void)dst;
    return OSNOS_EROFS;
}

const vfs_ops_t binfs_vfs_ops = {
    .stat    = binfs_stat,
    .readdir = binfs_readdir,
    .read    = binfs_read,
    .write   = binfs_write,
    .append  = binfs_write,
    .mkdir   = binfs_rofs,
    .rmdir   = binfs_rofs,
    .unlink  = binfs_rofs,
    .rename  = binfs_rename
};
