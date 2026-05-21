#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cp SRC DST\n");
        return 1;
    }

    int sfd = open(argv[1], O_RDONLY, 0);
    if (sfd < 0) {
        fprintf(stderr, "cp: cannot open %s\n", argv[1]);
        return 1;
    }

    int dfd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        fprintf(stderr, "cp: cannot create %s\n", argv[2]);
        close(sfd);
        return 1;
    }

    char buf[256];
    for (;;) {
        long n = read(sfd, buf, sizeof(buf));
        if (n <= 0) break;
        write(dfd, buf, (unsigned long)n);
    }

    close(sfd);
    close(dfd);
    return 0;
}
