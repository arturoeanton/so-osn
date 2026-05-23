#include <errno.h>
#include <sys/mman.h>

#include "syscall.h"

void *mmap(void *addr, size_t length, int prot, int flags,
            int fd, off_t offset) {
    long r = osnos_syscall6(SYS_MMAP,
                              (long)addr, (long)length,
                              (long)prot, (long)flags,
                              (long)fd,   (long)offset);
    if (r < 0) { errno = (int)(-r); return MAP_FAILED; }
    return (void *)r;
}

int munmap(void *addr, size_t length) {
    long r = osnos_syscall2(SYS_MUNMAP, (long)addr, (long)length);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* osnos doesn't enforce W^X on user pages — once mapped writeable
 * they're already executable. mprotect succeeds as a no-op so JIT
 * callers (e.g. TCC's `-run`) don't bail spuriously. */
int mprotect(void *addr, size_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    return 0;
}
