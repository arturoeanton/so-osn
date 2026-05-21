/*
 * tools/yes.c — repeat a string forever (default "y").
 *
 * Usage: yes [STRING...]
 *
 * Terminates on stdout write error (EPIPE when fed into `head`),
 * which is the standard way scripts use it.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char line[256];
    if (argc < 2) {
        line[0] = 'y'; line[1] = '\n'; line[2] = 0;
    } else {
        size_t pos = 0;
        for (int i = 1; i < argc; i++) {
            if (i > 1 && pos + 1 < sizeof(line) - 1) line[pos++] = ' ';
            const char *p = argv[i];
            while (*p && pos + 1 < sizeof(line) - 1) line[pos++] = *p++;
        }
        if (pos + 1 >= sizeof(line) - 1) pos = sizeof(line) - 2;
        line[pos++] = '\n';
        line[pos]   = 0;
    }
    size_t len = strlen(line);
    for (;;) {
        if (write(1, line, len) < 0) return 0;
    }
}
