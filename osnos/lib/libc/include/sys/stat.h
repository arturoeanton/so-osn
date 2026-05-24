#pragma once

#include <sys/types.h>

/*
 * struct stat — layout-compatible with osnos kernel's osnos_stat_t which
 * in turn matches Linux x86_64. New libc binaries see the same memory
 * layout as anything `osnos_stat_t` points at, so kernel writes flow
 * straight back to user without reinterpretation.
 */
/* struct timespec — Linux layout. tv_nsec is `long` (64-bit en x86_64)
 * para que el total sean 16 bytes y coincida con los pares
 * <time_t, uint64_t> del kernel `osnos_stat_t`. */
#ifndef _STRUCT_TIMESPEC_DEFINED
#define _STRUCT_TIMESPEC_DEFINED
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#endif

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    nlink_t   st_nlink;
    mode_t    st_mode;
    uid_t     st_uid;
    gid_t     st_gid;
    uint32_t  __pad0;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    int64_t   __unused[3];
};

/* POSIX.1-2001 legacy member names — Linux uses macros para que código
 * viejo (`st_mtime`) y nuevo (`st_mtim.tv_sec`) coexistan sin friction. */
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

int fstat(int fd, struct stat *out);

/*
 * stat(path, struct stat *out) — like fstat but takes a path.
 * Returns 0 on success, -1 + errno on failure (ENOENT if path
 * doesn't exist, EFAULT on bad pointer).
 */
int stat(const char *path, struct stat *out);

/* utimensat(2) — set file timestamps. osnos no track atime/mtime,
 * stub retorna 0 sin efecto. Declaramos con `void *` en vez de
 * `struct timespec*` para no requerir el include de <time.h> acá. */
int utimensat(int dirfd, const char *path, const void *times, int flags);
