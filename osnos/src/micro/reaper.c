#include "reaper.h"

#include <stddef.h>

#include "kmalloc.h"
#include "task.h"

static void         *pending[REAPER_MAX_PENDING];
static size_t        pending_count;
static unsigned long total_reaped;
static unsigned long leaked;

void reaper_init(void) {
    pending_count = 0;
    total_reaped  = 0;
    leaked        = 0;
}

void reaper_add_kstack(void *kstack) {
    if (!kstack) return;
    if (pending_count >= REAPER_MAX_PENDING) {
        leaked++;
        return;
    }
    pending[pending_count++] = kstack;
}

void reaper_drain(void) {
    for (size_t i = 0; i < pending_count; i++) {
        kfree(pending[i]);
        pending[i] = 0;
        total_reaped++;
    }
    pending_count = 0;

    /*
     * Reset DEAD slots to UNUSED so /sys/tasks does not accumulate
     * zombies. task_create already reuses DEAD slots, so this is
     * cosmetic for ps. We do it here (not in proc_exit_current_user)
     * because by now the scheduler is in steady state — no one is
     * mid-iretq on a stack we are about to clobber.
     */
    task_reap_dead();
}

unsigned long reaper_total_reaped(void) { return total_reaped; }
unsigned long reaper_leaks(void)        { return leaked; }
