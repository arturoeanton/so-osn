/*
 * /bin/head — print the first N lines from stdin (or N files).
 *
 *   head             # first 10 lines of stdin
 *   head -n 3        # first 3 lines of stdin
 *   head FILE        # first 10 lines of FILE
 *
 * Useful for testing pipes: `cat /home/o.txt | head -n 1`.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int head_stream(int fd, int max_lines) {
    char buf[256];
    int lines = 0;
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        long i = 0;
        while (i < n && lines < max_lines) {
            /* Print up to (and including) the next '\n'. */
            long j = i;
            while (j < n && buf[j] != '\n') j++;
            int got_nl = (j < n);
            long span = got_nl ? j - i + 1 : j - i;
            write(1, buf + i, (unsigned long)span);
            i = j + (got_nl ? 1 : 0);
            if (got_nl) lines++;
        }
        if (lines >= max_lines) break;
    }
    return 0;
}

int main(int argc, char **argv) {
    int max_lines = 10;
    int argi = 1;
    if (argi < argc && argv[argi][0] == '-' && argv[argi][1] == 'n') {
        if (++argi >= argc) {
            fprintf(stderr, "head: -n needs a count\n");
            return 1;
        }
        max_lines = atoi(argv[argi++]);
        if (max_lines <= 0) max_lines = 10;
    }

    if (argi >= argc) {
        return head_stream(0, max_lines);
    }
    int rc = 0;
    for (; argi < argc; argi++) {
        int fd = open(argv[argi], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "head: cannot open %s\n", argv[argi]);
            rc = 1;
            continue;
        }
        head_stream(fd, max_lines);
        close(fd);
    }
    return rc;
}
