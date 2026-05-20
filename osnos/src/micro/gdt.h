#pragma once

#include <stdint.h>

/*
 * Long-mode Global Descriptor Table.
 *
 * Layout (8 bytes per non-TSS entry):
 *   0x00  null
 *   0x08  kernel code  (CS, ring 0)
 *   0x10  kernel data  (DS, ring 0)
 *   0x18  user data    (DS, ring 3)   <- before user code (SYSRET ordering)
 *   0x20  user code    (CS, ring 3)
 *   0x28  TSS (16 bytes)
 *
 * The user-data-before-user-code order is mandatory for SYSCALL/SYSRET:
 *   SYSCALL  CS = STAR[47:32]      = 0x08 → kcode
 *            SS = STAR[47:32] + 8  = 0x10 → kdata
 *   SYSRET64 CS = STAR[63:48] + 16 | RPL3 = 0x10+16|3 = 0x23 → ucode
 *            SS = STAR[63:48] +  8 | RPL3 = 0x10+ 8|3 = 0x1b → udata
 *
 * Selectors used by the rest of the kernel:
 *   GDT_KCODE = 0x08
 *   GDT_KDATA = 0x10
 *   GDT_UDATA = 0x18 | 3 = 0x1b  (RPL 3)
 *   GDT_UCODE = 0x20 | 3 = 0x23
 *   GDT_TSS   = 0x28
 */

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA (0x18 | 3)
#define GDT_UCODE (0x20 | 3)

void gdt_init(void);

uint64_t gdt_limit(void);
uint64_t gdt_base(void);

/*
 * Install a 16-byte long-mode TSS descriptor at the given pair of GDT
 * slots (`slot` and `slot + 1`). Called by tss_init after the GDT is
 * already loaded. Selector = slot * 8.
 */
void gdt_install_tss(int slot, uint64_t base, uint32_t limit);
