#pragma once

/*
 * x87 + SSE init + per-task save/restore.
 *
 * Goal: enable hardware floating point for ring-3 ELFs so user
 * programs can use `double`/`float` and call into libc/math.c.
 * Kernel code is compiled with -mno-80387 -mno-sse so it never
 * touches FP registers.
 *
 * fpu_init():
 *   - CR0.EM (2) = 0   — disable software emulation; FP runs in HW.
 *   - CR0.MP (1) = 1   — required when EM=0; affects WAIT/FWAIT.
 *   - CR0.TS (3) = 0   — don't trap on FP after task switch.
 *   - CR0.NE (5) = 1   — native FP exception delivery (#MF / #XM).
 *   - CR4.OSFXSR     = 1   — OS supports FXSAVE / FXRSTOR.
 *   - CR4.OSXMMEXCPT = 1   — OS handles #XM (unmasked SSE excpt).
 *   - FNINIT             — clear x87 state.
 *   - LDMXCSR(0x1F80)    — default MXCSR (all excpt masked, RZ).
 *
 * Call once during early boot, before any user task is dispatched.
 */
void fpu_init(void);

/*
 * FXSAVE / FXRSTOR a 512-byte FP/SSE state block. `state` MUST
 * be aligned to 16 bytes — task_t.fpu_state guarantees that.
 * Called by the scheduler around context switches so multi-task
 * FP doesn't corrupt across them.
 */
void fpu_save   (void *state);
void fpu_restore(const void *state);

/*
 * Reset `state` to a clean FPU snapshot (FNINIT + default MXCSR).
 * Used at task_create_user_elf time so a fresh task's first
 * dispatch loads sane FP regs instead of uninitialised bytes.
 */
void fpu_state_init(void *state);
