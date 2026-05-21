#include "fpu.h"

#include <stdint.h>

void fpu_init(void) {
    uint64_t cr0, cr4;

    /* CR0: clear EM, set MP/NE, clear TS. */
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((uint64_t)((1u << 2) | (1u << 3)));   /* clear EM, TS */
    cr0 |=  (uint64_t)((1u << 1) | (1u << 5));     /* set MP, NE  */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    /* CR4: enable OSFXSR + OSXMMEXCPT. */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (uint64_t)((1u << 9) | (1u << 10));
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    /* Init x87 state + a sane MXCSR. */
    __asm__ volatile("fninit");

    uint32_t mxcsr = 0x1F80;        /* mask all exceptions, round-to-nearest */
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
}
