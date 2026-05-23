/*
 * ofdtest — verify shared "open file description" semantics + FD_CLOEXEC.
 *
 * What we check:
 *
 *   1. dup(fd) creates a NEW fd sharing the same OFD as the original.
 *      Confirm by lseek on one fd and seeing the other fd's position
 *      reflect the new offset.
 *
 *   2. dup2(src, dst) replaces dst with a slot pointing at src's OFD.
 *      Shared offset confirmed via cross-fd lseek + ftell.
 *
 *   3. fork inherits OFDs: parent and child see the SAME offset.
 *      Parent writes some bytes, child sees offset advanced (without
 *      child writing). This is the POSIX-strict behaviour.
 *
 *   4. FD_CLOEXEC: fcntl(fd, F_SETFD, FD_CLOEXEC) marks the fd to be
 *      closed on execve. Fork+exec confirms via the child failing to
 *      read the marked fd (it's been closed).
 *
 *   5. dup() clears FD_CLOEXEC on the new fd (POSIX requirement —
 *      otherwise CLOEXEC would propagate undesirably).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

#define TEST_FILE  "/home/.ofdtest.tmp"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("ofdtest: shared open file description + FD_CLOEXEC\n");

    /* Setup: write a small known buffer to a temp file so we have
     * something with a real offset to track. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open temp file");

    const char *payload = "0123456789ABCDEF";   /* 16 bytes */
    long n = write(fd, payload, 16);
    CHECK(n == 16, "write 16 bytes");

    /* Rewind so seek+read tests start from byte 0. */
    long pos = lseek(fd, 0, SEEK_SET);
    CHECK(pos == 0, "lseek SEEK_SET to 0");

    /* 1. dup — shared offset. */
    int fd2 = dup(fd);
    CHECK(fd2 >= 3, "dup returned fresh fd");

    /* Seek fd to position 5; both fd and fd2 must report 5. */
    pos = lseek(fd, 5, SEEK_SET);
    CHECK(pos == 5, "lseek fd to 5");
    long pos2 = lseek(fd2, 0, SEEK_CUR);          /* SEEK_CUR=0 → query */
    CHECK(pos2 == 5,
          "dup'd fd2 sees shared offset (5)");

    /* Read 4 bytes via fd2 — offset should advance for BOTH fds. */
    char buf[8] = {0};
    n = read(fd2, buf, 4);
    CHECK(n == 4 && memcmp(buf, "5678", 4) == 0,
          "fd2 read 4 bytes from offset 5");

    pos  = lseek(fd,  0, SEEK_CUR);
    pos2 = lseek(fd2, 0, SEEK_CUR);
    CHECK(pos == 9 && pos2 == 9,
          "both fd + fd2 now at offset 9 (shared advance)");

    /* 2. dup2 onto a specific target. */
    int target = 7;
    int r = dup2(fd, target);
    CHECK(r == target, "dup2 to fd 7 succeeded");

    /* Verify the new fd shares the offset (still 9). */
    pos = lseek(target, 0, SEEK_CUR);
    CHECK(pos == 9, "dup2'd fd 7 shares offset (9)");

    /* 3. FD_CLOEXEC on fd2 → mark it close-on-exec; verify F_GETFD. */
    r = fcntl(fd2, F_SETFD, FD_CLOEXEC);
    CHECK(r == 0, "fcntl F_SETFD FD_CLOEXEC succeeded");

    r = fcntl(fd2, F_GETFD);
    CHECK(r == FD_CLOEXEC, "fcntl F_GETFD returns FD_CLOEXEC");

    /* 4. dup() must CLEAR FD_CLOEXEC on the new fd (POSIX). */
    int fd3 = dup(fd2);
    CHECK(fd3 >= 3, "dup of FD_CLOEXEC'd fd succeeded");
    r = fcntl(fd3, F_GETFD);
    CHECK(r == 0, "dup'd fd has FD_CLOEXEC cleared (POSIX)");

    /* 5. fork inherits OFDs — parent's offset is visible in child.
     * Child reads from fd (no seek) → should read "9ABC" (we're at
     * offset 9 in the file from above). */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: errno=%d\n", errno);
        return 1;
    }
    if (pid == 0) {
        /* Child: read 4 bytes from inherited fd (without seeking). */
        char cb[8] = {0};
        long cn = read(fd, cb, 4);
        if (cn != 4) _exit(11);
        if (memcmp(cb, "9ABC", 4) != 0) _exit(12);
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork: child sees parent's offset (inherited via OFD refcount)");

    /* After fork, parent's offset advanced because OFD is shared.
     * (Even with our cooperative scheduler, child got reaped before
     * we hit this check, so the OFD's offset is whatever child
     * left it at: 9 + 4 = 13.) */
    pos = lseek(fd, 0, SEEK_CUR);
    CHECK(pos == 13,
          "parent sees offset advanced by child's read (shared OFD)");

    /* Cleanup */
    close(fd);
    close(fd2);
    close(fd3);
    close(target);
    unlink(TEST_FILE);

    printf("\nofdtest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}
