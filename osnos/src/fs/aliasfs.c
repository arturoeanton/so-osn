#include "aliasfs.h"

#include "../lib/string.h"

/*
 * Translate `in` (a path under aliasfs->mount_prefix) into the
 * equivalent absolute path under aliasfs->target_prefix, written into
 * `out`. Returns false on overflow or when `in` doesn't actually start
 * with the mount prefix (defensive — the VFS layer already guarantees
 * the latter, but check anyway).
 *
 *   mount = "/home", target = "/sd/home"
 *   "/home"            -> "/sd/home"
 *   "/home/README.TXT" -> "/sd/home/README.TXT"
 *   "/home/sub/file"   -> "/sd/home/sub/file"
 */
static bool translate(const aliasfs_t *a,
                       const char *in,
                       char *out, size_t out_size) {
    /* Must start with mount_prefix and be either exactly that or have
     * a '/' separator next. */
    if (os_strncmp(in, a->mount_prefix, a->mount_len) != 0) return false;
    char after = in[a->mount_len];
    if (after != 0 && after != '/') return false;

    if (a->target_len >= out_size) return false;
    for (size_t i = 0; i < a->target_len; i++) out[i] = a->target_prefix[i];

    /* Tail: everything past the mount_prefix in the input. For input
     * "/home" we copy nothing; output stays "/sd/home". For
     * "/home/foo" we copy "/foo" → "/sd/home/foo". */
    size_t w = a->target_len;
    const char *tail = in + a->mount_len;
    while (*tail) {
        if (w + 1 >= out_size) return false;
        out[w++] = *tail++;
    }
    out[w] = 0;
    return true;
}

bool aliasfs_init(aliasfs_t *slot,
                   const char *mount_prefix,
                   const char *target_prefix) {
    if (!slot || !mount_prefix || !target_prefix) return false;
    size_t ml = os_strlen(mount_prefix);
    size_t tl = os_strlen(target_prefix);
    if (ml == 0 || ml >= OSNOS_PATH_MAX) return false;
    if (tl == 0 || tl >= OSNOS_PATH_MAX) return false;

    os_strlcpy(slot->mount_prefix,  mount_prefix,  OSNOS_PATH_MAX);
    os_strlcpy(slot->target_prefix, target_prefix, OSNOS_PATH_MAX);
    slot->mount_len  = ml;
    slot->target_len = tl;
    return true;
}

/* ----- ops implementations ----- */

static osnos_status_t alias_stat(void *priv, const char *path,
                                   vfs_stat_t *out) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    return vfs_stat(xlat, out);
}

static osnos_status_t alias_readdir(void *priv, const char *path,
                                      size_t cursor,
                                      vfs_dirent_t *out,
                                      size_t *next_cursor) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    osnos_status_t s = vfs_readdir(xlat, &cursor, out);
    if (s == OSNOS_OK) *next_cursor = cursor;
    return s;
}

static osnos_status_t alias_read(void *priv, const char *path,
                                   char *buf, size_t buf_size,
                                   size_t *out_size) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    osnos_status_t s = vfs_read(xlat, buf, buf_size, out_size);
    return s;
}

static osnos_status_t alias_write(void *priv, const char *path,
                                    const char *buf, size_t buf_size) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    return vfs_write(xlat, buf, buf_size);
}

static osnos_status_t alias_append(void *priv, const char *path,
                                     const char *buf, size_t buf_size) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    return vfs_append(xlat, buf, buf_size);
}

static osnos_status_t alias_mkdir(void *priv, const char *path) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    return vfs_mkdir(xlat);
}

static osnos_status_t alias_rmdir(void *priv, const char *path) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    return vfs_rmdir(xlat);
}

static osnos_status_t alias_unlink(void *priv, const char *path) {
    char xlat[OSNOS_PATH_MAX];
    if (!translate(priv, path, xlat, sizeof(xlat))) return OSNOS_ENOENT;
    return vfs_unlink(xlat);
}

static osnos_status_t alias_rename(void *priv, const char *src, const char *dst) {
    char xsrc[OSNOS_PATH_MAX], xdst[OSNOS_PATH_MAX];
    if (!translate(priv, src, xsrc, sizeof(xsrc))) return OSNOS_ENOENT;
    if (!translate(priv, dst, xdst, sizeof(xdst))) return OSNOS_ENOENT;
    /* vfs_move is the multi-mount-safe layer (will fall back to copy
     * if src/dst sit on different backends — though here they share
     * the same target prefix so the rename fast path should fire). */
    return vfs_move(xsrc, xdst);
}

const vfs_ops_t aliasfs_ops = {
    .stat    = alias_stat,
    .readdir = alias_readdir,
    .read    = alias_read,
    .write   = alias_write,
    .append  = alias_append,
    .mkdir   = alias_mkdir,
    .rmdir   = alias_rmdir,
    .unlink  = alias_unlink,
    .rename  = alias_rename,
};
