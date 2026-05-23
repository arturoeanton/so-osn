#include <libgen.h>
#include <string.h>

/*
 * POSIX dirname/basename — mutate the caller's buffer. Algorithm
 * follows the POSIX spec verbatim:
 *   - "/usr/bin/cat"  → dirname "/usr/bin" / basename "cat"
 *   - "/usr/bin/"     → dirname "/usr"     / basename "bin"
 *   - "cat"           → dirname "."        / basename "cat"
 *   - "/"             → dirname "/"        / basename "/"
 *   - ""              → dirname "."        / basename "."
 *   - NULL            → dirname "."        / basename "."
 */

char *dirname(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    if (!path || !*path) return dot;

    /* Strip trailing slashes (but keep one if the path is "//..//"). */
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;

    /* Find the last '/'. */
    char *last = 0;
    for (size_t i = 0; i < n; i++) if (path[i] == '/') last = path + i;
    if (!last) return dot;            /* no slash → "."           */
    if (last == path) return slash;    /* only slash is at root    */

    /* Strip ALL trailing slashes from the dir component. */
    while (last > path && *last == '/') *last-- = 0;
    if (*path == 0) return slash;
    return path;
}

char *basename(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    if (!path || !*path) return dot;

    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    if (n == 1 && path[0] == '/') return slash;

    char *last = 0;
    for (size_t i = 0; i < n; i++) if (path[i] == '/') last = path + i;
    if (!last) return path;
    return last + 1;
}
