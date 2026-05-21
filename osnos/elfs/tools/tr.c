/*
 * tools/tr.c — translate / delete characters from stdin.
 *
 *   tr SET1 SET2          — map each char in SET1 to corresponding in SET2
 *   tr -d SET             — delete chars in SET
 *
 * Ranges like a-z work; no escape sequences other than \n \t \\.
 * Stdin-only (no file args).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned char tab[256];   /* map or delete-bitmap */

static void expand_set(const char *s, char *out, int *n_out) {
    int n = 0;
    while (*s) {
        char a = *s++;
        if (a == '\\') {
            if (*s == 'n') { a = '\n'; s++; }
            else if (*s == 't') { a = '\t'; s++; }
            else if (*s == '\\') { a = '\\'; s++; }
        }
        if (s[0] == '-' && s[1] && s[1] != 0) {
            char b = s[1];
            s += 2;
            for (char c = a; c <= b && n < 255; c++) out[n++] = c;
        } else {
            if (n < 255) out[n++] = a;
        }
    }
    out[n] = 0;
    *n_out = n;
}

int main(int argc, char **argv) {
    int delete = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-d") == 0) { delete = 1; i++; }

    char set1[256];
    int  n1 = 0;
    if (i >= argc) { fprintf(stderr, "usage: tr [-d] SET1 [SET2]\n"); return 2; }
    expand_set(argv[i++], set1, &n1);

    char set2[256];
    int  n2 = 0;
    if (!delete) {
        if (i >= argc) { fprintf(stderr, "tr: SET2 required\n"); return 2; }
        expand_set(argv[i++], set2, &n2);
    }

    if (delete) {
        for (int k = 0; k < 256; k++) tab[k] = 0;
        for (int k = 0; k < n1; k++) tab[(unsigned char)set1[k]] = 1;
        char buf[1024]; long n;
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            char out[1024]; int op = 0;
            for (long j = 0; j < n; j++) {
                unsigned char c = (unsigned char)buf[j];
                if (!tab[c]) out[op++] = (char)c;
            }
            if (op > 0) write(1, out, (size_t)op);
        }
        return 0;
    }

    /* Translate. */
    for (int k = 0; k < 256; k++) tab[k] = (unsigned char)k;
    for (int k = 0; k < n1; k++) {
        char to = (k < n2) ? set2[k] : set2[n2 - 1];
        tab[(unsigned char)set1[k]] = (unsigned char)to;
    }
    char buf[1024]; long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (long j = 0; j < n; j++) buf[j] = (char)tab[(unsigned char)buf[j]];
        write(1, buf, (size_t)n);
    }
    return 0;
}
