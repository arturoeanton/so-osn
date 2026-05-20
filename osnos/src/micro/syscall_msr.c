#include "syscall_msr.h"

#include <stdint.h>

#include "gdt.h"

#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_FMASK   0xC0000084

#define EFER_SCE    (1ULL << 0)   /* System Call Extensions enable */

/* Defined in src/micro/syscall_entry.c */
extern void syscall_entry(void);

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

void syscall_msr_init(void) {
    /*
     * STAR encodes two segment-selector bases:
     *   [47:32] : SYSCALL  -> CS = base,    SS = base + 8       (DPL=0)
     *   [63:48] : SYSRET64 -> CS = base+16, SS = base + 8       (DPL=3 forced)
     *
     * Our GDT was reorganized so that with base = GDT_KDATA (0x10):
     *   SYSCALL  CS=0x08 (kcode), SS=0x10 (kdata)
     *   SYSRET64 CS=0x23 (ucode), SS=0x1b (udata)
     */
    uint64_t star = ((uint64_t)GDT_KDATA << 48) | ((uint64_t)GDT_KCODE << 32);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /*
     * Clear on entry: TF (8) | IF (9) | DF (10) | AC (18) = 0x40700.
     * IF clear is the important one — kernel must run with interrupts
     * disabled until we set up our own context. TF / DF / AC are
     * defensive (don't inherit single-step or string-direction state
     * from user mode).
     */
    wrmsr(MSR_FMASK, 0x40700ULL);

    /* Finally enable SYSCALL/SYSRET by setting EFER.SCE. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}
