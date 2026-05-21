/*
 * /bin/mmaptest — smoke-test anonymous mmap/munmap.
 *
 *   - allocate 3 pages, scribble each byte, read back, verify.
 *   - munmap, allocate another region same size, verify zero-init.
 *   - try a too-big mmap to confirm clean ENOMEM.
 *
 * Counts PASS/FAIL. Returns 0 if all pass.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int pass_count, fail_count;

static void check(const char *label, int cond) {
    if (cond) { printf("  PASS %s\n", label); pass_count++; }
    else      { printf("  FAIL %s\n", label); fail_count++; }
}

int main(void) {
    printf("== mmap anonymous smoke test ==\n");

    /* 1. Basic alloc + write + read back. */
    size_t len = 3 * 4096;
    void *p = mmap(0, len, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("mmap-non-failed", p != MAP_FAILED);

    if (p != MAP_FAILED) {
        unsigned char *q = (unsigned char *)p;
        /* Zero-init guarantee. */
        check("mmap-zero[0]",       q[0]       == 0);
        check("mmap-zero[mid]",     q[len / 2] == 0);
        check("mmap-zero[last]",    q[len - 1] == 0);

        /* Write all bytes (touches every page). */
        for (size_t i = 0; i < len; i++) q[i] = (unsigned char)(i & 0xFF);
        check("mmap-write[0]",       q[0] == 0);
        check("mmap-write[4096]",    q[4096] == 0);   /* (i & 0xFF) at i=4096 wraps to 0 */
        check("mmap-write[mid]",     q[len / 2] == (unsigned char)((len / 2) & 0xFF));
        check("mmap-write[last]",    q[len - 1] == (unsigned char)((len - 1) & 0xFF));

        check("munmap",              munmap(p, len) == 0);
    }

    /* 2. Subsequent mmap returns fresh zero memory. */
    void *p2 = mmap(0, 4096, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("mmap2-non-failed", p2 != MAP_FAILED);
    if (p2 != MAP_FAILED) {
        unsigned char *q2 = (unsigned char *)p2;
        check("mmap2-zero",       q2[0] == 0 && q2[4095] == 0);
        munmap(p2, 4096);
    }

    /* 3. File-backed mmap → ENOSYS. */
    errno = 0;
    void *p3 = mmap(0, 4096, PROT_READ, MAP_PRIVATE, 0, 0);
    check("mmap-filebacked-enosys",
          p3 == MAP_FAILED && errno == ENOSYS);

    printf("== %d passed, %d failed ==\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
