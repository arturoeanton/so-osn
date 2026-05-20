#pragma once

/*
 * Errno values — Linux x86_64 exact match. Mirror of osnos_status.h
 * on the kernel side. Stays a global int because no threads exist
 * yet; FASE 10 onwards may make it thread-local.
 */
extern int errno;

#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EIO             5
#define E2BIG           7
#define EBADF           9
#define EAGAIN         11
#define ENOMEM         12
#define EACCES         13
#define EFAULT         14
#define EBUSY          16
#define EEXIST         17
#define ENOTDIR        20
#define EISDIR         21
#define EINVAL         22
#define ENFILE         23
#define EMFILE         24
#define ENOSPC         28
#define EROFS          30
#define ENAMETOOLONG   36
#define ENOTEMPTY      39
#define ENOSYS         38
#define ELOOP          40

/* Networking placeholders (Linux numbers). Functions that need them
 * are declared but return -ENOSYS until FASE 8.5 lands the stack. */
#define ENOTSOCK       88
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT   97
#define EADDRINUSE     98
#define EADDRNOTAVAIL  99
#define ENETDOWN      100
#define ECONNRESET    104
#define ETIMEDOUT     110
#define ECONNREFUSED  111
