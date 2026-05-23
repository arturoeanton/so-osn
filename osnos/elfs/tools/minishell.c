/*
 * minishell — minimal interactive "shell-like" demo program.
 *
 * Used as the child process of /bin/term to showcase the PTY
 * pipeline end-to-end. Reads lines from stdin, echoes them back
 * with formatting, exits on "exit" or EOF.
 *
 * NOT a real shell — just a pty-friendly read/echo loop. Doesn't
 * fork, doesn't run children, doesn't parse anything.
 *
 * Behaviour:
 *   - Prints a one-time banner.
 *   - Prompt "mini$ ".
 *   - Reads a line (up to 255 bytes) via read(0).
 *   - Strips trailing newline.
 *   - If the line is "exit", prints "bye" and exits 0.
 *   - Otherwise echoes "you said: <line>" on its own line.
 *   - Loops until EOF (read returns 0).
 *
 * In an interactive PTY (driven by /bin/term), the slave's termios
 * is canonical-mode by default so reads block until '\n' arrives.
 * Inside a piped /bin/termtest scenario the same logic works:
 * the test feeds full lines ending in '\n'.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    const char banner[] =
        "minishell: type 'exit' to quit.\n";
    write(1, banner, sizeof(banner) - 1);

    char buf[256];
    for (;;) {
        write(1, "mini$ ", 6);
        int n = (int)read(0, buf, sizeof(buf) - 1);
        if (n <= 0) break;            /* EOF (master closed) */
        buf[n] = 0;
        /* Strip trailing newline so we don't echo a double blank
         * line under canonical mode (slave_read returns "<line>\n",
         * we want "<line>" + a single explicit '\n' at end). */
        if (n > 0 && buf[n - 1] == '\n') { buf[--n] = 0; }

        if (strcmp(buf, "exit") == 0) {
            write(1, "bye\n", 4);
            return 0;
        }

        write(1, "you said: ", 10);
        write(1, buf, n);
        write(1, "\n", 1);
    }
    write(1, "minishell: EOF, exiting.\n", 25);
    return 0;
}
