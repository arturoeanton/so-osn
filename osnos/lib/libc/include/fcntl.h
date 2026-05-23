#pragma once

#include <sys/types.h>

/*
 * open(2) flags — Linux x86_64 numeric values. Matches
 * src/include/osnos_fcntl.h on the kernel side.
 */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003

#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800

int open(const char *path, int flags, ...);

/*
 * fcntl(2) — minimal cmd set:
 *   F_DUPFD (0) — like dup, but min fd is taken from `arg`.
 *   F_GETFD (1) — get per-fd flags (FD_CLOEXEC).
 *   F_SETFD (2) — set per-fd flags. Only FD_CLOEXEC is honoured.
 *   F_GETFL (3) — returns the fd's open flags.
 *   F_SETFL (4) — updates O_APPEND / O_NONBLOCK only.
 *
 * FD_CLOEXEC: when set on an fd, that fd is closed automatically
 * on execve(2). Crucial for inheritable file descriptors: a parent
 * can mark "do not pass to my children" before exec.
 */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

/* Per-fd flags (F_GETFD / F_SETFD). */
#define FD_CLOEXEC 1

int fcntl(int fd, int cmd, ...);
