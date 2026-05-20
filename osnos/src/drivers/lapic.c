#include "lapic.h"

#include "../micro/pmm.h"
#include "../micro/vmm.h"

#include <stdint.h>

#define IA32_APIC_BASE_MSR  0x1B
#define APIC_BASE_MASK      0xFFFFF000ULL   /* base addr in MSR */

/* LAPIC register offsets (32-bit, byte addresses). */
#define LAPIC_SVR    0x0F0     /* Spurious-Interrupt Vector Register */
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360

/* SVR bits. */
#define LAPIC_SVR_ENABLE  (1u << 8)

/* LVT delivery modes. */
#define LVT_MODE_FIXED   (0u << 8)
#define LVT_MODE_NMI     (4u << 8)
#define LVT_MODE_EXTINT  (7u << 8)

/* LVT mask bit. */
#define LVT_MASK         (1u << 16)

static inline uint64_t rdmsr(uint32_t idx) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(idx));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t idx, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(idx), "a"(lo), "d"(hi));
}

static volatile uint32_t *lapic_regs;

static inline uint32_t lapic_read(uint32_t off) {
    return lapic_regs[off / 4];
}

static inline void lapic_write(uint32_t off, uint32_t value) {
    lapic_regs[off / 4] = value;
}

void lapic_init(void) {
    /*
     * Read the MSR and make sure the global LAPIC enable bit (11) is
     * set, otherwise the MMIO region is dead. On most platforms the
     * BIOS leaves it on, but writing it back is cheap and idempotent.
     */
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= (1ULL << 11);                /* APIC global enable */
    wrmsr(IA32_APIC_BASE_MSR, base);

    uint64_t phys = base & APIC_BASE_MASK;
    uint64_t virt = phys + pmm_hhdm_offset();

    /*
     * Limine's HHDM maps RAM only — MMIO ranges (LAPIC at 0xFEE00000)
     * are NOT pre-mapped. Add the one page we need into the kernel
     * pml4 with cache disabled (LAPIC reads/writes must hit the chip,
     * never the cache).
     */
    if (!vmm_map(vmm_kernel_pml4(), virt, phys, PTE_W | PTE_PCD)) {
        return;
    }

    lapic_regs = (volatile uint32_t *)virt;

    /*
     * Software-enable the LAPIC and pin the spurious vector to 0xFF
     * (we don't install a handler for it; if it ever fires we'll see
     * EXCEPTION 255 from idt.c's generic catch-all).
     */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr  = (svr & ~0xFFu) | 0xFFu;
    lapic_write(LAPIC_SVR, svr);

    /*
     * Route LINT0 to ExtINT mode (unmasked). This wires the 8259's
     * INTR output through LAPIC into the CPU. LINT1 = NMI, unmasked.
     */
    lapic_write(LAPIC_LVT_LINT0, LVT_MODE_EXTINT);
    lapic_write(LAPIC_LVT_LINT1, LVT_MODE_NMI);
}
