#include "syscall.h"

#include "../proc/exec.h"
#include "../servers/console_server.h"
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

    /*
     * Drain pending console writes synchronously so a user task's
     * sys_write to stdout becomes visible BEFORE we iretq back to
     * ring 3 (which won't yield until it does another syscall or
     * faults). Temporary until we have proper preemption / yield.
     */
    console_server_tick();

    /*
     * Honour a pending Ctrl+C kill before returning to ring 3. The
     * shell sets kill_pending when the user typed ^C; we now route
     * to proc_exit_current_user (never returns) instead of iretq'ing.
     */
    task_t *t = task_current();
    if (t && t->pml4 && t->kill_pending) {
        proc_exit_current_user(130);   /* 128 + SIGINT */
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
