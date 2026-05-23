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
           "  ___  ____         ___  ____  \n"
           " / _ \\/ ___| _ __  / _ \\/ ___| \n"
           "| | | \\___ \\| '_ \\| | | \\___ \\ \n"
           "| |_| |___) | | | | |_| |___) |\n"
           " \\___/|____/|_| |_|\\___/|____/ \n"
           "\x1b[39m");
    printf("\n");
    printf("\x1b[38;2;255;200;100m"
           "  osnos — x86_64 microkernel hobby OS\n"
           "  Build: FASE 11.3 (tcc + lua + jq self-host)\n"
           "  Type 'help' for a list of shell builtins.\n"
           "\x1b[39m");
    printf("\n");
    return 0;
}
