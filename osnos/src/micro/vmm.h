#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * x86_64 4-level paging manager.
 *
 * Operates on PML4 pointers (HHDM-mapped virtual addresses of 4KB page
 * tables). vmm_init() builds `kernel_pml4` by cloning Limine's top-level
 * PML4 entries, then atomically switches CR3 — after that we own the
 * page tables and Limine's mapping is no longer authoritative.
 *
 * Today we keep using the cloned intermediate tables (sharing PDPT/PD/PT
 * with Limine), so initial mappings remain intact. Subsequent vmm_map
 * calls only allocate new intermediate tables when traversing into
 * unmapped regions.
 *
 * Page table entry flags follow Intel SDM bits 0..7 and bit 63.
 */

#define PTE_P   (1ULL << 0)   /* Present */
#define PTE_W   (1ULL << 1)   /* Writable */
#define PTE_U   (1ULL << 2)   /* User-accessible */
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_A   (1ULL << 5)   /* Accessed (set by HW) */
#define PTE_D   (1ULL << 6)   /* Dirty   (set by HW) */
#define PTE_NX  (1ULL << 63)  /* No-execute (requires EFER.NXE) */

#define PTE_ADDR_MASK 0x000ffffffffff000ULL

/*
 * Initialize the VMM: clone Limine's PML4 into a fresh one allocated
 * from PMM, then load it into CR3.
 *
 * After this returns, the kernel runs on our page tables and you may
 * vmm_map/vmm_unmap freely. Must be called AFTER pmm_init.
 */
void vmm_init(void);

uint64_t *vmm_kernel_pml4(void);

/*
 * Establish a mapping virt -> phys with the given flags.
 * `flags` typically includes PTE_W and/or PTE_U; PTE_P is OR'd in.
 * Allocates intermediate tables on demand from PMM.
 *
 * Both virt and phys must be 4KB-aligned. Returns 1 on success, 0 on OOM.
 */
int vmm_map(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* Remove a virt->phys mapping (4KB granularity). Does NOT free the phys. */
void vmm_unmap(uint64_t *pml4, uint64_t virt);

/*
 * Walk the tables and return the physical address that `virt` maps to,
 * plus the page offset (low 12 bits of virt). Returns 0 if unmapped.
 */
uint64_t vmm_lookup(uint64_t *pml4, uint64_t virt);

/* Count of non-zero entries in the top-level PML4 (for /sys/meminfo). */
size_t vmm_pml4_used(uint64_t *pml4);

/*
 * Per-process address space.
 *
 * `address_space_create` allocates a fresh PML4 from PMM and clones the
 * upper half (entries 256..511) from kernel_pml4. The lower half is
 * zeroed and is the future user portion: vmm_map(new_as, low_virt, ...)
 * adds user mappings without polluting the kernel namespace.
 *
 * `address_space_destroy` walks the lower half, frees every leaf page
 * AND every intermediate table back to PMM, then frees the PML4 page
 * itself. The upper half is shared with the kernel and is never touched.
 *
 * Returns NULL on OOM.
 */
uint64_t *address_space_create(void);
void      address_space_destroy(uint64_t *pml4);

/*
 * User-virtual range. Addresses below this belong to the current
 * address space's lower half (where user code/data goes).
 */
#define OSNOS_USER_VIRT_MAX  0x0000800000000000ULL
