/*
 * tools/tee.c — copy stdin to stdout AND to each named file.
 *
 *   tee file              — overwrite
 *   tee -a file           — append
 *   tee f1 f2 f3          — multiple files
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEE_MAX_FILES 8

int main(int argc, char **argv) {
    int append = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-a") == 0) { append = 1; i++; }
        else { fprintf(stderr, "tee: unknown flag %s\n", argv[i]); return 1; }
    }

    int fds[TEE_MAX_FILES];
    int nfds = 0;
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    for (; i < argc && nfds < TEE_MAX_FILES; i++) {
        int fd = open(argv[i], flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "tee: %s: cannot open\n", argv[i]);
            continue;
        }
        fds[nfds++] = fd;
    }

    char buf[1024];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
        for (int k = 0; k < nfds; k++) write(fds[k], buf, (size_t)n);
    }
    for (int k = 0; k < nfds; k++) close(fds[k]);
    return 0;
}
