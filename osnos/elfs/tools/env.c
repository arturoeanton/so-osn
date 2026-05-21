/*
 * tools/env.c — print the environment, one KEY=VAL per line.
 *
 * No-arg behaviour mirrors POSIX env(1). Setting / unsetting via
 * `env -i / VAR=VAL cmd` is not implemented yet (needs proper
 * exec-with-env from the shell side).
 */

#include <stdio.h>
#include <stdlib.h>

extern char **environ;

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!environ) return 0;
    for (int i = 0; environ[i]; i++) {
        printf("%s\n", environ[i]);
    }
    return 0;
}
