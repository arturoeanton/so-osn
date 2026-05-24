#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Mini-libc rm:
 *   -f  ignora missing files / nunca prompt (POSIX -f). Crítico para
 *       Makefile recipes tipo `rm -f hello` que corren idempotente.
 *   -r/-R deliberadamente NO soportado todavía (no rmdir-recursive).
 */
int main(int argc, char **argv) {
    int force = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') break;
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (int k = 1; argv[i][k]; k++) {
            switch (argv[i][k]) {
            case 'f': force = 1; break;
            default:
                fprintf(stderr, "rm: unknown flag -%c\n", argv[i][k]);
                return 1;
            }
        }
    }
    if (i >= argc) {
        if (force) return 0;
        fprintf(stderr, "usage: rm [-f] FILE [FILE...]\n");
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        if (unlink(argv[i]) != 0) {
            if (force && errno == ENOENT) continue;
            fprintf(stderr, "rm: cannot remove %s\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
