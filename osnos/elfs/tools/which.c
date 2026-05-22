/*
 * tools/which.c — find an executable in $PATH.
 *
 *   which NAME [NAME...]    — print full path of first PATH match
 *                              per arg, or stay silent + exit 1 if
 *                              not found.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int find_one(const char *name) {
    if (name[0] == '/') {
        struct stat st;
        if (stat(name, &st) == 0) { printf("%s\n", name); return 0; }
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    const char *p = path;
    while (*p) {
        char dir[PATH_MAX];
        size_t dn = 0;
        while (*p && *p != ':' && dn + 1 < sizeof(dir)) dir[dn++] = *p++;
        dir[dn] = 0;
        if (*p == ':') p++;
        if (dn == 0) continue;

        char cand[PATH_MAX];
        size_t cn = 0;
        while (cn + 1 < sizeof(cand) && dir[cn]) { cand[cn] = dir[cn]; cn++; }
        if (cn > 0 && cand[cn - 1] != '/' && cn + 1 < sizeof(cand)) cand[cn++] = '/';
        for (size_t i = 0; cn + 1 < sizeof(cand) && name[i]; i++) cand[cn++] = name[i];
        cand[cn] = 0;

        struct stat st;
        if (stat(cand, &st) == 0) { printf("%s\n", cand); return 0; }
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: which NAME [NAME...]\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (find_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
