#include "task.h"

#include <stddef.h>

#include "fpu.h"

static task_t tasks[MAX_TASKS];

static uint64_t next_pid = 1;
static int current_index = -1;

/*
 * Reset a slot to a fully cleared task_t. Used by task_init for the
 * boot zeroing, by task_reap_dead when collecting a DEAD task, and
 * implicitly by task_create which always starts from a cleared slot
 * (UNUSED or freshly-reaped DEAD).
 */
static void task_clear(task_t *t) {
    /* Byte-wise zero — keeps things simple as task_t grows. */
    char *p = (char *)t;
    for (size_t i = 0; i < sizeof(*t); i++) p[i] = 0;
}

void task_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) task_clear(&tasks[i]);
    next_pid = 1;
    current_index = -1;
}

int task_create(
    const char *name,
    task_entry_t entry
) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_DEAD) {

            task_clear(&tasks[i]);
            fd_init_for_task(&tasks[i]);
            tasks[i].pid   = next_pid++;
            tasks[i].name  = name;
            tasks[i].entry = entry;
            tasks[i].state = TASK_READY;

            return (int)tasks[i].pid;
        }
    }

    return -1;
}

void task_set_state(
    uint64_t pid,
    task_state_t state
) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid) {
            tasks[i].state = state;
            return;
        }
    }
}

task_t *task_current(void) {
    if (current_index < 0 || current_index >= MAX_TASKS) {
        return 0;
    }

    return &tasks[current_index];
}

void task_run_next(void) {
    /*
     * Snapshot the outgoing task's FP/SSE state BEFORE we start
     * looking for the next victim. The HW registers still hold
     * whatever the user was doing when the timer IRQ or syscall
     * dragged us into the kernel — once we dispatch a different
     * task, FXRSTOR will overwrite them. We need that snapshot
     * sitting in the outgoing task's struct so its next dispatch
     * picks up where it left off.
     *
     * Kernel tasks (servers, shell) compile with -mno-sse so they
     * never touch FP. The save still runs against their slot, but
     * the bytes it captures are whatever the previous user task
     * left in HW — harmless because no kernel task reads them.
     */
    int prev_index = current_index;
    if (prev_index >= 0 && prev_index < MAX_TASKS) {
        task_t *prev = &tasks[prev_index];
        if (prev->state != TASK_UNUSED) {
            fpu_save(prev->fpu_state);
        }
    }

    for (int step = 0; step < MAX_TASKS; step++) {
        current_index++;
        current_index %= MAX_TASKS;

        task_t *task = &tasks[current_index];

        if (task->state != TASK_READY) {
            continue;
        }

        task->state = TASK_RUNNING;

        /* Reload the incoming task's FP/SSE before its code runs.
         * Skip if it's the same slot we just saved — pure waste of
         * cycles and the regs are already correct. */
        if (current_index != prev_index) {
            fpu_restore(task->fpu_state);
        }

        if (task->entry) {
            task->dispatches++;
            task->entry();
        }

        if (task->state == TASK_RUNNING) {
            task->state = TASK_READY;
        }

        return;
    }
}

const task_t *task_slot(size_t idx) {
    if (idx >= MAX_TASKS) return 0;
    if (tasks[idx].state == TASK_UNUSED) return 0;
    return &tasks[idx];
}

task_t *task_by_pid(uint64_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return 0;
}

void task_reap_dead(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD) continue;
        task_clear(&tasks[i]);
    }
}

static uint64_t wakeups_fired;

uint64_t task_wakeups_fired(void) { return wakeups_fired; }

void task_check_wakeups(uint64_t now_ms) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state != TASK_BLOCKED) continue;
        if (t->wakeup_at_ms == 0)     continue;
        if (now_ms < t->wakeup_at_ms) continue;
        t->wakeup_at_ms = 0;
        t->state        = TASK_READY;
        wakeups_fired++;
    }
}

void task_unblock(uint64_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {

        if (tasks[i].pid == pid &&
            tasks[i].state == TASK_BLOCKED) {

            tasks[i].state = TASK_READY;
        }
    }
}
