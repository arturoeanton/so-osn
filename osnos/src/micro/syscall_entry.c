#include <stdint.h>

#include "syscall.h"

/*
 * SYSCALL fast-path entry stub.
 *
 * On `syscall` from CPL=3 the CPU:
 *   - Loads CS/SS from STAR (no stack switch — RSP keeps the user RSP)
 *   - Saves user RIP into RCX, user RFLAGS into R11
 *   - Masks RFLAGS by FMASK and jumps to LSTAR (this entry)
 *
 * The stub must:
 *   1. Save user RSP somewhere kernel can find it later
 *   2. Switch to the current task's kernel stack (TSS.RSP0 mirror)
 *   3. Build a syscall_frame_t exactly like int80_entry does, but also
 *      preserve RCX (user RIP) and R11 (user RFLAGS) across the C call
 *   4. Invoke the shared int80_dispatch_wrapper
 *   5. Restore registers, restore user RSP, sysretq
 *
 * Register contract (matches Linux x86_64 SYSCALL ABI):
 *   RAX            = syscall number; on return holds the result
 *   RDI/RSI/RDX/R10/R8/R9 = arguments 0..5  (R10 replaces RCX because
 *                          RCX carries the user RIP for SYSRET)
 *   RCX, R11        = clobbered by SYSCALL itself (user RIP / RFLAGS)
 *
 * The dispatcher operates on a syscall_frame_t whose layout exactly
 * matches what we push here, so it is shared verbatim with int 0x80.
 */

/*
 * Saved user RSP. SYSCALL does not switch stacks, so we stash it here
 * before swapping to the kernel stack. Single-CPU only — once we have
 * SMP this becomes per-CPU. The pointer must live in kernel data
 * (high half) so it is reachable regardless of CR3.
 */
uint64_t syscall_user_rsp;

/*
 * SYSCALL path. Unlike int 0x80 (where the CPU pushes a 5-entry iret
 * frame for us), SYSCALL leaves the kernel stack untouched and stuffs
 * user RIP/RFLAGS into RCX/R11. To keep the post-entry kernel-stack
 * layout identical to int80 — so sys_nanosleep & friends can read the
 * iret frame at a fixed offset — we synthesize the iret frame by
 * hand and use IRETQ for the return (slightly slower than SYSRET but
 * uniform with int 0x80).
 *
 * Layout after entry (low → high addresses):
 *   [ syscall_frame_t (15 GPRs) ] [ iret frame: rip cs rflags rsp ss ]
 *                  RSP                                            kstack_top
 */
__asm__ (
    ".global syscall_entry\n"
    "syscall_entry:\n"

    /* 1. Stash user RSP, swap to kernel stack. */
    "    movq %rsp, syscall_user_rsp(%rip)\n"
    "    movq tss_kernel_rsp0(%rip), %rsp\n"

    /* 2. Push synthesized iret frame: ss, rsp, rflags, cs, rip. */
    "    pushq $0x1b\n"                 /* SS = GDT_UDATA */
    "    pushq syscall_user_rsp(%rip)\n"
    "    pushq %r11\n"                  /* user RFLAGS (saved by SYSCALL) */
    "    pushq $0x23\n"                 /* CS = GDT_UCODE */
    "    pushq %rcx\n"                  /* user RIP (saved by SYSCALL) */

    /* 3. Push the 15-GPR frame (same layout as int80_entry). */
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

    "    movq %rsp, %rdi\n"             /* arg1 = frame* */

    "    movq %rsp, %r12\n"
    "    andq $-16, %rsp\n"

    "    call int80_dispatch_wrapper\n"

    "    movq %r12, %rsp\n"

    "    movq %rax, (%rsp)\n"           /* overwrite frame.rax with retval */

    /* 4. Restore frame. */
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

    /* 5. Iret frame is on top of the stack; iretq pops it. */
    "    iretq\n"
);
