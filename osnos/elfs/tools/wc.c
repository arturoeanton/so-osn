/*
 * tools/wc.c — count lines, words, characters from stdin or files.
 *
 * Usage:
 *   wc                 # read stdin
 *   wc -l              # only lines
 *   wc -w              # only words
 *   wc -c              # only chars
 *   wc file1 file2 ... # one summary per file
 *
 * "Word" = run of non-whitespace separated by whitespace, POSIX-ish.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct { long lines, words, chars; } counts_t;

static void count_fd(int fd, counts_t *c) {
    char buf[1024];
    int  in_word = 0;
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char b = buf[i];
            c->chars++;
            if (b == '\n') c->lines++;
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
}

int main(int argc, char **argv) {
    int want_l = 0, want_w = 0, want_c = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l') want_l = 1;
            else if (*p == 'w') want_w = 1;
            else if (*p == 'c') want_c = 1;
            else { fprintf(stderr, "wc: unknown flag -%c\n", *p); return 1; }
        }
        i++;
    }
    if (!want_l && !want_w && !want_c) {
        want_l = want_w = want_c = 1;
    }

    counts_t total = {0,0,0};
    int n_files = argc - i;

    if (n_files == 0) {
        counts_t c = {0,0,0};
        count_fd(0, &c);
        if (want_l) printf("%ld ", c.lines);
        if (want_w) printf("%ld ", c.words);
        if (want_c) printf("%ld ", c.chars);
        printf("\n");
        return 0;
    }

    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: cannot open\n", argv[i]);
            continue;
        }
        counts_t c = {0,0,0};
        count_fd(fd, &c);
        close(fd);
        if (want_l) printf("%ld ", c.lines);
        if (want_w) printf("%ld ", c.words);
        if (want_c) printf("%ld ", c.chars);
        printf("%s\n", argv[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.chars += c.chars;
    }
    if (n_files > 1) {
        if (want_l) printf("%ld ", total.lines);
        if (want_w) printf("%ld ", total.words);
        if (want_c) printf("%ld ", total.chars);
        printf("total\n");
    }
    return 0;
}
