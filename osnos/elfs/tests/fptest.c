/*
 * /bin/fptest — stress the per-task FXSAVE/FXRSTOR.
 *
 * Runs N rounds of FP arithmetic that exercises x87 + SSE
 * registers, sleeping briefly between rounds so the scheduler
 * gets preemption opportunities. If FP state is properly saved
 * and restored across context switches, the results stay
 * deterministic. If it isn't, another task's FP regs leak in
 * and our values drift.
 *
 *   fptest [ROUNDS]      # default 200
 *
 * Tip: run two in parallel — `fptest &` then `fptest` — to
 * actually expose multi-task FP corruption. Both should print
 * the same final value.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int rounds = (argc >= 2) ? atoi(argv[1]) : 200;
    if (rounds <= 0) rounds = 200;

    /* Seed values that survive many sin/cos/sqrt iterations
     * without trivially collapsing to 0 or Inf. */
    double a = 1.1234, b = 2.7182, c = 3.1416;

    for (int i = 0; i < rounds; i++) {
        a = sin(a) + cos(b) * 0.9;
        b = sqrt(fabs(b * 1.0001) + 0.001);
        c = c * 0.999 + 0.1234;

        /* Brief sleep gives the scheduler a chance to switch us
         * out — which is exactly when FXSAVE/FXRSTOR must work. */
        struct timespec ts = { 0, 1 * 1000000 };   /* 1 ms */
        nanosleep(&ts, 0);
    }

    /* Expected values are deterministic given the seeds + ops.
     * Run twice in isolation, confirm bit-identical output. */
    printf("fptest [%d rounds]: a=%.6f  b=%.6f  c=%.6f\n",
           rounds, a, b, c);
    return 0;
}
