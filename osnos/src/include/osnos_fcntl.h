#pragma once

/*
 * open() flags. Numeric values match Linux x86_64 fcntl.h so that ELF
 * binaries built against glibc/newlib pass the same constants.
 *
 * Access mode lives in the lower 2 bits; everything else is bit-OR'd on
 * top.
 */
#define O_RDONLY    00
#define O_WRONLY    01
#define O_RDWR      02
#define O_ACCMODE   03

#define O_CREAT     0100        /* create if missing */
#define O_EXCL      0200        /* error if O_CREAT and file exists */
#define O_TRUNC     01000       /* truncate to length 0 */
#define O_APPEND    02000       /* writes append at end of file */
#define O_NONBLOCK  04000       /* non-blocking (unused today) */

/* lseek whence values. */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2
