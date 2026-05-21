#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int cat_one(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "cat: cannot open %s\n", path);
        return 1;
    }

    char buf[256];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        write(1, buf, (unsigned long)n);
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        /* POSIX: no args → copy stdin to stdout. Useful with shell
         * input redirection (`cat < file > other`). */
        char buf[256];
        for (;;) {
            long n = read(0, buf, sizeof(buf));
            if (n <= 0) break;
            write(1, buf, (unsigned long)n);
        }
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) if (cat_one(argv[i])) rc = 1;
    return rc;
}
