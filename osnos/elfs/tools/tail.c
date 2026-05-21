/*
 * tools/tail.c — last N lines of a file (or stdin).
 *
 *   tail file              — last 10 lines
 *   tail -n N file         — last N lines
 *   tail file1 file2       — multiple files with headers
 *
 * Implementation: ring of N pointers into a single byte buffer
 * holding the file. Last N \n-delimited lines are emitted in order.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv) {
    int n = TAIL_DEFAULT_N;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atoi(argv[i + 1]);
            i += 2;
        } else {
            fprintf(stderr, "tail: unknown flag %s\n", argv[i]);
            return 1;
        }
    }
    if (n < 1) n = 1;

    if (i >= argc) {
        tail_fd(0, n);
        return 0;
    }
    int multi = (argc - i) > 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: cannot open\n", argv[i]);
            continue;
        }
        if (multi) printf("==> %s <==\n", argv[i]);
        tail_fd(fd, n);
        close(fd);
    }
    return 0;
}
