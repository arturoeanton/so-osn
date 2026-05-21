/*
 * tools/pwd.c — print working directory. Counterpart of the shellsrv
 * `pwd` builtin, available as a real /bin/pwd ELF for scripts.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == 0) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}
