/*
 * setjmp / longjmp for osnos libc.
 *
 * System V AMD64 calling convention:
 *   rdi = jmp_buf env
 *   rsi = val (longjmp only)
 *
 * jmp_buf layout (offsets in bytes):
 *    0  rbx
 *    8  rbp
 *   16  r12
 *   24  r13
 *   32  r14
 *   40  r15
 *   48  rsp  (caller's RSP after the call instruction popped its
 *             return address — i.e., points at the parent frame)
 *   56  rip  (return address of the caller of setjmp)
 *
 * setjmp returns 0 on the direct call; the longjmp-triggered return
 * delivers `val` instead (or 1 if val was 0, per the C spec).
 */

    .section .text

    .global setjmp
    .type setjmp, @function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    /* %rsp at entry already has the return address popped off the
     * call-site by `call setjmp` — we want the value *before* the
     * push, so adjust by +8 to skip the return slot. */
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax           /* return address = rip to restore */
    movq %rax, 56(%rdi)
    xorl %eax, %eax             /* setjmp returns 0 directly */
    ret
    .size setjmp, .-setjmp

    .global longjmp
    .type longjmp, @function
longjmp:
    movq %rsi, %rax             /* return value first — survives reloads */
    testl %eax, %eax
    jnz  1f
    movl $1, %eax               /* longjmp(env, 0) returns 1, per spec */
1:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    /* Resume at the saved rip. Avoid touching any register the
     * callee might rely on — just jump. */
    jmpq *56(%rdi)
    .size longjmp, .-longjmp

    /* sigsetjmp / siglongjmp — same as setjmp/longjmp (no signal
     * mask to save while signals aren't a thing). */
    .global sigsetjmp
    .type sigsetjmp, @function
sigsetjmp:
    jmp setjmp
    .size sigsetjmp, .-sigsetjmp

    .global siglongjmp
    .type siglongjmp, @function
siglongjmp:
    jmp longjmp
    .size siglongjmp, .-siglongjmp

    .section .note.GNU-stack,"",@progbits
