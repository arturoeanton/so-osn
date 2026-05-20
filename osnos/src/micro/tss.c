#include "tss.h"

#include <stddef.h>

#include "gdt.h"

/*
 * Single kernel stack for now. When the scheduler dispatches a ring-3
 * task, it will overwrite tss.rsp0 with that task's per-task kernel
 * stack (allocated in FASE 6.3 alongside the ELF/ring-3 work).
 */

#define KERNEL_STACK_SIZE 16384

static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
static tss_t   tss __attribute__((aligned(16)));

uint64_t tss_kernel_rsp0;   /* see tss.h — mirror for the SYSCALL entry */

/*
 * Long-mode TSS descriptor lives in two consecutive GDT slots (16 bytes
 * total). Lower half mirrors a normal descriptor; upper half holds the
 * high 32 bits of the base address plus padding.
 *
 * Type nibble = 0x9 = "available 64-bit TSS".
 */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran_limit_high;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

extern void gdt_install_tss(int slot, uint64_t base, uint32_t limit);

void tss_init(void) {
    for (size_t i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;

    /* Push RSP0 to the top of the stack (stack grows down). */
    tss.rsp0          = (uint64_t)(uintptr_t)(kernel_stack + KERNEL_STACK_SIZE);
    tss_kernel_rsp0   = tss.rsp0;

    /* IOPB offset past the TSS limit = no I/O permitted from ring 3.
     * Any `in`/`out` from user mode traps as #GP. */
    tss.iopb_offset = sizeof(tss_t);

    gdt_install_tss(5, (uint64_t)(uintptr_t)&tss, sizeof(tss_t) - 1);

    /* Load Task Register. */
    __asm__ volatile ("ltr %w0" :: "r"((uint16_t)GDT_TSS));
}

tss_t   *tss_get(void)              { return &tss; }
uint64_t tss_get_rsp0(void)         { return tss.rsp0; }
void     tss_set_rsp0(uint64_t r)   { tss.rsp0 = r; tss_kernel_rsp0 = r; }
