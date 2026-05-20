#pragma once

/*
 * Legacy 8259 PIC pair.
 *
 * After pic_init(), the chips deliver hardware IRQs at IDT vectors
 *   0x20..0x27   master  (IRQ 0..7)
 *   0x28..0x2F   slave   (IRQ 8..15)
 *
 * All lines are masked by default — pic_unmask() each one when its
 * handler is installed.
 *
 * IRQ assignments we care about:
 *   IRQ 0  — PIT (timer tick)
 *   IRQ 1  — PS/2 keyboard (future, FASE 9.x once preemption is on)
 *   IRQ 2  — slave cascade (handled by hardware, never used directly)
 */

#define PIC_VECTOR_BASE_MASTER 0x20
#define PIC_VECTOR_BASE_SLAVE  0x28

#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1

void pic_init    (void);
void pic_mask    (int irq);
void pic_unmask  (int irq);
void pic_send_eoi(int irq);
