/*
 * tests/hello_libc.c — first osnos user ELF that uses the libc.
 * Exercises printf (formats + varargs), malloc/free, and string.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("hola %s, %d!\n", "mundo", 7);

    char *buf = malloc(64);
    if (!buf) {
        fprintf(stderr, "malloc fallo\n");
        return 1;
    }
    strcpy(buf, "via malloc + strcpy");
    puts(buf);
    free(buf);

    printf("pointer demo: stack=%p heap-sized=%zu\n",
           (void *)&argc, sizeof(size_t));

    printf("hex/octal/dec: 0x%x 0%o %d %u\n", 255, 64, -42, 4000000000u);

    return 0;
}
