#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: touch FILE\n");
        return 1;
    }
    int fd = open(argv[1], O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "touch: cannot create %s\n", argv[1]);
        return 1;
    }
    close(fd);
    return 0;
}
