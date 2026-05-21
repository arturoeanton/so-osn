/*
 * tools/uniq.c — dedupe consecutive identical lines from stdin or file.
 *
 *   uniq                 — drop adjacent duplicates
 *   uniq -c              — prefix each line with its count
 *   uniq -d              — only show repeated lines
 *
 * Like POSIX uniq, only ADJACENT duplicates are squashed. Pipe to
 * `sort` first if you want global dedup.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int show_count = 0;
static int dups_only  = 0;

static void emit(const char *line, int count) {
    if (count == 0) return;
    if (dups_only && count <= 1) return;
    if (show_count) printf("%4d %s\n", count, line);
    else            printf("%s\n", line);
}

static void process_fd(int fd) {
    char prev[1024];
    char cur[1024];
    prev[0] = 0;
    int prev_count = 0;
    size_t pos = 0;
    char chunk[256];
    long n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || pos + 1 >= sizeof(cur)) {
                cur[pos] = 0;
                if (prev_count == 0) {
                    /* First line ever — buffer it. */
                    for (size_t j = 0; cur[j]; j++) prev[j] = cur[j];
                    prev[pos] = 0;
                    prev_count = 1;
                } else if (strcmp(prev, cur) == 0) {
                    prev_count++;
                } else {
                    emit(prev, prev_count);
                    for (size_t j = 0; j <= pos; j++) prev[j] = cur[j];
                    prev_count = 1;
                }
                pos = 0;
                if (c != '\n') cur[pos++] = c;
            } else {
                cur[pos++] = c;
            }
        }
    }
    if (pos > 0) {
        cur[pos] = 0;
        if (prev_count > 0 && strcmp(prev, cur) == 0) prev_count++;
        else {
            emit(prev, prev_count);
            for (size_t j = 0; j <= pos; j++) prev[j] = cur[j];
            prev_count = 1;
        }
    }
    emit(prev, prev_count);
}

int main(int argc, char **argv) {
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'c') show_count = 1;
            else if (*p == 'd') dups_only = 1;
            else { fprintf(stderr, "uniq: unknown flag -%c\n", *p); return 2; }
        }
        i++;
    }
    if (i >= argc) {
        process_fd(0);
    } else {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "uniq: %s: cannot open\n", argv[i]); return 1; }
        process_fd(fd);
        close(fd);
    }
    return 0;
}
