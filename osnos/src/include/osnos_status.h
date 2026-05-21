#pragma once

#include <stdint.h>

/*
 * OSnOS status / errno codes.
 *
 * Numeric values match Linux x86_64 errno (asm-generic/errno-base.h, errno.h)
 * so that future Linux ELF binaries can interpret syscall return values
 * unmodified. Do not invent new numbers in the Linux errno range; if osnos
 * needs to express something Linux does not, either alias to the closest
 * Linux code or reserve a value above 200.
 */
typedef enum {
    OSNOS_OK             = 0,

    OSNOS_EPERM          = 1,   /* operation not permitted */
    OSNOS_ENOENT         = 2,   /* no such file or directory */
    OSNOS_ESRCH          = 3,   /* no such process / target */
    OSNOS_EINTR          = 4,   /* interrupted system call */
    OSNOS_EIO            = 5,   /* i/o error */
    OSNOS_E2BIG          = 7,   /* argument list too long */
    OSNOS_EBADF          = 9,   /* bad file descriptor */
    OSNOS_EAGAIN         = 11,  /* try again (used for queue full) */
    OSNOS_ENOMEM         = 12,  /* out of memory */
    OSNOS_EACCES         = 13,  /* permission denied */
    OSNOS_EFAULT         = 14,  /* bad address */
    OSNOS_EBUSY          = 16,  /* resource busy */
    OSNOS_EEXIST         = 17,  /* file exists */
    OSNOS_ENOTDIR        = 20,  /* not a directory */
    OSNOS_EISDIR         = 21,  /* is a directory */
    OSNOS_EINVAL         = 22,  /* invalid argument */
    OSNOS_ENFILE         = 23,  /* file table overflow */
    OSNOS_EMFILE         = 24,  /* too many open files */
    OSNOS_ENOSPC         = 28,  /* no space left on device */
    OSNOS_EROFS          = 30,  /* read-only file system */
    OSNOS_ENAMETOOLONG   = 36,  /* file name too long */
    OSNOS_ENOTEMPTY      = 39,  /* directory not empty */
    /* Networking (Linux errno-base) — used by the socket syscalls. */
    OSNOS_ENOTSOCK       = 88,  /* not a socket */
    OSNOS_EPROTONOSUPPORT= 93,  /* protocol not supported */
    OSNOS_EAFNOSUPPORT   = 97,  /* address family not supported */
    OSNOS_EADDRINUSE     = 98,  /* address already in use */
    OSNOS_EADDRNOTAVAIL  = 99,
    OSNOS_ENETDOWN       = 100,
    OSNOS_ECONNRESET     = 104,
    OSNOS_ENOTTY         = 25,  /* not a typewriter (bad ioctl) */
    OSNOS_ETIMEDOUT      = 110,
    OSNOS_ECONNREFUSED   = 111,
    OSNOS_EINPROGRESS    = 115  /* connect() still negotiating */
} osnos_status_t;
