/*
 * lib/libc/sigtramp.S — signal-handler trampoline epilogue.
 *
 * The osnos kernel delivers a signal by:
 *   1. Pushing a sigframe (saved iret + GPRs) onto the user stack.
 *   2. Setting up the user iret frame so it lands in the user's
 *      signal handler with rdi = signum.
 *   3. Setting the return address (top of user stack at iret time)
 *      to __sigtramp. When the handler executes `ret` it jumps here.
 *
 * __sigtramp's only job is to invoke SYS_RT_SIGRETURN (#15), which
 * tells the kernel to pop the sigframe and restore the original
 * user context (the state right before the signal interrupted us).
 * That syscall NEVER returns to userland directly — the kernel
 * iret's back to the pre-signal RIP/RSP with the original GPRs.
 *
 * If the handler somehow returned to a different point (e.g. via
 * longjmp), we never reach __sigtramp — the user is responsible
 * for the sigmask state in that case.
 */

    .section .text
    .global __sigtramp
    .type   __sigtramp, @function
__sigtramp:
    movq    $15, %rax              /* SYS_RT_SIGRETURN */
    syscall
    /* Unreachable — sigreturn doesn't come back here. */
1:  hlt
    jmp     1b
    .size   __sigtramp, . - __sigtramp
