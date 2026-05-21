/*
 * tests/user_hello.c — minimal ring-3 program loaded by the kernel's
 * ELF parser. No libc, no crt0: we write a fixed string and exit via
 * `syscall` directly.
 *
 * Build: compiled by the kernel Makefile target `user_hello.elf`,
 * embedded into the kernel image via objcopy, and registered as
 * /bin/hello_elf in the builtin table.
 *
 * Linker script user_hello.lds places .text/.rodata at 0x400000 so
 * the resulting ELF carries an ET_EXEC e_entry there with one PT_LOAD
 * segment.
 */

#include <stddef.h>

#define SYS_WRITE 1
#define SYS_EXIT  60

static long sys_write(int fd, const void *buf, unsigned long len) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(SYS_WRITE), "D"(fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

__attribute__((noreturn))
static void sys_exit(int code) {
    __asm__ volatile (
        "syscall"
        :
        : "a"(SYS_EXIT), "D"(code)
        : "rcx", "r11", "memory"
    );
    __builtin_unreachable();
}

static unsigned long s_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

__attribute__((noreturn))
void _start(void) {
    static const char msg[] = "hello from /bin/hello_elf (ELF64, ring 3)\n";
    sys_write(1, msg, s_strlen(msg));
    sys_exit(0);
}
