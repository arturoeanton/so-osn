#pragma once

#include <stdint.h>

/*
 * Long-mode Interrupt Descriptor Table.
 *
 * 256 entries × 16 bytes = 4 KiB. Each entry points to a handler
 * function compiled with __attribute__((interrupt)) so clang emits the
 * proper save/restore + iretq epilogue.
 *
 * Today every vector ends up in a handler that prints "EXCEPTION N" and
 * halts. The intent is to STOP triple-faults — any unhandled exception
 * gets a controlled panic. Specific handlers (page fault, GPF, DF, etc.)
 * print extra diagnostic info.
 */

void idt_init(void);

uint64_t idt_limit(void);
uint64_t idt_base(void);

/* Total count of exceptions handled since boot, surfaced via sysfs. */
uint64_t idt_exception_count(void);

/*
 * Register a handler for an IDT vector after idt_init() has run. Used
 * by drivers that install IRQ handlers (timer, eventually keyboard,
 * disk, etc.). dpl is 0 for hardware IRQs / kernel-only exceptions,
 * 3 for software interrupts callable from ring 3 (int 0x80, int3).
 *
 * `handler` must be a function declared with __attribute__((interrupt)).
 */
void idt_set_handler(int vec, void *handler, int dpl);
