/*
 * tools/uname.c — print system identification. POSIX subset.
 *
 *   uname        — kernel name (default)
 *   uname -a     — all fields
 *   uname -s     — kernel name
 *   uname -n     — node name (hostname)
 *   uname -r     — release version
 *   uname -m     — machine architecture
 */

#include <stdio.h>

#define OSNAME    "osnos"
#define NODENAME  "localhost"
#define RELEASE   "0.10"
#define VERSION   "FASE 10.4 (ring-3 microkernel)"
#define MACHINE   "x86_64"

int main(int argc, char **argv) {
    int s = 0, n = 0, r = 0, v = 0, m = 0, a = 0;
    if (argc == 1) { s = 1; }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
                case 'a': a = 1; break;
                case 's': s = 1; break;
                case 'n': n = 1; break;
                case 'r': r = 1; break;
                case 'v': v = 1; break;
                case 'm': m = 1; break;
                default:
                    fprintf(stderr, "uname: unknown flag -%c\n", *p);
                    return 1;
            }
        }
    }
    if (a) { s = n = r = v = m = 1; }

    int first = 1;
    #define FIELD(flag, val) do { \
        if (flag) { \
            if (!first) printf(" "); \
            printf("%s", val); \
            first = 0; \
        } \
    } while (0)

    FIELD(s, OSNAME);
    FIELD(n, NODENAME);
    FIELD(r, RELEASE);
    FIELD(v, VERSION);
    FIELD(m, MACHINE);
    printf("\n");
    return 0;
}
