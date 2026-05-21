/*
 * tools/basename.c — strip directory part of a path, leaving the
 * leaf. Optional suffix trim. POSIX subset.
 *
 *   basename /a/b/c        → c
 *   basename /a/b/c.txt .txt → c
 *   basename /              → /
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: basename PATH [SUFFIX]\n"); return 1; }
    const char *path = argv[1];
    const char *suffix = (argc >= 3) ? argv[2] : 0;

    /* Empty / single "/" → print as-is. */
    if (path[0] == 0)           { printf("\n"); return 0; }
    if (path[0] == '/' && path[1] == 0) { printf("/\n"); return 0; }

    /* Strip trailing slashes (POSIX: ignore them). */
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') end--;

    /* Find last '/' before `end`. */
    size_t start = 0;
    for (size_t i = end; i > 0; i--) {
        if (path[i - 1] == '/') { start = i; break; }
    }

    size_t len = end - start;
    /* Suffix trim, only if it doesn't eat the whole name. */
    if (suffix) {
        size_t sl = strlen(suffix);
        if (sl > 0 && sl < len &&
            memcmp(path + end - sl, suffix, sl) == 0) {
            len -= sl;
        }
    }

    for (size_t i = 0; i < len; i++) putchar(path[start + i]);
    putchar('\n');
    return 0;
}
