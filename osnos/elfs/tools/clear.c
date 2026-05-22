/*
 * tools/clear.c — clear the terminal screen + home the cursor.
 *
 * VT100: ESC[2J clears, ESC[H homes. The osnos framebuffer parser
 * honours both.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    write(1, "\x1b[2J\x1b[H", 7);
    return 0;
}
