#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "syscall.h"

/*
 * DIR handle: holds the underlying fd plus a buffer where getdents
 * dumps raw kernel records. We parse them one at a time and copy each
 * into a stable struct dirent that the caller reads through readdir's
 * return pointer.
 */
struct DIR {
    int            fd;
    size_t         buf_pos;
    size_t         buf_len;
    struct dirent  ent;
    char           buf[1024];
};

/*
 * Raw record laid down by SYS_GETDENTS (Linux linux_dirent64 layout).
 * d_name is variable-length; the record's total size is in d_reclen.
 *
 * Packed because the kernel emits the prefix at offset 19 (8 + 8 + 2 + 1)
 * without padding — without `packed` the struct's natural layout would
 * push d_name to offset 24 and we would mis-parse the name.
 */
struct __attribute__((packed)) raw_dirent {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

DIR *opendir(const char *path) {
    /* POSIX: opendir must fail with ENOTDIR if `path` exists but
     * isn't a directory. Without this check ls expanded globs of
     * regular files silently produced no output: open() succeeded
     * on the file, readdir/getdents saw 0 entries and returned NULL
     * — caller couldn't tell "empty dir" apart from "wrong kind of
     * fd". Stat first; fail loudly on non-directory. */
    struct stat st;
    if (stat(path, &st) != 0) return 0;                /* errno set by stat */
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return 0; }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;

    DIR *d = malloc(sizeof(DIR));
    if (!d) { close(fd); errno = ENOMEM; return 0; }

    d->fd      = fd;
    d->buf_pos = 0;
    d->buf_len = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return 0; }

    if (d->buf_pos >= d->buf_len) {
        long r = osnos_syscall3(SYS_GETDENTS, d->fd, (long)d->buf, (long)sizeof(d->buf));
        if (r < 0) { errno = (int)(-r); return 0; }
        if (r == 0) return 0;                           /* end of directory */
        d->buf_pos = 0;
        d->buf_len = (size_t)r;
    }

    struct raw_dirent *raw = (struct raw_dirent *)(d->buf + d->buf_pos);

    d->ent.d_ino    = raw->d_ino;
    d->ent.d_off    = raw->d_off;
    d->ent.d_reclen = raw->d_reclen;
    d->ent.d_type   = raw->d_type;

    /* d_name fits within reclen minus the fixed-prefix bytes; cap at 255. */
    size_t name_max = raw->d_reclen - (sizeof(struct raw_dirent));
    if (name_max > 255) name_max = 255;
    size_t i = 0;
    for (; i < name_max && raw->d_name[i]; i++) d->ent.d_name[i] = raw->d_name[i];
    d->ent.d_name[i] = 0;

    d->buf_pos += raw->d_reclen;
    return &d->ent;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    int rc = close(d->fd);
    free(d);
    return rc;
}
