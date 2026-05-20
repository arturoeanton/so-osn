#pragma once

#include <stdint.h>

/*
 * Layout-compatible with Linux x86_64 `struct stat` (asm-generic/stat.h).
 * Newlib's sys/stat.h reads st_mode, st_size, st_blksize, st_blocks,
 * st_ino — all those land at the same offsets here.
 *
 * Fields we don't populate yet are zeroed (uid/gid/timestamps wait for
 * a user model + clock).
 */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;

    uint32_t st_mode;       /* type (S_IF*) | permission bits */
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;

    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;

    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;

    int64_t  __unused[3];
} osnos_stat_t;
