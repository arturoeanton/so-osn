#pragma once

/*
 * setjmp / longjmp — non-local jump within the same task.
 *
 * jmp_buf stores the System V AMD64 callee-saved registers plus the
 * return address and the caller's RSP. No signal mask (we don't have
 * signals yet — sigsetjmp/siglongjmp will alias this when they land).
 *
 * Slots:
 *   0: rbx     1: rbp     2: r12     3: r13
 *   4: r14     5: r15     6: rsp     7: rip
 *
 * Caller-saved regs (rax/rcx/rdx/rsi/rdi/r8-r11) are NOT preserved.
 * That's the standard contract — they're scratch across function
 * calls and the compiler reloads them after the longjmp.
 */

typedef unsigned long jmp_buf[8];

int  setjmp (jmp_buf env);
__attribute__((noreturn))
void longjmp(jmp_buf env, int val);

/* sigjmp_buf / sigsetjmp / siglongjmp aliases — same layout. */
typedef jmp_buf sigjmp_buf;
int  sigsetjmp (sigjmp_buf env, int savesigs);
__attribute__((noreturn))
void siglongjmp(sigjmp_buf env, int val);
