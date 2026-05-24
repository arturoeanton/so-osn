#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_limits.h"
#include "../include/osnos_status.h"

/*
 * Virtual Filesystem layer — CONTRACT, NOT IMPLEMENTED.
 *
 * Implementation arrives in FASE 2 of ROADMAP.md. This header is the
 * agreed-upon shape so that:
 *   - fs_server stops calling ramfs_* directly and instead calls vfs_*.
 *   - ramfs becomes one backend among several (future: devfs, sysfs,
 *     disk-backed FS).
 *   - the design can be reviewed and stress-tested on paper before any
 *     code is written.
 *
 * See ARCH.md for the target architecture diagram.
 *
 * Path conventions:
 *   - All public vfs_* entry points take absolute paths (start with '/').
 *   - The vfs layer matches the longest mountpoint prefix and dispatches
 *     to the backend, passing the *full* path. Backends interpret paths
 *     in their own namespace (a devfs at /dev sees "/dev/fb0", not
 *     "/fb0"). Backends can strip the prefix if they want.
 *   - Paths are null-terminated; max length OSNOS_PATH_MAX including '\0'.
 *
 * Statelessness:
 *   - No file descriptors yet. Every operation is path-based and atomic
 *     at the granularity of a single call. FD layer is FASE 4 of ROADMAP
 *     (see the future-fd note at the bottom).
 */

/* ---------------------------------------------------------------- */
/* Node types                                                       */
/* ---------------------------------------------------------------- */

/*
 * Numeric values match Linux S_IF* (high bits of mode_t) so that future
 * stat() syscall exposure to userland is a zero-translation copy. Do not
 * renumber.
 */
typedef enum {
    VFS_NODE_NONE = 0,        /* not present */
    VFS_NODE_FIFO = 0x1000,   /* S_IFIFO */
    VFS_NODE_CHR  = 0x2000,   /* S_IFCHR  character device */
    VFS_NODE_DIR  = 0x4000,   /* S_IFDIR  directory */
    VFS_NODE_BLK  = 0x6000,   /* S_IFBLK  block device */
    VFS_NODE_REG  = 0x8000,   /* S_IFREG  regular file */
    VFS_NODE_LNK  = 0xA000,   /* S_IFLNK  symlink */
    VFS_NODE_SOCK = 0xC000    /* S_IFSOCK */
} vfs_node_type_t;

/* ---------------------------------------------------------------- */
/* Stat / dirent payloads                                           */
/* ---------------------------------------------------------------- */

typedef struct {
    vfs_node_type_t type;     /* what kind of node */
    uint64_t        size;     /* byte size for files; 0 for dirs/devices */
    uint64_t        inode;    /* unique within a mount; opaque to caller */
    uint32_t        mode;     /* permission bits (0o644 etc.); not enforced */
    /*
     * Future fields when relevant:
     *   uint64_t atime, mtime, ctime;   -- needs clock (FASE 9)
     *   uint32_t uid, gid;              -- needs user model
     *   uint32_t nlink;                 -- needs hardlink support
     */
} vfs_stat_t;

typedef struct {
    char            name[OSNOS_NAME_MAX];  /* basename, no path */
    vfs_node_type_t type;
} vfs_dirent_t;

/* ---------------------------------------------------------------- */
/* Backend contract                                                 */
/* ---------------------------------------------------------------- */

/*
 * Every filesystem backend exports a `const vfs_ops_t` and a private
 * state pointer. Both are handed to vfs_mount().
 *
 * Return values are osnos_status_t. OSNOS_OK on success; >0 on error
 * (errno-style, matches Linux values).
 *
 * `priv` is opaque backend state, never inspected by the vfs layer.
 *
 * Lifetime: backends must keep their ops table and priv valid for as
 * long as the mount exists. There is no vfs_unmount() yet.
 */
typedef struct {
    osnos_status_t (*stat)   (void *priv, const char *path,
                              vfs_stat_t *out);

    /*
     * readdir: enumerate one entry at a time.
     *   `cursor` is opaque backend-local state (e.g. a slot index for
     *   ramfs, a directory entry offset for a disk FS). The caller starts
     *   at 0 and increments based on the backend's protocol.
     *   On end of directory the backend returns OSNOS_ENOENT.
     *
     * The vfs layer is responsible for advancing cursor across calls and
     * presenting a clean iterator API to fs_server.
     */
    osnos_status_t (*readdir)(void *priv, const char *path, size_t cursor,
                              vfs_dirent_t *out, size_t *next_cursor);

    /*
     * Read up to buf_size bytes starting at file offset `off` into
     * `buf`. Backends that don't track real offsets (char devices,
     * sysfs) ignore off and always return their current stream
     * batch — sys_read handles those via the `is_chr` slot flag.
     *
     * Originally this took no offset and the caller (sys_read)
     * read the whole file into a heap scratch and sliced. That
     * was unusable for files > a few KB; FASE 11.0 (TCC) finally
     * needed offset-native reads so callers can request arbitrary
     * 8 KB chunks without copying 50 KB through the heap every
     * time.
     */
    osnos_status_t (*read)   (void *priv, const char *path,
                              size_t off,
                              char *buf, size_t buf_size,
                              size_t *out_size);

    osnos_status_t (*write)  (void *priv, const char *path,
                              const char *buf, size_t buf_size);

    osnos_status_t (*append) (void *priv, const char *path,
                              const char *buf, size_t buf_size);

    osnos_status_t (*mkdir)  (void *priv, const char *path);
    osnos_status_t (*rmdir)  (void *priv, const char *path);
    osnos_status_t (*unlink) (void *priv, const char *path);

    /*
     * Optional fast path. If non-null, the vfs layer prefers this over
     * read+write+unlink. Used when the backend can do an O(1) rename.
     * If null, vfs falls back to copy+unlink.
     */
    osnos_status_t (*rename) (void *priv, const char *src, const char *dst);
} vfs_ops_t;

/* ---------------------------------------------------------------- */
/* Mount table                                                      */
/* ---------------------------------------------------------------- */

#define VFS_MAX_MOUNTS 16   /* was 8; /home alias didn't fit. 16 leaves headroom for future mounts (/tmp, /proc, /var, etc.) */

typedef struct {
    char             mountpoint[OSNOS_PATH_MAX];  /* e.g. "/dev", "/" */
    const vfs_ops_t *ops;
    void            *priv;
    bool             used;
} vfs_mount_t;

/* ---------------------------------------------------------------- */
/* Public API (implemented in vfs.c — FASE 2)                       */
/* ---------------------------------------------------------------- */

void vfs_init(void);

osnos_status_t vfs_mount(const char *mountpoint,
                         const vfs_ops_t *ops,
                         void *priv);

/* Read-only mount slot accessor for introspection. NULL when unused. */
const vfs_mount_t *vfs_mount_slot(size_t idx);

osnos_status_t vfs_stat(const char *path, vfs_stat_t *out);

/*
 * Caller-side readdir loop:
 *   size_t cursor = 0;
 *   vfs_dirent_t ent;
 *   while (vfs_readdir(path, &cursor, &ent) == OSNOS_OK) { ... }
 */
osnos_status_t vfs_readdir(const char *path,
                           size_t *cursor,
                           vfs_dirent_t *out);

osnos_status_t vfs_read(const char *path,
                        char *buf, size_t buf_size,
                        size_t *out_size);

/* Offset-native read. Backend-direct path used by sys_read so we
 * don't have to slurp the whole file every time. Char devices
 * ignore `off`. */
osnos_status_t vfs_read_at(const char *path,
                           size_t off,
                           char *buf, size_t buf_size,
                           size_t *out_size);

osnos_status_t vfs_write(const char *path,
                         const char *buf, size_t buf_size);

osnos_status_t vfs_append(const char *path,
                          const char *buf, size_t buf_size);

osnos_status_t vfs_mkdir(const char *path);
osnos_status_t vfs_rmdir(const char *path);
osnos_status_t vfs_unlink(const char *path);

/*
 * Higher-level ops composed from primitives. When src and dst share a
 * mount and the backend offers .rename, that fast path is taken.
 */
osnos_status_t vfs_copy(const char *src, const char *dst);
osnos_status_t vfs_move(const char *src, const char *dst);

/* Touch = stat-then-write-empty-if-absent. */
osnos_status_t vfs_touch(const char *path);

/*
 * Convenience views built over vfs_readdir. They pack their output into
 * the caller-supplied buffer as null-terminated text suitable for the
 * shell to display. Return value is bytes written (excluding '\0').
 */
size_t vfs_list_dir(const char *path, char *out, size_t out_size);
size_t vfs_tree(const char *path, char *out, size_t out_size);

/*
 * Glob helpers. `pattern` is an absolute path whose last segment may
 * contain '*' (no '/' in the segment). Wildcards in intermediate segments
 * are not supported.
 *
 * Returns: number of matches.
 */
bool   vfs_path_has_wildcard(const char *path);
size_t vfs_glob_list  (const char *pattern, char *out, size_t out_size);
size_t vfs_glob_read  (const char *pattern, char *out, size_t out_size);
size_t vfs_glob_unlink(const char *pattern);

/* ---------------------------------------------------------------- */
/* Future (FASE 4 of ROADMAP — file descriptors)                    */
/* ---------------------------------------------------------------- */

/*
 *   typedef int vfs_fd_t;
 *
 *   vfs_fd_t        vfs_open    (const char *path, int flags);
 *   osnos_status_t  vfs_close   (vfs_fd_t fd);
 *   ssize_t         vfs_read_fd (vfs_fd_t fd, void *buf, size_t count);
 *   ssize_t         vfs_write_fd(vfs_fd_t fd, const void *buf, size_t count);
 *   off_t           vfs_lseek   (vfs_fd_t fd, off_t offset, int whence);
 *
 * The FD layer wraps a vfs_node + position. Per-task FD tables come
 * with FASE 6 (ELF/ring3); until then a single global table in fs_server
 * is fine.
 */
