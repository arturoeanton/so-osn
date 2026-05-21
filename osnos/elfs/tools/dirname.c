/*
 * tools/dirname.c — return everything before the last '/'.
 *
 *   dirname /a/b/c       → /a/b
 *   dirname c            → .
 *   dirname /            → /
 *   dirname /a           → /
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dirname PATH\n"); return 1; }
    const char *p = argv[1];
    size_t end = strlen(p);

    if (end == 0) { printf(".\n"); return 0; }
    /* Strip trailing slashes (not the root one). */
    while (end > 1 && p[end - 1] == '/') end--;
    /* Find last '/'. */
    size_t cut = 0;
    int    saw_slash = 0;
    for (size_t i = end; i > 0; i--) {
        if (p[i - 1] == '/') { cut = i - 1; saw_slash = 1; break; }
    }
    if (!saw_slash) { printf(".\n"); return 0; }
    if (cut == 0)    { printf("/\n"); return 0; }
    /* Strip trailing slashes in the dir part too. */
    while (cut > 1 && p[cut - 1] == '/') cut--;
    for (size_t i = 0; i < cut; i++) putchar(p[i]);
    putchar('\n');
    return 0;
}
