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
            /* Copy the name in — the caller's pointer may point to
             * user-mode memory (proc_execve VFS path) that becomes
             * unreachable from sys_taskinfo's caller pml4. */
            size_t k = 0;
            if (name) {
                while (name[k] && k + 1 < OSNOS_TASK_NAME_MAX) {
                    tasks[i].name[k] = name[k];
                    k++;
                }
            }
            tasks[i].name[k] = 0;
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
            /* Snapshot MSR_FS_BASE — TLS pointer del task saliente.
             * musl errno, pthread_self, stack-protector cookie todos
             * viven en %fs:offset, así que sin save/restore un task
             * hereda el FS_BASE del que corrió antes. */
            if (prev->pml4) {
                uint32_t lo, hi;
                __asm__ volatile ("rdmsr"
                    : "=a"(lo), "=d"(hi)
                    : "c"(0xC0000100));   /* MSR_FS_BASE */
                prev->fs_base = ((uint64_t)hi << 32) | lo;
            }
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
            /* Restaurar MSR_FS_BASE del task entrante (ver comment
             * arriba en el save path). 0 es válido — tasks recién
             * spawneados aún no han ejecutado arch_prctl. */
            if (task->pml4) {
                uint64_t fs = task->fs_base;
                uint32_t lo = (uint32_t)fs;
                uint32_t hi = (uint32_t)(fs >> 32);
                __asm__ volatile ("wrmsr"
                    :
                    : "a"(lo), "d"(hi), "c"(0xC0000100));
            }
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

/*
 * Grace counter per slot. When a task hits TASK_DEAD, we wait at
 * least REAP_GRACE_PASSES calls to task_reap_dead before actually
 * clearing the slot. That keeps `exit_code` + `pid` visible long
 * enough for shellsrv's wait_pid (polled every ~20 ms via
 * sys_taskinfo) to capture the value before the reaper recycles
 * the slot. Without this, fast-finishing tasks like `/bin/false`
 * disappear between the spawn and the first poll, leaving
 * shellsrv with exit_code = 0 — breaking `&&` / `||` chains.
 *
 * REAP_GRACE_PASSES = 4 gives ~40-80 ms of zombie lifetime since
 * task_reap_dead is called from reaper_drain at every
 * scheduler_tick (each task switch).
 */
#define REAP_GRACE_PASSES 4
static int reap_age[MAX_TASKS];

void task_reap_dead(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD) {
            reap_age[i] = 0;
            continue;
        }
        if (reap_age[i] < REAP_GRACE_PASSES) {
            reap_age[i]++;
            continue;
        }
        task_clear(&tasks[i]);
        reap_age[i] = 0;
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
