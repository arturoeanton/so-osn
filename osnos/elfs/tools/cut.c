/*
 * tools/cut.c — slice fields out of each line.
 *
 *   cut -d DELIM -f LIST [file]
 *   cut -c LIST [file]
 *
 * LIST is a comma-separated list of 1-based field indices: "1",
 * "1,3", "2,4,7". Ranges ("1-3") and "-3" / "3-" are NOT supported
 * — keeps the parser tiny. Delimiter defaults to TAB.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FIELDS 16

static int  fields[MAX_FIELDS];
static int  nfields = 0;
static char delim   = '\t';
static int  by_char = 0;

static void parse_list(const char *s) {
    int v = 0;
    int seen = 0;
    for (; *s; s++) {
        if (*s == ',') {
            if (seen && nfields < MAX_FIELDS) fields[nfields++] = v;
            v = 0; seen = 0;
            continue;
        }
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0'); seen = 1;
        }
    }
    if (seen && nfields < MAX_FIELDS) fields[nfields++] = v;
}

static int wanted(int idx) {
    for (int i = 0; i < nfields; i++) if (fields[i] == idx) return 1;
    return 0;
}

static void process_line(const char *line) {
    if (by_char) {
        int len = (int)strlen(line);
        int first = 1;
        for (int i = 0; i < nfields; i++) {
            int c = fields[i] - 1;
            if (c >= 0 && c < len) {
                if (!first) putchar(' ');
                putchar(line[c]);
                first = 0;
            }
        }
        putchar('\n');
        return;
    }
    /* by delimiter */
    int idx = 1;
    const char *p = line;
    int first = 1;
    while (1) {
        const char *q = p;
        while (*q && *q != delim) q++;
        if (wanted(idx)) {
            if (!first) putchar(delim);
            for (const char *r = p; r < q; r++) putchar(*r);
            first = 0;
        }
        if (!*q) break;
        p = q + 1;
        idx++;
    }
    putchar('\n');
}

static void process_fd(int fd) {
    char line[1024];
    size_t pos = 0;
    char chunk[256];
    long n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || pos + 1 >= sizeof(line)) {
                line[pos] = 0;
                process_line(line);
                pos = 0;
                if (c != '\n') line[pos++] = c;
            } else line[pos++] = c;
        }
    }
    if (pos > 0) { line[pos] = 0; process_line(line); }
}

int main(int argc, char **argv) {
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[i + 1][0];
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            parse_list(argv[i + 1]); i += 2;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            by_char = 1; parse_list(argv[i + 1]); i += 2;
        } else { fprintf(stderr, "cut: bad flag %s\n", argv[i]); return 2; }
    }
    if (nfields == 0) {
        fprintf(stderr, "usage: cut [-d DELIM] -f LIST [file] | cut -c LIST [file]\n");
        return 2;
    }
    if (i >= argc) process_fd(0);
    else {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cut: %s: cannot open\n", argv[i]); return 1; }
        process_fd(fd);
        close(fd);
    }
    return 0;
}
