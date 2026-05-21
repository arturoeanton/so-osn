/*
 * tools/grep.c — print lines containing a substring.
 *
 *   grep PATTERN              — read stdin
 *   grep PATTERN file1 file2  — search each file
 *   grep -v PATTERN ...       — invert (lines NOT matching)
 *   grep -i PATTERN ...       — case insensitive
 *   grep -n PATTERN ...       — prefix line number
 *
 * Plain substring search (no regex). Matches what 95% of shell
 * scripts use.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int  invert = 0;
static int  ignore_case = 0;
static int  show_lineno = 0;
static int  multi_file = 0;

static char lower(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int contains(const char *line, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (const char *p = line; *p; p++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = p[i];
            char b = needle[i];
            if (!a) return 0;
            if (ignore_case) { a = lower(a); b = lower(b); }
            if (a != b) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

static void search_fd(int fd, const char *needle, const char *fname) {
    char line[1024];
    size_t pos = 0;
    long lineno = 0;
    char chunk[256];
    long n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || pos + 1 >= sizeof(line)) {
                line[pos] = 0;
                lineno++;
                int match = contains(line, needle);
                if (invert) match = !match;
                if (match) {
                    if (multi_file) printf("%s:", fname);
                    if (show_lineno) printf("%ld:", lineno);
                    printf("%s\n", line);
                }
                pos = 0;
                if (c != '\n') line[pos++] = c;
            } else {
                line[pos++] = c;
            }
        }
    }
    if (pos > 0) {
        line[pos] = 0;
        lineno++;
        int match = contains(line, needle);
        if (invert) match = !match;
        if (match) {
            if (multi_file) printf("%s:", fname);
            if (show_lineno) printf("%ld:", lineno);
            printf("%s\n", line);
        }
    }
}

int main(int argc, char **argv) {
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'v') invert = 1;
            else if (*p == 'i') ignore_case = 1;
            else if (*p == 'n') show_lineno = 1;
            else { fprintf(stderr, "grep: unknown flag -%c\n", *p); return 2; }
        }
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: grep [-vin] PATTERN [FILE...]\n");
        return 2;
    }
    const char *needle = argv[i++];

    if (i >= argc) {
        search_fd(0, needle, "(stdin)");
        return 0;
    }
    multi_file = (argc - i) > 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "grep: %s: cannot open\n", argv[i]);
            continue;
        }
        search_fd(fd, needle, argv[i]);
        close(fd);
    }
    return 0;
}
