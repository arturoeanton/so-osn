#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mv SRC DST\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) != 0) {
        fprintf(stderr, "mv: cannot rename %s -> %s\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
