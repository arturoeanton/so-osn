/*
 * tools/seq.c — print arithmetic sequences (POSIX-ish).
 *
 *   seq LAST              — 1..LAST
 *   seq FIRST LAST        — FIRST..LAST step 1
 *   seq FIRST STEP LAST   — FIRST..LAST step STEP
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    long first = 1, step = 1, last = 1;
    if (argc == 2) {
        last = atol(argv[1]);
    } else if (argc == 3) {
        first = atol(argv[1]);
        last  = atol(argv[2]);
    } else if (argc == 4) {
        first = atol(argv[1]);
        step  = atol(argv[2]);
        last  = atol(argv[3]);
    } else {
        fprintf(stderr, "usage: seq [FIRST [STEP]] LAST\n");
        return 1;
    }
    if (step == 0) { fprintf(stderr, "seq: step cannot be 0\n"); return 1; }

    if (step > 0) {
        for (long i = first; i <= last; i += step) printf("%ld\n", i);
    } else {
        for (long i = first; i >= last; i += step) printf("%ld\n", i);
    }
    return 0;
}
