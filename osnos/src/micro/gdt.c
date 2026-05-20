#include "gdt.h"

/*
 * In long mode most fields of the legacy 8-byte segment descriptor are
 * ignored. The CPU still consults:
 *   access byte:    P, DPL, S, type
 *   granularity:    L (long mode), G
 * Everything else (base, limit) is fixed at 0 / max.
 *
 * Access byte:
 *   bit 7   P     present
 *   bit 6-5 DPL   descriptor privilege level (0 = kernel, 3 = user)
 *   bit 4   S     1 for code/data, 0 for system
 *   bit 3   E     executable (set for code)
 *   bit 2   DC    direction / conforming
 *   bit 1   RW    readable (code) / writable (data)
 *   bit 0   A     accessed (set by CPU)
 *
 * Flags nibble (granularity upper):
 *   bit 7   G   granularity (1 = 4KB)
 *   bit 6   DB  size (must be 0 when L=1)
 *   bit 5   L   long mode code
 *   bit 4   AVL software-available bit
 */

#define ACCESS_KCODE 0x9A   /* P=1 DPL=0 S=1 E=1 RW=1 */
#define ACCESS_KDATA 0x92
#define ACCESS_UCODE 0xFA   /* P=1 DPL=3 S=1 E=1 RW=1 */
#define ACCESS_UDATA 0xF2

#define FLAGS_LONG   0xA0   /* G=1, L=1 — long-mode code */
#define FLAGS_DATA   0xA0

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran_flags;
    uint8_t  base_high;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define GDT_ENTRY_COUNT 7  /* 5 used + 2 reserved for the future TSS desc */

static struct gdt_entry  gdt[GDT_ENTRY_COUNT];
static struct gdtr       gdtr;

static struct gdt_entry make_descriptor(uint8_t access, uint8_t flags) {
    struct gdt_entry e;
    e.limit_low  = 0xFFFF;
    e.base_low   = 0;
    e.base_mid   = 0;
    e.access     = access;
    e.gran_flags = (uint8_t)(flags | 0x0F);  /* keep low nibble = limit high bits = 0xF */
    e.base_high  = 0;
    return e;
}

void gdt_init(void) {
    gdt[0] = (struct gdt_entry){0};                       /* null */
    gdt[1] = make_descriptor(ACCESS_KCODE, FLAGS_LONG);   /* 0x08  kcode */
    gdt[2] = make_descriptor(ACCESS_KDATA, FLAGS_DATA);   /* 0x10  kdata */
    /*
     * The user-data descriptor comes BEFORE user-code: SYSRET64 reads
     * SS from STAR[63:48]+8 and CS from STAR[63:48]+16. With STAR base
     * at kdata (0x10), SS lands on this slot and CS on the next one.
     */
    gdt[3] = make_descriptor(ACCESS_UDATA, FLAGS_DATA);   /* 0x18  udata */
    gdt[4] = make_descriptor(ACCESS_UCODE, FLAGS_LONG);   /* 0x20  ucode */
    /* gdt[5..6] reserved for TSS (16-byte) descriptor */
    gdt[5] = (struct gdt_entry){0};
    gdt[6] = (struct gdt_entry){0};

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt;

    /*
     * Load GDTR, then reload segment registers. The far return reloads
     * CS via the stack-pushed pair (selector, RIP-of-next-instruction).
     */
    __asm__ volatile (
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"     /* kernel data selector */
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x08\n\t"         /* kernel code selector */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "m"(gdtr)
        : "rax", "memory"
    );
}

uint64_t gdt_limit(void) { return gdtr.limit; }
uint64_t gdt_base(void)  { return gdtr.base;  }

/*
 * Layout of a long-mode TSS descriptor (16 bytes = two gdt_entry slots):
 *
 *   slot[N]:
 *     limit_low     (16)
 *     base_low      (16)
 *     base_mid       (8)
 *     access         (8)  -> 0x89 = P=1 DPL=0 S=0 type=1001 (available TSS)
 *     gran_limit_hi  (8)  -> low 4 = limit[16..19], high 4 = G/AVL flags
 *     base_high      (8)
 *   slot[N+1]:
 *     base_upper    (32)  -> bits 32..63 of base
 *     reserved      (32)
 */
void gdt_install_tss(int slot, uint64_t base, uint32_t limit) {
    struct gdt_entry *lower = &gdt[slot];
    struct gdt_entry *upper = &gdt[slot + 1];

    lower->limit_low  = (uint16_t)(limit & 0xFFFF);
    lower->base_low   = (uint16_t)(base & 0xFFFF);
    lower->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    lower->access     = 0x89;                   /* available 64-bit TSS */
    lower->gran_flags = (uint8_t)((limit >> 16) & 0x0F);  /* G=0, limit hi */
    lower->base_high  = (uint8_t)((base >> 24) & 0xFF);

    /* The second slot stores base bits 32..63 in its first 4 bytes,
     * then 4 reserved bytes. Reuse the gdt_entry packed layout. */
    uint8_t *bytes = (uint8_t *)upper;
    bytes[0] = (uint8_t)((base >> 32) & 0xFF);
    bytes[1] = (uint8_t)((base >> 40) & 0xFF);
    bytes[2] = (uint8_t)((base >> 48) & 0xFF);
    bytes[3] = (uint8_t)((base >> 56) & 0xFF);
    bytes[4] = 0;
    bytes[5] = 0;
    bytes[6] = 0;
    bytes[7] = 0;
}
