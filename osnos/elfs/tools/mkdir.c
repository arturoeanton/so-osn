#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir DIR\n");
        return 1;
    }
    if (mkdir(argv[1], 0755) != 0) {
        fprintf(stderr, "mkdir: cannot create %s\n", argv[1]);
        return 1;
    }
    return 0;
}
