#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sleep SECONDS\n");
        return 1;
    }
    int s = atoi(argv[1]);
    if (s < 0) {
        fprintf(stderr, "sleep: invalid seconds\n");
        return 1;
    }
    printf("sleeping %ds...\n", s);
    sleep((unsigned int)s);
    printf("done\n");
    return 0;
}
