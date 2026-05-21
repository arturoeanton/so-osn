#include <stdio.h>
#include <string.h>

/*
 * `echo` with `-e` (interpret escapes) and `-n` (no trailing
 * newline). GNU coreutils convention. `-e` is what most users
 * actually want when piping `\n`-separated lines.
 *
 *   echo -e "uno\ndos"          → "uno\ndos\n" (newline-separated)
 *   echo -n hola                → "hola" (no trailing newline)
 *   echo "hola"                  → "hola\n" (default: literal)
 */

static void emit_escaped(const char *s) {
    for (; *s; s++) {
        if (*s != '\\' || s[1] == 0) { putchar((unsigned char)*s); continue; }
        s++;
        switch (*s) {
        case 'n':  putchar('\n'); break;
        case 't':  putchar('\t'); break;
        case 'r':  putchar('\r'); break;
        case 'b':  putchar('\b'); break;
        case '0':  putchar('\0'); break;
        case '\\': putchar('\\'); break;
        case '"':  putchar('"');  break;
        case 'a':  putchar(0x07); break;
        default:   putchar('\\'); putchar((unsigned char)*s); break;
        }
    }
}

int main(int argc, char **argv) {
    int interpret = 0;
    int newline   = 1;
    int argi = 1;

    /* Parse leading flags. Stops at the first non-flag word. */
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != 0) {
        const char *f = argv[argi] + 1;
        if (strcmp(f, "e") == 0)      { interpret = 1; argi++; }
        else if (strcmp(f, "E") == 0) { interpret = 0; argi++; }
        else if (strcmp(f, "n") == 0) { newline   = 0; argi++; }
        else break;
    }

    for (int i = argi; i < argc; i++) {
        if (i > argi) putchar(' ');
        if (interpret) emit_escaped(argv[i]);
        else           printf("%s", argv[i]);
    }
    if (newline) putchar('\n');
    return 0;
}
