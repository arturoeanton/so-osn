#pragma once

/*
 * Local APIC compatibility-mode setup.
 *
 * On modern x86_64 chipsets (QEMU q35, real ICH9+) the legacy 8259's
 * INTR output is wired to the LAPIC's LINT0 pin, NOT directly to the
 * CPU. With the LAPIC software-disabled (default on power-on) or with
 * LINT0 masked, 8259 IRQs never reach the core — the 8259 latches them
 * in its IRR but the CPU never sees an INTR.
 *
 * lapic_init enables the LAPIC at the software level and programs:
 *   LINT0 = ExtINT (delivery mode 7, unmasked) — 8259 passthrough
 *   LINT1 = NMI    (delivery mode 4, unmasked)
 *
 * This restores legacy IRQ delivery without needing to set up the
 * IOAPIC. When we eventually want SMP / per-CPU timers / MSI, the
 * IOAPIC + LAPIC vectors become the primary path; until then the
 * 8259 carries every IRQ via LINT0.
 *
 * Must run AFTER pmm_init (it reads pmm_hhdm_offset) and BEFORE the
 * first `sti`.
 */

void lapic_init(void);
