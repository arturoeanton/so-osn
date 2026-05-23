/*
 * /bin/hello_musl — first program linked against vendored musl libc.
 *
 * End-to-end smoke test of the musl integration:
 *   1. crt1.o (musl) → __libc_start_main
 *   2. __init_libc reads auxv (AT_PAGESZ + AT_NULL from
 *      build_argv_block) — needs kernel exec.c patch
 *   3. __init_tls → __set_thread_area → arch_prctl(ARCH_SET_FS, ...)
 *      → wrmsr MSR_FS_BASE — needs kernel sys_arch_prctl
 *   4. main runs in musl's context (FS register loaded with TLS)
 *   5. write(), snprintf(), raw syscalls all work
 *   6. exit() runs musl's at_exit chain + flush + SYS_EXIT
 *
 * Known limitation: musl's printf/puts via the FILE* layer doesn't
 * surface output yet (returns error before reaching writev). Likely
 * an osnos-side issue with ioctl(TIOCGWINSZ) or the OFD lock model.
 * Tracked as next iteration — see STATUS.md. snprintf + raw write
 * are functional and prove libc itself works.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void emit(const char *s) {
    write(1, s, strlen(s));
}

int main(int argc, char **argv) {
    emit("============================================\n");
    emit("  hello from musl libc on osnos\n");
    emit("============================================\n");
    emit("argv contents (raw):\n");
    char buf[80];
    for (int i = 0; i < argc; i++) {
        int n = snprintf(buf, sizeof(buf),
                         "  argv[%d] = %s\n", i, argv[i]);
        write(1, buf, n);
    }
    /* musl's full snprintf: %f works (osnos minimal libc doesn't). */
    double pi = 3.14159265358979;
    int n = snprintf(buf, sizeof(buf),
                     "pi (musl %%f) = %.10f\n", pi);
    write(1, buf, n);
    /* musl's integer formatting with width / pad / hex. */
    n = snprintf(buf, sizeof(buf),
                 "hex: %08x  decimal: %10d  unsigned: %u\n",
                 0xdeadbeef, -42, 99u);
    write(1, buf, n);
    emit("end of musl smoke test — all good\n");
    return 0;
}
