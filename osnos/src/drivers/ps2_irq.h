#pragma once

#include <stdint.h>

/*
 * IRQ-driven PS/2 input (FASE 15.0 — input latency rework).
 *
 * Replaces the cooperative ring-0 "keyboard" / "mouse" feeder tasks
 * that polled the 8042 on every scheduler round. The 8042 now raises
 * IRQ 1 (keyboard) / IRQ 12 (AUX mouse) per byte; the handlers reuse
 * the existing keyboard_poll / mouse_poll state machines and push the
 * decoded events into the same /dev/input0 + /dev/mouse0 devfs rings,
 * so ring-3 consumers (kbdsrv, oxsrv) see no difference — except
 * events now arrive within microseconds instead of "whenever the
 * feeder task got dispatched".
 *
 * Removing the always-READY feeders is also what lets the scheduler
 * HLT when idle instead of busy-spinning.
 *
 * Call AFTER keyboard_init() + mouse_init() (their command/ACK
 * sequences poll 0x60 directly — interrupts must not steal those
 * bytes). Safe while IF=0: the PIC lines stay latched until sti.
 */
void ps2_irq_init(void);

uint64_t ps2_kbd_irqs  (void);   /* diagnostics: IRQ1 count  */
uint64_t ps2_mouse_irqs(void);   /* diagnostics: IRQ12 count */
