#pragma once

#include <stdint.h>

void scheduler_init(void);

void scheduler_tick(void);

/* Number of scheduler_tick invocations since boot. Proxy for uptime
 * until a real timer lands (FASE 9). */
uint64_t scheduler_get_ticks(void);

/*
 * Main scheduler loop. Never returns. Saves a resume point at the top
 * of its body, then loops calling scheduler_tick(). When
 * sched_resume_jump() is called from anywhere (e.g. sys_exit of a user
 * task), control long-jumps back to the resume point and the loop
 * continues. Equivalent to setjmp + for(;;) — the "host" function for
 * the long jump.
 *
 * kmain transfers control here once boot setup is done.
 */
__attribute__((noreturn))
void scheduler_loop(void);

/*
 * Long-jump back to scheduler_loop's saved resume point. Used by
 * sys_exit when a user task has nowhere meaningful to return to.
 * Restores kernel CR3, callee-saved registers, RSP and RIP from the
 * scheduler_loop's saved frame. Any task in TASK_RUNNING state when
 * this is called is reset to TASK_READY so it can be re-dispatched.
 */
__attribute__((noreturn))
void sched_resume_jump(void);
