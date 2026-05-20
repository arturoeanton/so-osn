#pragma once

#include <stdint.h>

/*
 * Periodic timer driver — PIT channel 0 at TIMER_HZ Hz, wired to
 * IDT vector 0x20 via the legacy PIC (IRQ 0).
 *
 * timer_init() must be called AFTER pic_init() (which masks all
 * IRQs and remaps the PIC vectors) and AFTER idt_init() (which
 * installs the default catch-all). timer_init then:
 *   - programs the PIT divisor for TIMER_HZ
 *   - installs its IRQ handler at IDT[0x20]
 *   - unmasks IRQ 0
 *
 * The caller is responsible for the final `sti` once every IRQ
 * source has been initialized.
 *
 * Counters are monotonic uint64_t — they will not wrap in practical
 * uptimes (millions of years).
 */

#define TIMER_HZ        100
#define TIMER_TICK_MS   (1000 / TIMER_HZ)

void     timer_init  (void);
uint64_t timer_ticks (void);   /* total ticks since timer_init */
uint64_t timer_ms    (void);   /* ticks * TIMER_TICK_MS         */
uint64_t timer_irqs  (void);   /* IRQs serviced (== ticks today, sanity counter) */
uint64_t timer_preempts(void); /* user-task preemptions performed (FASE 9.3b) */
