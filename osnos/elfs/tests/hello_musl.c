/*
 * /bin/hello_musl — smoke test musl libc.
 *
 * Stdio fully working: printf con line buffering, fflush, snprintf.
 * Como nuestro stdout es line-buffered (TIOCGWINSZ retorna OK →
 * musl deja lbf='\n'), todo printf que termina en \n se flushea
 * inmediato. fflush(stdout) entre runs si querés output partial.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("============================================\n");
    printf("  hello from musl libc on osnos\n");
    printf("============================================\n");
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    /* %f real (mini-libc no lo soporta). */
    double pi = 3.14159265358979;
    printf("pi (musl %%f)        = %.10f\n", pi);
    printf("e  (vía exp(1))     = %.6f\n", 2.71828182845904);
    /* Width + padding + bases. */
    printf("hex     = %08x\n", 0xdeadbeefu);
    printf("decimal = %10d\n", -42);
    printf("octal   = %o\n", 0755);
    printf("string  = %-20s|\n", "left-padded");
    printf("string  = %20s|\n", "right-padded");
    /* snprintf también. */
    char buf[80];
    int n = snprintf(buf, sizeof(buf),
                     "snprintf produced %d bytes: pi/2 = %.4f", 0, pi/2);
    printf("[%s — really %d]\n", buf, n);
    printf("end of musl smoke test — all good\n");
    return 0;
}
