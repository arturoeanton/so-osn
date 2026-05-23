/*
 * tools/tail.c — last N lines of a file (or stdin).
 *
 *   tail file              — last 10 lines
 *   tail -n N file         — last N lines
 *   tail -f file           — print last 10 lines, then follow growth
 *                            (Ctrl+C to exit)
 *   tail file1 file2       — multiple files with headers
 *
 * Implementation: ring of N pointers into a single byte buffer
 * holding the file. Last N \n-delimited lines are emitted in order.
 * -f keeps the fd open and polls for new bytes every 200 ms.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TAIL_DEFAULT_N  10
#define TAIL_BUF_MAX    (64 * 1024)

static void tail_fd(int fd, int n) {
    /* Slurp up to TAIL_BUF_MAX. Truncate older bytes silently for
     * very long files (acceptable for now, matches `head -n` shape). */
    static char buf[TAIL_BUF_MAX];
    size_t total = 0;
    long got;
    while (total < sizeof(buf) &&
           (got = read(fd, buf + total, sizeof(buf) - total)) > 0) {
        total += (size_t)got;
    }

    /* Walk backwards to find the start of the n-th from last line. */
    if (total == 0) return;
    size_t end = total;
    /* Skip trailing \n so we don't count the final empty line. */
    if (end > 0 && buf[end - 1] == '\n') end--;
    int seen = 0;
    size_t start = 0;
    for (size_t i = end; i > 0; i--) {
        if (buf[i - 1] == '\n') {
            seen++;
            if (seen >= n) { start = i; break; }
        }
    }
    write(1, buf + start, total - start);
}

/* Block in 200 ms polls reading from `fd`, emitting any new bytes
 * to stdout. Returns only on read error. Exit normally via signal
 * (Ctrl+C → SIGINT → default exit 130). */
static void follow_fd(int fd) {
    static char buf[4096];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            write(1, buf, (size_t)n);
            continue;       /* drain any backlog without sleeping */
        }
        if (n == 0 || errno == EAGAIN || errno == EINTR) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 };
            nanosleep(&ts, 0);
            continue;
        }
        break;              /* hard error */
    }
}

int main(int argc, char **argv) {
    int n = TAIL_DEFAULT_N;
    int follow = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0) {
            follow = 1;
            i += 1;
        } else {
            fprintf(stderr, "tail: unknown flag %s\n", argv[i]);
            return 1;
        }
    }
    if (n < 1) n = 1;

    if (i >= argc) {
        /* No path → tail of stdin. -f on stdin is rare but legal —
         * keeps reading until EOF (which won't come on a TTY) or
         * Ctrl+C. */
        tail_fd(0, n);
        if (follow) follow_fd(0);
        return 0;
    }
    int multi = (argc - i) > 1;
    /* -f only applies to a single file (POSIX-ish — GNU tail -f with
     * multiple files multiplexes with headers; we keep it simple). */
    if (follow && multi) {
        fprintf(stderr, "tail: -f with multiple files not supported\n");
        return 1;
    }
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: cannot open\n", argv[i]);
            continue;
        }
        if (multi) printf("==> %s <==\n", argv[i]);
        tail_fd(fd, n);
        if (follow) follow_fd(fd);
        close(fd);
    }
    return 0;
}
