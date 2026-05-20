#pragma once

/*
 * Deferred cleanup for resources tied to tasks that just died.
 *
 * When a ring-3 task exits (sys_exit) or faults (#PF / #GP / #UD from
 * CPL=3), proc_exit_current_user runs while standing on that task's
 * per-task kernel stack — so it cannot kfree the stack itself. Instead
 * the pointer is handed to the reaper. The next scheduler_tick calls
 * reaper_drain on the (now-different) scheduler stack, kfree's every
 * pending kstack, and resets dead task slots back to UNUSED so they
 * disappear from /sys/tasks.
 *
 * Safe by construction:
 *   - reaper_add_kstack only stores the pointer; the memory itself is
 *     still in use until sched_resume_jump swaps RSP.
 *   - reaper_drain runs from scheduler_tick which is called on the
 *     scheduler's stack — the per-task kstack is no longer live.
 */

#define REAPER_MAX_PENDING 16

void reaper_init(void);

/* Queue a per-task kernel stack pointer for kfree. Silently no-ops on
 * NULL. If the queue is full, the kstack is leaked and a counter is
 * bumped (visible via reaper_leaks). */
void reaper_add_kstack(void *kstack);

/* Drain all pending kstacks (kfree) and reap DEAD task slots
 * (state → UNUSED). Called at the top of each scheduler_tick. */
void reaper_drain(void);

/* Counters for introspection (mem command / /sys/meminfo). */
unsigned long reaper_total_reaped(void);
unsigned long reaper_leaks(void);
