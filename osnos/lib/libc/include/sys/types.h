#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int64_t  off_t;
typedef int64_t  ssize_t;
typedef uint32_t mode_t;
typedef int32_t  pid_t;
typedef uint64_t ino_t;
typedef uint64_t dev_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint64_t nlink_t;   /* Linux x86_64: 8 bytes, matches kernel osnos_stat_t */
typedef int64_t  blksize_t;
typedef int64_t  blkcnt_t;
typedef int64_t  time_t;
