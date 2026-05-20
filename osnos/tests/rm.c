#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rm FILE [FILE...]\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) != 0) {
            fprintf(stderr, "rm: cannot remove %s\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
