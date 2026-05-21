#pragma once

#include <stdint.h>

/*
 * Read-only snapshot of a task slot — ABI for SYS_TASKINFO (#265).
 *
 * Lets ring-3 inspectors (cmd_top, kerntest, future ps) enumerate
 * the task table without leaking kernel internals like saved iret
 * frames or pml4 pointers. Stable layout: any field added in the
 * future must go at the END.
 */

#define OSNOS_TASKINFO_NAME_MAX 32

/* task_state_t values mirrored for userland (kernel: src/micro/task.h). */
#define OSNOS_TASK_UNUSED   0
#define OSNOS_TASK_READY    1
#define OSNOS_TASK_RUNNING  2
#define OSNOS_TASK_BLOCKED  3
#define OSNOS_TASK_STOPPED  4
#define OSNOS_TASK_DEAD     5

typedef struct {
    uint64_t pid;
    char     name[OSNOS_TASKINFO_NAME_MAX];
    uint8_t  state;          /* OSNOS_TASK_* */
    uint8_t  is_user;        /* 1 if ring-3 (pml4 != 0), 0 if kernel */
    uint8_t  pad[6];
    uint64_t dispatches;     /* monotonic counter, for top */
} osnos_taskinfo_t;
