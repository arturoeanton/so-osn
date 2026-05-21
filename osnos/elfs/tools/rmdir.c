#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rmdir DIR\n");
        return 1;
    }
    if (rmdir(argv[1]) != 0) {
        fprintf(stderr, "rmdir: cannot remove %s\n", argv[1]);
        return 1;
    }
    return 0;
}
