#include "vmm.h"

#include "pmm.h"

static uint64_t *kernel_pml4;

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

static inline void invlpg(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* Index into the level-N table (level 4 = PML4, level 1 = PT). */
static int idx(uint64_t virt, int level) {
    int shift = 12 + 9 * (level - 1);
    return (int)((virt >> shift) & 0x1FFu);
}

/* Convert a table entry to the HHDM-mapped pointer of the table it
 * references. Assumes the entry is Present. */
static uint64_t *table_of(uint64_t entry) {
    return (uint64_t *)((entry & PTE_ADDR_MASK) + pmm_hhdm_offset());
}

/* Zero out a freshly-allocated 4KB page (used for new intermediate tables). */
static void zero_page(uint64_t *p) {
    for (int i = 0; i < 512; i++) p[i] = 0;
}

void vmm_init(void) {
    /* Clone Limine's top-level PML4 entries so all current mappings
     * (kernel, HHDM, framebuffer, identity low memory) carry over. */
    uint64_t  cur_cr3   = read_cr3();
    uint64_t *limine_p4 =
        (uint64_t *)((cur_cr3 & PTE_ADDR_MASK) + pmm_hhdm_offset());

    uint64_t our_phys = pmm_alloc_page();
    if (!our_phys) return;  /* OOM before any allocs -> nothing we can do */

    kernel_pml4 = (uint64_t *)(our_phys + pmm_hhdm_offset());
    for (int i = 0; i < 512; i++) {
        kernel_pml4[i] = limine_p4[i];
    }

    /* The scary line: from here on we use our own page tables. The
     * memory map is identical to Limine's so no fault should occur. */
    write_cr3(our_phys);
}

uint64_t *vmm_kernel_pml4(void) {
    return kernel_pml4;
}

/* Walks down one level, allocating an intermediate table if needed.
 * Returns the next table pointer, or NULL on OOM. */
static uint64_t *walk_or_alloc(uint64_t *table, int slot) {
    if (!(table[slot] & PTE_P)) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return 0;
        uint64_t *child = (uint64_t *)(phys + pmm_hhdm_offset());
        zero_page(child);
        /* Intermediate tables are P|W|U; the leaf PT entry actually
         * constrains access via its own flags. */
        table[slot] = phys | PTE_P | PTE_W | PTE_U;
        return child;
    }
    return table_of(table[slot]);
}

int vmm_map(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt & 0xFFF) return 0;
    if (phys & 0xFFF) return 0;

    uint64_t *pdpt = walk_or_alloc(pml4, idx(virt, 4));
    if (!pdpt) return 0;
    uint64_t *pd   = walk_or_alloc(pdpt, idx(virt, 3));
    if (!pd) return 0;
    uint64_t *pt   = walk_or_alloc(pd,   idx(virt, 2));
    if (!pt) return 0;

    pt[idx(virt, 1)] = (phys & PTE_ADDR_MASK) | flags | PTE_P;
    invlpg(virt);
    return 1;
}

void vmm_unmap(uint64_t *pml4, uint64_t virt) {
    if (!(pml4[idx(virt, 4)] & PTE_P)) return;
    uint64_t *pdpt = table_of(pml4[idx(virt, 4)]);

    if (!(pdpt[idx(virt, 3)] & PTE_P)) return;
    uint64_t *pd = table_of(pdpt[idx(virt, 3)]);

    if (!(pd[idx(virt, 2)] & PTE_P)) return;
    uint64_t *pt = table_of(pd[idx(virt, 2)]);

    pt[idx(virt, 1)] = 0;
    invlpg(virt);
}

uint64_t vmm_lookup(uint64_t *pml4, uint64_t virt) {
    if (!(pml4[idx(virt, 4)] & PTE_P)) return 0;
    uint64_t *pdpt = table_of(pml4[idx(virt, 4)]);

    if (!(pdpt[idx(virt, 3)] & PTE_P)) return 0;
    uint64_t *pd = table_of(pdpt[idx(virt, 3)]);

    if (!(pd[idx(virt, 2)] & PTE_P)) return 0;
    uint64_t *pt = table_of(pd[idx(virt, 2)]);

    uint64_t pte = pt[idx(virt, 1)];
    if (!(pte & PTE_P)) return 0;
    return (pte & PTE_ADDR_MASK) | (virt & 0xFFFu);
}

size_t vmm_pml4_used(uint64_t *pml4) {
    size_t n = 0;
    for (int i = 0; i < 512; i++) {
        if (pml4[i] & PTE_P) n++;
    }
    return n;
}

uint64_t *address_space_create(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;

    uint64_t *pml4 = (uint64_t *)(phys + pmm_hhdm_offset());
    zero_page(pml4);

    /* Upper half: shared with the kernel. Cloning at PML4 granularity
     * means we share PDPT/PD/PT down the chain — kernel updates remain
     * visible in every address space. */
    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel_pml4[i];
    }

    return pml4;
}

/*
 * address_space_clone — deep-copy the user low-half of `parent` into
 * a freshly allocated PML4. Kernel high-half is shared via the same
 * mechanism as address_space_create (top 256 entries point at the
 * kernel's shared upper tables).
 *
 * Used by sys_fork: child gets identical mappings + identical bytes,
 * but distinct physical pages so writes don't trample the parent.
 * This is the "full copy" implementation — no copy-on-write yet, so
 * forking a process with a 1 MiB heap costs 1 MiB of physical RAM up
 * front. Acceptable for our 64 KiB-ish user programs.
 *
 * Walks pml4[0..255], every present PDPT/PD/PT, allocates a new
 * physical page for each leaf, memcpys via HHDM. If any allocation
 * fails midway, frees what was allocated so far and returns NULL.
 *
 * Returns the HHDM-mapped pointer to the new PML4 (just like
 * address_space_create). On NULL, caller must handle ENOMEM.
 */
uint64_t *address_space_clone(const uint64_t *parent) {
    if (!parent) return 0;

    uint64_t *child = address_space_create();
    if (!child) return 0;

    for (int i = 0; i < 256; i++) {
        if (!(parent[i] & PTE_P)) continue;
        const uint64_t *p_pdpt = (const uint64_t *)
            ((parent[i] & PTE_ADDR_MASK) + pmm_hhdm_offset());

        for (int j = 0; j < 512; j++) {
            if (!(p_pdpt[j] & PTE_P)) continue;
            const uint64_t *p_pd = (const uint64_t *)
                ((p_pdpt[j] & PTE_ADDR_MASK) + pmm_hhdm_offset());

            for (int k = 0; k < 512; k++) {
                if (!(p_pd[k] & PTE_P)) continue;
                const uint64_t *p_pt = (const uint64_t *)
                    ((p_pd[k] & PTE_ADDR_MASK) + pmm_hhdm_offset());

                for (int l = 0; l < 512; l++) {
                    if (!(p_pt[l] & PTE_P)) continue;
                    uint64_t flags    = p_pt[l] & ~PTE_ADDR_MASK;
                    uint64_t src_phys = p_pt[l] & PTE_ADDR_MASK;

                    uint64_t dst_phys = pmm_alloc_page();
                    if (!dst_phys) {
                        address_space_destroy(child);
                        return 0;
                    }
                    /* Page-sized memcpy via HHDM (both src + dst
                     * are mapped in the kernel's high-half). */
                    uint8_t *src = (uint8_t *)(src_phys + pmm_hhdm_offset());
                    uint8_t *dst = (uint8_t *)(dst_phys + pmm_hhdm_offset());
                    for (int b = 0; b < 4096; b++) dst[b] = src[b];

                    /* Reconstruct the virtual address from indices
                     * and map the new physical page with the same
                     * flags. vmm_map allocates intermediate tables
                     * as needed. */
                    uint64_t virt =
                        ((uint64_t)i << 39) |
                        ((uint64_t)j << 30) |
                        ((uint64_t)k << 21) |
                        ((uint64_t)l << 12);
                    /* Sign-extend bit 47 to canonical form (low half
                     * stays low, since i < 256 means bit 47 == 0). */
                    /* vmm_map returns 1 on success, 0 on failure
                     * (non-standard but matches the rest of the
                     * file). Bail on failure. */
                    if (vmm_map(child, virt, dst_phys, flags) == 0) {
                        pmm_free_page(dst_phys);
                        address_space_destroy(child);
                        return 0;
                    }
                }
            }
        }
    }

    return child;
}

void address_space_destroy(uint64_t *pml4) {
    if (!pml4) return;
    if (pml4 == kernel_pml4) return;  /* never destroy the kernel AS */

    /* Walk only the lower half (user). Free leaf pages, then the
     * intermediate tables, then the PML4 itself. */
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_P)) continue;
        uint64_t *pdpt = table_of(pml4[i]);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_P)) continue;
            uint64_t *pd = table_of(pdpt[j]);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_P)) continue;
                uint64_t *pt = table_of(pd[k]);

                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PTE_P) {
                        pmm_free_page(pt[l] & PTE_ADDR_MASK);
                    }
                }
                pmm_free_page(pd[k] & PTE_ADDR_MASK);
            }
            pmm_free_page(pdpt[j] & PTE_ADDR_MASK);
        }
        pmm_free_page(pml4[i] & PTE_ADDR_MASK);
    }

    uint64_t pml4_phys = (uint64_t)pml4 - pmm_hhdm_offset();
    pmm_free_page(pml4_phys);
}
