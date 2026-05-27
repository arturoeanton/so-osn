/* ox_errno_shim.c — only used when ox.c is recompiled against musl
 * (oxnetsurf, etc.). mini-libc declares `extern int errno;` as a
 * plain global symbol, while musl exposes `(*__errno_location())`.
 * When the ox_*.c sources are pulled into a musl-linked binary,
 * their references to `errno` resolve to neither — link fails with
 * "undefined symbol: errno".
 *
 * Provide the global here so those references link. The musl side
 * keeps using its own __errno_location for everything else; this
 * symbol exists only to satisfy the shim's writes (oxsrv error
 * channel) and reads (EAGAIN check on poll/recv). The two stores
 * are independent — a refactor to use musl's __errno_location
 * is cheap once we need real cross-libc errno consistency.
 *
 * The shim is included in liboxshim.a (NetSurf-side ox client).
 * Building it into libosnos_c.a is harmless because mini-libc
 * already declares the same symbol elsewhere; the linker dedups.
 */
int errno = 0;

/* --- POSIX shm_open / shm_unlink override ---------------------------
 * musl's shm_open does open("/dev/shm/<name>") via the regular VFS.
 * osnos has no /dev/shm — the equivalent is the kernel-private shm
 * object table reached via syscalls SYS_SHM_OPEN(519)/SHM_UNLINK(520).
 * Mini-libc wraps those directly (lib/libc/mman.c); for musl-linked
 * binaries we need to override musl's POSIX impl with a thin syscall.
 *
 * Link order in oxnetsurf.elf places liboxshim.a BEFORE libc.a (musl),
 * so ld.lld picks these definitions first.
 */
#include <sys/types.h>

static long oxshim_syscall3(long n, long a, long b, long c) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(n), "D"(a), "S"(b), "d"(c),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
static long oxshim_syscall1(long n, long a) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(n), "D"(a),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#define OXSHIM_SYS_SHM_OPEN    519
#define OXSHIM_SYS_SHM_UNLINK  520

int shm_open(const char *name, int oflag, mode_t mode) {
    long r = oxshim_syscall3(OXSHIM_SYS_SHM_OPEN, (long)name,
                              (long)oflag, (long)mode);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int shm_unlink(const char *name) {
    long r = oxshim_syscall1(OXSHIM_SYS_SHM_UNLINK, (long)name);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* --- connect() retry-on-EINPROGRESS override ------------------------
 * osnos's kernel sys_connect is non-blocking: SYN sent → returns
 * EINPROGRESS=115. Mini-libc wraps that with a retry+nanosleep loop;
 * musl's connect does NOT retry (POSIX semantics expect the kernel to
 * block). Provide a wrapping connect() so musl-linked code on osnos
 * gets the same retry behaviour as mini-libc.
 *
 * We also retry on EAGAIN=11 (which has been observed empirically —
 * the SYN_SENT slot transiently returns EAGAIN before flipping to
 * ECONNREFUSED). 10 ms backoff, 500 attempts → 5 s total cap.
 */
#define OXSHIM_SYS_CONNECT          42
#define OXSHIM_SYS_NANOSLEEP        35
#define OXSHIM_EAGAIN               11
#define OXSHIM_EINPROGRESS         115

struct oxshim_timespec { long tv_sec; long tv_nsec; };

static long oxshim_syscall2(long n, long a, long b) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(n), "D"(a), "S"(b),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

int connect(int fd, const void *addr, unsigned int len) {
    for (int i = 0; i < 500; i++) {
        long r = oxshim_syscall3(OXSHIM_SYS_CONNECT, (long)fd,
                                  (long)addr, (long)len);
        if (r == 0) return 0;
        long e = -r;
        if (e != OXSHIM_EINPROGRESS && e != OXSHIM_EAGAIN) {
            errno = (int)e;
            return -1;
        }
        struct oxshim_timespec t = { 0, 10L * 1000L * 1000L };
        oxshim_syscall2(OXSHIM_SYS_NANOSLEEP, (long)&t, 0);
    }
    errno = OXSHIM_EINPROGRESS;
    return -1;
}
