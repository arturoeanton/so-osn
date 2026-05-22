#include "vfs.h"

#include "../lib/string.h"

/*
 * VFS dispatch layer. Owns the mount table; forwards every public vfs_*
 * call to the appropriate backend via longest-prefix match.
 *
 * Implementation notes:
 *   - Mount table is a fixed array of VFS_MAX_MOUNTS slots, indexed by
 *     `used` flag. No defragmenting; future vfs_unmount marks `used=false`.
 *   - find_mount() does a linear scan keeping the longest matching prefix.
 *     With VFS_MAX_MOUNTS=8 and average paths of ~16 chars this is well
 *     under a microsecond — no need for trees.
 *   - Public entry points validate inputs (non-null, path starts with '/')
 *     so backends never need defensive checks.
 *   - copy/move are composed from primitives. Backends can opt-in to a
 *     fast-path rename via ops->rename when src and dst share a mount.
 */

#define VFS_COPY_BUF_SIZE 1024

static vfs_mount_t mounts[VFS_MAX_MOUNTS];

void vfs_init(void) {
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].used = false;
        mounts[i].mountpoint[0] = 0;
        mounts[i].ops = 0;
        mounts[i].priv = 0;
    }
}

osnos_status_t vfs_mount(
    const char *mountpoint,
    const vfs_ops_t *ops,
    void *priv
) {
    if (!mountpoint || !ops) return OSNOS_EINVAL;
    if (mountpoint[0] != '/') return OSNOS_EINVAL;

    size_t mp_len = os_strlen(mountpoint);
    if (mp_len + 1 > OSNOS_PATH_MAX) return OSNOS_ENAMETOOLONG;

    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].used && os_streq(mounts[i].mountpoint, mountpoint)) {
            return OSNOS_EEXIST;
        }
    }

    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) {
            os_strlcpy(mounts[i].mountpoint, mountpoint, OSNOS_PATH_MAX);
            mounts[i].ops = ops;
            mounts[i].priv = priv;
            mounts[i].used = true;
            return OSNOS_OK;
        }
    }

    return OSNOS_ENOSPC;
}

const vfs_mount_t *vfs_mount_slot(size_t idx) {
    if (idx >= VFS_MAX_MOUNTS) return 0;
    if (!mounts[idx].used) return 0;
    return &mounts[idx];
}

/*
 * Longest-prefix match. A mountpoint "/dev" matches paths "/dev" and
 * "/dev/foo" but NOT "/devfs". The root mountpoint "/" is a wildcard
 * fallback when present.
 */
static const vfs_mount_t *find_mount(const char *path) {
    const vfs_mount_t *best = 0;
    size_t best_len = 0;

    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) continue;

        size_t mp_len = os_strlen(mounts[i].mountpoint);

        if (!os_strstarts(path, mounts[i].mountpoint)) continue;

        /* Disambiguate "/dev" from "/devfs": next char must be '\0' or '/'. */
        if (mp_len > 1 && path[mp_len] != 0 && path[mp_len] != '/') {
            continue;
        }

        if (mp_len > best_len) {
            best = &mounts[i];
            best_len = mp_len;
        }
    }

    return best;
}

static osnos_status_t check_path(const char *path) {
    if (!path) return OSNOS_EINVAL;
    if (path[0] != '/') return OSNOS_EINVAL;
    if (os_strlen(path) + 1 > OSNOS_PATH_MAX) return OSNOS_ENAMETOOLONG;
    return OSNOS_OK;
}

osnos_status_t vfs_stat(const char *path, vfs_stat_t *out) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;
    if (!out) return OSNOS_EINVAL;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->stat) return OSNOS_EINVAL;

    return m->ops->stat(m->priv, path, out);
}

/* Helper: count non-root mounts whose mountpoint is exactly one
 * directory below "/" (i.e. "/sys", "/bin", "/dev", "/home", "/sd"
 * — not their sub-paths). Used by vfs_readdir to expose them as
 * synthetic entries when listing the root dir. */
static int submount_at_index(int want, char *name_out) {
    int i_real = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) continue;
        const char *mp = mounts[i].mountpoint;
        if (mp[0] != '/') continue;
        if (mp[1] == 0) continue;            /* root itself */
        int has_more_slash = 0;
        for (size_t k = 1; mp[k]; k++) if (mp[k] == '/') { has_more_slash = 1; break; }
        if (has_more_slash) continue;
        if (i_real == want) {
            size_t k = 1;
            int n = 0;
            while (mp[k] && n + 1 < OSNOS_NAME_MAX) name_out[n++] = mp[k++];
            name_out[n] = 0;
            return 1;
        }
        i_real++;
    }
    return 0;
}

osnos_status_t vfs_readdir(
    const char *path,
    size_t *cursor,
    vfs_dirent_t *out
) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;
    if (!cursor || !out) return OSNOS_EINVAL;

    /* Listing the root: synthesise entries for the top-level mounts
     * (/bin, /dev, /sys, /home, /sd). Ramfs at "/" today only stores
     * files in subdirectories that are themselves mounts, so once
     * the synthetic entries are exhausted there's nothing more to
     * return. Just signal ENOENT. */
    if (path[0] == '/' && path[1] == 0) {
        char name[OSNOS_NAME_MAX];
        if (!submount_at_index((int)*cursor, name)) return OSNOS_ENOENT;
        size_t nl = 0;
        while (name[nl] && nl + 1 < OSNOS_NAME_MAX) {
            out->name[nl] = name[nl];
            nl++;
        }
        out->name[nl] = 0;
        out->type = VFS_NODE_DIR;
        (*cursor)++;
        return OSNOS_OK;
    }

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->readdir) return OSNOS_EINVAL;

    size_t next = *cursor;
    osnos_status_t status =
        m->ops->readdir(m->priv, path, *cursor, out, &next);

    if (status == OSNOS_OK) {
        *cursor = next;
    }

    return status;
}

osnos_status_t vfs_read(
    const char *path,
    char *buf,
    size_t buf_size,
    size_t *out_size
) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;
    if (!buf || !out_size) return OSNOS_EINVAL;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->read) return OSNOS_EINVAL;

    return m->ops->read(m->priv, path, buf, buf_size, out_size);
}

osnos_status_t vfs_write(
    const char *path,
    const char *buf,
    size_t buf_size
) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;
    if (!buf && buf_size > 0) return OSNOS_EINVAL;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->write) return OSNOS_EINVAL;

    return m->ops->write(m->priv, path, buf, buf_size);
}

osnos_status_t vfs_append(
    const char *path,
    const char *buf,
    size_t buf_size
) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;
    if (!buf && buf_size > 0) return OSNOS_EINVAL;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->append) return OSNOS_EINVAL;

    return m->ops->append(m->priv, path, buf, buf_size);
}

osnos_status_t vfs_mkdir(const char *path) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->mkdir) return OSNOS_EINVAL;

    return m->ops->mkdir(m->priv, path);
}

osnos_status_t vfs_rmdir(const char *path) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->rmdir) return OSNOS_EINVAL;

    return m->ops->rmdir(m->priv, path);
}

osnos_status_t vfs_unlink(const char *path) {
    osnos_status_t s = check_path(path);
    if (s != OSNOS_OK) return s;

    const vfs_mount_t *m = find_mount(path);
    if (!m) return OSNOS_ENOENT;
    if (!m->ops->unlink) return OSNOS_EINVAL;

    return m->ops->unlink(m->priv, path);
}

osnos_status_t vfs_copy(const char *src, const char *dst) {
    osnos_status_t s;

    s = check_path(src);
    if (s != OSNOS_OK) return s;
    s = check_path(dst);
    if (s != OSNOS_OK) return s;

    /*
     * Single-shot copy. Works for any file up to VFS_COPY_BUF_SIZE; today
     * RAMFS_DATA_SIZE=512 so this is comfortable. When disk-backed FS
     * arrives, this becomes a chunked loop using a future
     * read-at-offset op.
     */
    static char buf[VFS_COPY_BUF_SIZE];
    size_t got = 0;

    s = vfs_read(src, buf, sizeof(buf), &got);
    if (s != OSNOS_OK) return s;

    return vfs_write(dst, buf, got);
}

osnos_status_t vfs_move(const char *src, const char *dst) {
    osnos_status_t s;

    s = check_path(src);
    if (s != OSNOS_OK) return s;
    s = check_path(dst);
    if (s != OSNOS_OK) return s;

    const vfs_mount_t *src_m = find_mount(src);
    const vfs_mount_t *dst_m = find_mount(dst);

    if (!src_m || !dst_m) return OSNOS_ENOENT;

    /* Fast path: same mount + backend offers rename. */
    if (src_m == dst_m && src_m->ops->rename) {
        return src_m->ops->rename(src_m->priv, src, dst);
    }

    /* Fallback: copy + unlink. Not atomic across the steps. */
    s = vfs_copy(src, dst);
    if (s != OSNOS_OK) return s;

    return vfs_unlink(src);
}

osnos_status_t vfs_touch(const char *path) {
    vfs_stat_t st;
    osnos_status_t s = vfs_stat(path, &st);
    if (s == OSNOS_OK) return OSNOS_OK;
    if (s != OSNOS_ENOENT) return s;
    return vfs_write(path, "", 0);
}

/* --- helpers for path / glob composition --- */

bool vfs_path_has_wildcard(const char *path) {
    if (!path) return false;
    while (*path) {
        if (*path == '*') return true;
        path++;
    }
    return false;
}

static bool glob_match(const char *pattern, const char *name) {
    if (*pattern == 0) return *name == 0;

    if (*pattern == '*') {
        while (*(pattern + 1) == '*') pattern++;

        if (*(pattern + 1) == 0) {
            while (*name) {
                if (*name == '/') return false;
                name++;
            }
            return true;
        }

        while (*name) {
            if (glob_match(pattern + 1, name)) return true;
            if (*name == '/') return false;
            name++;
        }
        return glob_match(pattern + 1, name);
    }

    if (*pattern != *name) return false;
    return glob_match(pattern + 1, name + 1);
}

static void split_at_last_slash(
    const char *path,
    char *dir_out, size_t dir_size,
    char *base_out, size_t base_size
) {
    const char *slash = os_strrchr(path, '/');

    if (!slash) {
        dir_out[0] = 0;
        os_strlcpy(base_out, path, base_size);
        return;
    }

    if (slash == path) {
        os_strlcpy(dir_out, "/", dir_size);
    } else {
        size_t dir_len = (size_t)(slash - path);
        if (dir_len + 1 > dir_size) dir_len = dir_size - 1;
        for (size_t i = 0; i < dir_len; i++) dir_out[i] = path[i];
        dir_out[dir_len] = 0;
    }

    os_strlcpy(base_out, slash + 1, base_size);
}

static void path_join(
    char *out, size_t out_size,
    const char *dir, const char *name
) {
    if (os_streq(dir, "/")) {
        os_strlcpy(out, "/", out_size);
    } else {
        os_strlcpy(out, dir, out_size);
        os_strlcat(out, "/", out_size);
    }
    os_strlcat(out, name, out_size);
}

static size_t emit_entry(
    const vfs_dirent_t *ent,
    size_t indent,
    char *out, size_t out_size, size_t written
) {
    for (size_t d = 0; d < indent; d++) {
        if (written + 2 >= out_size) return written;
        out[written++] = ' ';
        out[written++] = ' ';
    }
    size_t name_len = os_strlen(ent->name);
    for (size_t i = 0; i < name_len && written + 1 < out_size; i++) {
        out[written++] = ent->name[i];
    }
    if (ent->type == VFS_NODE_DIR && written + 1 < out_size) {
        out[written++] = '/';
    }
    if (written + 1 < out_size) out[written++] = '\n';
    out[written] = 0;
    return written;
}

size_t vfs_list_dir(const char *path, char *out, size_t out_size) {
    out[0] = 0;
    size_t written = 0;
    size_t cursor = 0;
    vfs_dirent_t ent;

    while (vfs_readdir(path, &cursor, &ent) == OSNOS_OK) {
        written = emit_entry(&ent, 0, out, out_size, written);
    }

    return written;
}

#define VFS_TREE_MAX_DEPTH 16

typedef struct {
    char    path[OSNOS_PATH_MAX];
    size_t  depth;
    size_t  cursor;
} vfs_tree_frame_t;

size_t vfs_tree(const char *path, char *out, size_t out_size) {
    out[0] = 0;
    size_t written = 0;

    vfs_tree_frame_t stack[VFS_TREE_MAX_DEPTH];
    int top = 0;
    os_strlcpy(stack[0].path,
               (path && path[0]) ? path : "/",
               OSNOS_PATH_MAX);
    stack[0].depth  = 0;
    stack[0].cursor = 0;

    while (top >= 0) {
        vfs_tree_frame_t *frame = &stack[top];
        vfs_dirent_t ent;

        if (vfs_readdir(frame->path, &frame->cursor, &ent) != OSNOS_OK) {
            top--;
            continue;
        }

        written = emit_entry(&ent, frame->depth, out, out_size, written);

        if (ent.type == VFS_NODE_DIR && top + 1 < VFS_TREE_MAX_DEPTH) {
            top++;
            path_join(stack[top].path, OSNOS_PATH_MAX,
                      stack[top - 1].path, ent.name);
            stack[top].depth  = stack[top - 1].depth + 1;
            stack[top].cursor = 0;
        }
    }

    return written;
}

size_t vfs_glob_list(const char *pattern, char *out, size_t out_size) {
    char dir[OSNOS_PATH_MAX];
    char base[OSNOS_NAME_MAX];
    split_at_last_slash(pattern, dir, sizeof(dir), base, sizeof(base));

    out[0] = 0;
    size_t written = 0;
    size_t matches = 0;
    size_t cursor = 0;
    vfs_dirent_t ent;

    while (vfs_readdir(dir, &cursor, &ent) == OSNOS_OK) {
        if (!glob_match(base, ent.name)) continue;
        written = emit_entry(&ent, 0, out, out_size, written);
        matches++;
    }

    return matches;
}

size_t vfs_glob_read(const char *pattern, char *out, size_t out_size) {
    char dir[OSNOS_PATH_MAX];
    char base[OSNOS_NAME_MAX];
    split_at_last_slash(pattern, dir, sizeof(dir), base, sizeof(base));

    out[0] = 0;
    size_t written = 0;
    size_t matches = 0;
    size_t cursor = 0;
    vfs_dirent_t ent;

    while (vfs_readdir(dir, &cursor, &ent) == OSNOS_OK) {
        if (ent.type == VFS_NODE_DIR) continue;
        if (!glob_match(base, ent.name)) continue;

        char full[OSNOS_PATH_MAX];
        path_join(full, sizeof(full), dir, ent.name);

        if (written + 1 >= out_size) break;
        size_t got = 0;
        if (vfs_read(full, out + written, out_size - written - 1, &got) == OSNOS_OK) {
            written += got;
            out[written] = 0;
        }
        matches++;
    }

    return matches;
}

size_t vfs_glob_unlink(const char *pattern) {
    char dir[OSNOS_PATH_MAX];
    char base[OSNOS_NAME_MAX];
    split_at_last_slash(pattern, dir, sizeof(dir), base, sizeof(base));

    size_t deleted = 0;
    size_t cursor = 0;
    vfs_dirent_t ent;

    while (vfs_readdir(dir, &cursor, &ent) == OSNOS_OK) {
        if (ent.type == VFS_NODE_DIR) continue;
        if (!glob_match(base, ent.name)) continue;

        char full[OSNOS_PATH_MAX];
        path_join(full, sizeof(full), dir, ent.name);

        if (vfs_unlink(full) == OSNOS_OK) deleted++;
    }

    return deleted;
}
