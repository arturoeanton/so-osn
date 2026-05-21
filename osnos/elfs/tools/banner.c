/*
 * tools/banner.c — print the osnos ASCII-art logo + uname-style info.
 *
 * Invoked from /home/.oshrc to greet the user at boot. Plain
 * write() to stdout; the consrv ring-3 server (FASE 10.1) renders
 * it on the framebuffer.
 */

#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\n");
    printf("\x1b[38;2;0;255;102m"
           "   ___  ____             ____   ____  \n"
           "  / _ \\/ ___| _ __   ___/ ___| / ___| \n"
           " | | | \\___ \\| '_ \\ / _ \\___ \\ \\___ \\ \n"
           " | |_| |___) | | | | (_) |__) | ___) |\n"
           "  \\___/|____/|_| |_|\\___/____/ |____/ \n"
           "\x1b[39m");
    printf("\n");
    printf("\x1b[38;2;255;200;100m"
           "  osnos — x86_64 microkernel hobby OS\n"
           "  Build: FASE 10.4 (servers in ring 3)\n"
           "  Type 'help' for a list of shell builtins.\n"
           "\x1b[39m");
    printf("\n");
    return 0;
}
