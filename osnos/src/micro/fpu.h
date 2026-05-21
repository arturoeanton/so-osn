#pragma once

/*
 * x87 + SSE init.
 *
 * Goal: enable hardware floating point for ring-3 ELFs so user
 * programs can use `double`/`float` and call into libc/math.c.
 * Kernel code is compiled with -mno-80387 -mno-sse so it never
 * touches FP registers — no need for per-task FXSAVE/FXRSTOR
 * yet. Single-task FP works; multi-task FP across context
 * switches may corrupt state until that lands.
 *
 * fpu_init():
 *   - CR0.EM (2) = 0   — disable software emulation; FP runs in HW.
 *   - CR0.MP (1) = 1   — required when EM=0; affects WAIT/FWAIT.
 *   - CR0.TS (3) = 0   — don't trap on FP after task switch (we
 *                        don't save state, so nothing to "switch").
 *   - CR0.NE (5) = 1   — native FP exception delivery (#MF / #XM).
 *   - CR4.OSFXSR     = 1   — OS supports FXSAVE / FXRSTOR.
 *   - CR4.OSXMMEXCPT = 1   — OS handles #XM (unmasked SSE excpt).
 *   - FNINIT             — clear x87 state.
 *   - LDMXCSR(0x1F80)    — default MXCSR (all excpt masked, RZ).
 *
 * Call once during early boot, before any user task is dispatched.
 */
void fpu_init(void);
