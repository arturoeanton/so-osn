#include "syscall.h"

#include "../proc/exec.h"
#include "scheduler.h"
#include "task.h"

/*
 * int 0x80 syscall entry — the bridge that lets ring-3 code invoke
 * sys_* via the same dispatcher the kernel-side builtins use.
 *
 * Register contract (matches Linux SYSCALL ABI, so user code is portable
 * to the future SYSCALL/SYSRET path):
 *   RAX            = syscall number; on return holds the result
 *   RDI/RSI/RDX/R10/R8/R9 = arguments 0..5
 *
 * Caller-saved regs not in this list (RCX, R11) may be clobbered, like
 * SYSCALL does. Callee-saved (RBX, RBP, R12..R15) are preserved (we
 * never touch them).
 *
 * The asm stub pushes user GPRs in syscall_frame_t order, calls a
 * tiny C wrapper that runs the dispatcher + flushes console output,
 * then restores GPRs (overriding RAX with the return value) and
 * iretq's back to ring 3.
 */

uint64_t int80_dispatch_wrapper(syscall_frame_t *frame) {
    uint64_t result = syscall_dispatch(frame);

    /* Note: pre-FASE-10.1 we drained the in-kernel console_server
     * here so console writes were visible before the iretq. That's
     * no longer needed — ipc_send wakes the ring-3 consrv via
     * task_unblock, and the scheduler picks it up on its next
     * dispatch. */

    task_t *t = task_current();

    /*
     * Signal delivery on syscall return.
     *
     * Replaces the legacy `kill_pending → proc_exit(130)` fast-path
     * which used to live here. That hardcoded SIGINT for every kill,
     * so SIGTERM-killed children showed as exit_code 130 instead of
     * 143 (WTERMSIG returned SIGINT not SIGTERM). The sig_pending
     * path below carries the actual signal number through to
     * user_task_resume, which calls proc_exit_current_user(128 + sig)
     * for SIG_DFL — POSIX-correct.
     *
     * user_task_resume handles signals when a task wakes up from a
     * suspended state (saved_valid path), but a plain "syscall →
     * return to user" goes back via iretq from this wrapper without
     * touching user_task_resume. Without an injection here, signals
     * fired by sys_kill/raise/tty during a syscall would never be
     * delivered — handler installed via sigaction would never run.
     *
     * Trick: snapshot the current iret + GPRs into saved_*, set
     * saved_rax = the syscall's return value, then sched_resume_jump.
     * The scheduler re-dispatches us; user_task_trampoline sees
     * saved_valid=1, hands off to user_task_resume which spots
     * sig_pending and constructs the sigframe + handler redirect.
     * The handler runs; on return, __sigtramp does rt_sigreturn
     * which restores saved_iret_* with the syscall RIP just past
     * the syscall and rax = result. Net effect: signal handler runs
     * BETWEEN the syscall and the next user instruction, with the
     * syscall's return value preserved.
     */
    if (t && t->pml4 && t->sig_pending != 0) {
        uint64_t *iret = (uint64_t *)(t->kernel_stack_top - 40);
        t->saved_iret_rip    = iret[0];
        t->saved_iret_cs     = iret[1];
        t->saved_iret_rflags = iret[2];
        t->saved_iret_rsp    = iret[3];
        t->saved_iret_ss     = iret[4];
        t->saved_rax = result;
        t->saved_rbx = frame->rbx;
        t->saved_rcx = frame->rcx;
        t->saved_rdx = frame->rdx;
        t->saved_rsi = frame->rsi;
        t->saved_rdi = frame->rdi;
        t->saved_rbp = frame->rbp;
        t->saved_r8  = frame->r8;
        t->saved_r9  = frame->r9;
        t->saved_r10 = frame->r10;
        t->saved_r11 = frame->r11;
        t->saved_r12 = frame->r12;
        t->saved_r13 = frame->r13;
        t->saved_r14 = frame->r14;
        t->saved_r15 = frame->r15;
        t->saved_valid = 1;
        sched_resume_jump();           /* never returns */
    }

    return result;
}

/*
 * Push 15 user GPRs in reverse syscall_frame_t order so the resulting
 * frame matches the struct layout: rax at the lowest offset (top of
 * stack), r15 at the highest. Then use %rbp as a "frame pointer"
 * trick to remember the unaligned %rsp across the SysV-aligned call.
 *
 * Saving every GPR (not just the args) is mandatory for
 * sys_nanosleep: it needs to checkpoint the full pre-syscall user
 * state so a wakeup can restore it.
 */
__asm__ (
    ".global int80_entry\n"
    "int80_entry:\n"

    "    pushq %r15\n"
    "    pushq %r14\n"
    "    pushq %r13\n"
    "    pushq %r12\n"
    "    pushq %r11\n"
    "    pushq %r10\n"
    "    pushq %r9\n"
    "    pushq %r8\n"
    "    pushq %rbp\n"
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %rdx\n"
    "    pushq %rcx\n"
    "    pushq %rbx\n"
    "    pushq %rax\n"

    "    movq %rsp, %rdi\n"            /* arg1 = pointer to frame */

    /* Pivot to a 16-byte-aligned stack for the C call; remember the
     * original (mid-frame) rsp in a callee-saved reg. We can't use
     * %rbp because that's part of the saved frame; %r12 is free
     * because we already saved it. */
    "    movq %rsp, %r12\n"
    "    andq $-16, %rsp\n"

    "    call int80_dispatch_wrapper\n"

    "    movq %r12, %rsp\n"

    "    movq %rax, (%rsp)\n"          /* overwrite saved rax with retval */

    "    popq %rax\n"
    "    popq %rbx\n"
    "    popq %rcx\n"
    "    popq %rdx\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbp\n"
    "    popq %r8\n"
    "    popq %r9\n"
    "    popq %r10\n"
    "    popq %r11\n"
    "    popq %r12\n"
    "    popq %r13\n"
    "    popq %r14\n"
    "    popq %r15\n"

    "    iretq\n"
);
