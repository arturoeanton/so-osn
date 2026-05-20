#include "scheduler.h"
#include "task.h"

#include "pmm.h"
#include "reaper.h"
#include "timer.h"
#include "vmm.h"

static uint64_t ticks = 0;

/*
 * Resume context for the scheduler_loop. Saved once at the top of
 * scheduler_loop; restored by sched_resume_jump() to longjmp back from
 * any depth (user-task sys_exit, fatal fault, etc.).
 *
 * We save callee-saved registers because the long jump lands inside
 * scheduler_loop and the compiler assumes those held their saved
 * values across the asm block.
 */
static uint64_t resume_rsp;
static uint64_t resume_rbp;
static uint64_t resume_rbx;
static uint64_t resume_r12;
static uint64_t resume_r13;
static uint64_t resume_r14;
static uint64_t resume_r15;
static uint64_t resume_rip;

void scheduler_init(void) {
    ticks = 0;
}

void scheduler_tick(void) {
    /*
     * Drain DEAD-task resources before running the next task. This is
     * the one spot we can be sure we are not standing on any per-task
     * kstack: scheduler_loop's frame is the host for the longjmp, so
     * after sched_resume_jump RSP is back here.
     */
    reaper_drain();

    /* Wake any sleeping task whose deadline has passed. */
    task_check_wakeups(timer_ms());

    ticks++;
    task_run_next();
}

uint64_t scheduler_get_ticks(void) {
    return ticks;
}

__attribute__((noreturn))
void scheduler_loop(void) {
    /*
     * Save the resume point. Label 1 marks where the long jump lands.
     * This function never returns — its frame must remain alive on
     * the stack for the saved RSP to keep pointing into a valid
     * region.
     */
    __asm__ volatile (
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, %0\n\t"
        "movq %%rsp, %1\n\t"
        "movq %%rbp, %2\n\t"
        "movq %%rbx, %3\n\t"
        "movq %%r12, %4\n\t"
        "movq %%r13, %5\n\t"
        "movq %%r14, %6\n\t"
        "movq %%r15, %7\n\t"
        "1:\n\t"
        : "=m"(resume_rip),
          "=m"(resume_rsp),
          "=m"(resume_rbp),
          "=m"(resume_rbx),
          "=m"(resume_r12),
          "=m"(resume_r13),
          "=m"(resume_r14),
          "=m"(resume_r15)
        :
        : "rax", "memory"
    );

    for (;;) {
        scheduler_tick();
    }
}

__attribute__((noreturn))
void sched_resume_jump(void) {
    /*
     * Switch CR3 back to the kernel address space before doing
     * anything else. The caller might be running inside a user task's
     * pml4 (e.g. sys_exit from int 0x80). The kernel half is shared,
     * so this function's own code keeps executing.
     */
    uint64_t kpml4_phys =
        (uint64_t)vmm_kernel_pml4() - pmm_hhdm_offset();
    __asm__ volatile ("mov %0, %%cr3" :: "r"(kpml4_phys) : "memory");

    /*
     * The task that was dispatched when the long jump happens was
     * never given the chance to transition RUNNING -> READY (that
     * normally happens at the end of task_run_next). Do it manually
     * so the scheduler picks it up again next round.
     */
    task_t *t = task_current();
    if (t && t->state == TASK_RUNNING) t->state = TASK_READY;

    /*
     * Caller arrived from a syscall handler (FMASK cleared IF) or a
     * fault handler (interrupt gate cleared IF). The longjmp below
     * restores RSP/RBP/RBX/R12-R15/RIP but NOT RFLAGS, so without
     * this `sti` the scheduler_loop would resume with IF=0 and
     * hardware IRQs (incl. the PIT) would silently stop arriving.
     * That symptom is what the FASE 9.2 sleep diagnostics caught
     * (timer_ms barely advanced after sleep blocked).
     */
    __asm__ volatile ("sti" ::: "memory");

    /* Restore callee-saved + RSP + RIP. */
    __asm__ volatile (
        "movq %0, %%rsp\n\t"
        "movq %1, %%rbp\n\t"
        "movq %2, %%rbx\n\t"
        "movq %3, %%r12\n\t"
        "movq %4, %%r13\n\t"
        "movq %5, %%r14\n\t"
        "movq %6, %%r15\n\t"
        "jmpq *%7\n\t"
        :
        : "m"(resume_rsp),
          "m"(resume_rbp),
          "m"(resume_rbx),
          "m"(resume_r12),
          "m"(resume_r13),
          "m"(resume_r14),
          "m"(resume_r15),
          "m"(resume_rip)
        : "memory"
    );
    __builtin_unreachable();
}
