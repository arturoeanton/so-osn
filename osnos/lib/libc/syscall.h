#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Internal syscall helpers used by the libc wrappers. Linux x86_64
 * numbering — must stay in lockstep with src/micro/syscall.h on the
 * kernel side.
 *
 * Return convention: the kernel sticks the result in RAX. >=0 is the
 * real return; <0 is -errno. The wrappers in unistd.c / fcntl.c /
 * stdlib.c translate the negative case into setting `errno` and
 * returning -1 (or NULL / (void*)-1 / EOF depending on the API).
 */

#define SYS_READ       0
#define SYS_WRITE      1
#define SYS_OPEN       2
#define SYS_CLOSE      3
#define SYS_STAT       4
#define SYS_FSTAT      5
#define SYS_ACCESS    21
#define SYS_LSEEK      8
#define SYS_BRK       12
#define SYS_IOCTL     16
#define SYS_EXECVE    59
#define SYS_NANOSLEEP 35
#define SYS_TIME     201
#define SYS_CLOCK_GETTIME 228
#define SYS_GETPID    39
#define SYS_EXIT      60
#define SYS_KILL      62
#define SYS_GETCWD    79
#define SYS_CHDIR     80
#define SYS_RENAME    82
#define SYS_MKDIR     83
#define SYS_RMDIR     84
#define SYS_UNLINK    87
#define SYS_ISATTY   250   /* osnos-specific (above 250 to dodge Linux #201/#228) */
#define SYS_GETDENTS 217
#define SYS_SELECT     23
#define SYS_SOCKET     41
#define SYS_CONNECT    42
#define SYS_ACCEPT     43
#define SYS_SENDTO     44
#define SYS_RECVFROM   45
#define SYS_BIND       49
#define SYS_LISTEN     50
#define SYS_SETSOCKOPT 54

static inline long osnos_syscall0(long n) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
    return r;
}

static inline long osnos_syscall1(long n, long a) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}

static inline long osnos_syscall2(long n, long a, long b) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline long osnos_syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline long osnos_syscall4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10") = d;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}

static inline long osnos_syscall5(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

static inline long osnos_syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return r;
}
