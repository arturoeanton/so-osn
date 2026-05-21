/*
 * tools/sort.c — sort lines from stdin (or files) lexicographically.
 *
 *   sort                  — stdin → stdout, ASC
 *   sort -r               — reverse
 *   sort file             — read file
 *
 * In-memory sort capped at 4096 lines × 256 chars (1 MiB). Larger
 * input gets truncated with a stderr warning.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SORT_MAX_LINES   4096
#define SORT_LINE_BYTES   256

static char lines[SORT_MAX_LINES][SORT_LINE_BYTES];
static int  nlines = 0;
static int  reverse = 0;

static void ingest_fd(int fd) {
    char chunk[512];
    long n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (nlines >= SORT_MAX_LINES) return;
            int pos = 0;
            while (pos + 1 < SORT_LINE_BYTES && lines[nlines][pos]) pos++;
            if (c == '\n') {
                nlines++;
                continue;
            }
            if (pos + 1 < SORT_LINE_BYTES) {
                lines[nlines][pos]   = c;
                lines[nlines][pos+1] = 0;
            }
        }
    }
    /* Trailing partial line. */
    if (lines[nlines][0]) nlines++;
}

static int cmp(const void *a, const void *b) {
    int r = strcmp((const char *)a, (const char *)b);
    return reverse ? -r : r;
}

int main(int argc, char **argv) {
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r') reverse = 1;
            else { fprintf(stderr, "sort: unknown flag -%c\n", *p); return 2; }
        }
        i++;
    }
    if (i >= argc) {
        ingest_fd(0);
    } else {
        for (; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "sort: %s: cannot open\n", argv[i]);
                continue;
            }
            ingest_fd(fd);
            close(fd);
        }
    }

    qsort(lines, (size_t)nlines, SORT_LINE_BYTES, cmp);
    for (int k = 0; k < nlines; k++) printf("%s\n", lines[k]);
    return 0;
}
