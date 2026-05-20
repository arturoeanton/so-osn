#pragma once

/*
 * SYSCALL/SYSRET MSR programming.
 *
 * Enables the modern fast-syscall path on x86_64:
 *   - EFER.SCE  = 1
 *   - STAR      = ((kdata << 48) | (kcode << 32))    (selectors)
 *   - LSTAR     = syscall_entry                       (entry point)
 *   - FMASK     = bits to clear in RFLAGS on entry
 *
 * After this, ring-3 code can use the `syscall` instruction instead of
 * `int 0x80`. Both paths reach the same syscall_dispatch and share the
 * same Linux x86_64 ABI (rax = #, rdi/rsi/rdx/r10/r8/r9 = args).
 *
 * Must be called after idt_init (since both paths share the dispatcher)
 * and after gdt_init (so STAR's encoded selectors match the live GDT).
 */
void syscall_msr_init(void);
