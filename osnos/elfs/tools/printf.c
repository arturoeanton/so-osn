/*
 * tools/printf.c — minimal POSIX printf(1).
 *
 * Supports format directives %s, %d, %x, %c, %% and escapes
 * \n \t \\ \" \r \0 (truncates). The format string is reused if
 * there are more args than directives, like POSIX. No width/
 * precision modifiers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit_escapes(const char **p) {
    char c = *(*p)++;
    switch (c) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case '\\': putchar('\\'); break;
        case '"': putchar('"'); break;
        case '0': /* nothing — POSIX says \0 starts an octal, we just
                   * drop it. */ break;
        default: putchar('\\'); putchar(c); break;
    }
}

static int emit_format(const char *fmt, int argc, char **argv, int *ai) {
    int consumed = 0;
    const char *p = fmt;
    while (*p) {
        if (*p == '\\') { p++; if (*p) emit_escapes(&p); continue; }
        if (*p == '%') {
            p++;
            char d = *p ? *p++ : 0;
            if (d == 0) { putchar('%'); break; }
            if (d == '%') { putchar('%'); continue; }
            const char *a = (*ai < argc) ? argv[(*ai)++] : "";
            consumed = 1;
            switch (d) {
                case 's': fputs(a, stdout); break;
                case 'd': printf("%d", atoi(a)); break;
                case 'x': printf("%x", atoi(a)); break;
                case 'c':
                    if (a[0]) putchar(a[0]);
                    break;
                default: putchar('%'); putchar(d); break;
            }
            continue;
        }
        putchar(*p++);
    }
    return consumed;
}

int main(int argc, char **argv) {
    if (argc < 2) return 0;
    int ai = 2;
    /* POSIX: re-apply format while there are unused args. */
    int safety = 32;
    do {
        int consumed = emit_format(argv[1], argc, argv, &ai);
        if (!consumed) break;
        if (--safety <= 0) break;
    } while (ai < argc);
    return 0;
}
