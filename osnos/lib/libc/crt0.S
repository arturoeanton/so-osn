/*
 * crt0.S — entry stub for osnos user ELF programs linked with libc.
 *
 * The kernel iretq's into _start with RSP pointing at the System V
 * x86_64 init block:
 *
 *     (rsp + 0)                    : argc
 *     (rsp + 8)                    : argv[0]
 *     (rsp + 16)                   : argv[1]
 *     ...
 *     (rsp + 8 + 8*argc)           : NULL (argv terminator)
 *     (rsp + 8 + 8*(argc+1))       : envp[0] (always NULL for now)
 *     strings... (above the pointer block, NUL-terminated)
 *
 * Pulls argc, argv, envp into the SysV call registers (rdi, rsi, rdx),
 * aligns the stack to 16 bytes, calls main, and forwards the return
 * code into _exit. If _exit ever returns we trap.
 */

    .section .text
    .global _start
    .type   _start, @function
_start:
    movq    (%rsp), %rdi                   /* argc                  */
    leaq    8(%rsp), %rsi                  /* argv = rsp + 8        */
    leaq    16(%rsp,%rdi,8), %rdx          /* envp = rsp + 16 + 8*argc */

    /*
     * Align RSP to a 16-byte boundary immediately before the call.
     * SysV requires RSP & 15 == 0 at the call site (callee sees +8).
     */
    andq    $-16, %rsp

    call    main                            /* returns int in %eax  */

    movl    %eax, %edi                      /* exit_code            */
    call    _exit                           /* never returns        */

    /*
     * Defensive trap: if _exit returns (it shouldn't), drop into int3
     * so the page-fault / breakpoint handler kills the task instead
     * of corrupting downstream state.
     */
1:  int3
    jmp     1b
    .size   _start, . - _start
