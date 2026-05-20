#pragma once

#include <stdint.h>

/*
 * Layout-compatible with Linux x86_64 `struct linux_dirent64`. The
 * header occupies 19 bytes; the variable-length `d_name` follows it
 * null-terminated. Each record's total size is rounded up to 8 bytes
 * so the next dirent in a getdents64 buffer is 8-byte aligned.
 *
 *   struct {
 *       uint64_t d_ino;
 *       int64_t  d_off;
 *       uint16_t d_reclen;
 *       uint8_t  d_type;
 *       char     d_name[];
 *   }
 *
 * Caller iterates by stepping `d_reclen` bytes at a time:
 *
 *   char buf[1024]; size_t pos = 0;
 *   int64_t n = sys_getdents(fd, buf, sizeof buf);
 *   while (pos < (size_t)n) {
 *       osnos_dirent_t *d = (osnos_dirent_t *)(buf + pos);
 *       use(d->d_name, d->d_type);
 *       pos += d->d_reclen;
 *   }
 */

typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed)) osnos_dirent_t;

/* d_type values mirror Linux: <dirent.h> DT_* */
#define OSNOS_DT_UNKNOWN  0
#define OSNOS_DT_FIFO     1
#define OSNOS_DT_CHR      2
#define OSNOS_DT_DIR      4
#define OSNOS_DT_BLK      6
#define OSNOS_DT_REG      8
#define OSNOS_DT_LNK     10
#define OSNOS_DT_SOCK    12
