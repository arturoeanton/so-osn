#pragma once

#include <stddef.h>
#include <stdint.h>

#include <limine.h>

/*
 * Physical Memory Manager.
 *
 * Tracks 4 KiB pages of physical RAM via a bitmap stored inside the largest
 * USABLE region reported by Limine. Hands out physical addresses; the
 * caller is responsible for translating to virtual via the HHDM offset
 * (pmm_hhdm_offset) until a real VMM exists.
 *
 * Designed to be called exactly once during boot (pmm_init), then any
 * number of pmm_alloc_page / pmm_free_page calls.
 *
 *   uint64_t phys  = pmm_alloc_page();
 *   uint8_t  *virt = (uint8_t *)(phys + pmm_hhdm_offset());
 *   // use *virt ...
 *   pmm_free_page(phys);
 */

#define PAGE_SIZE 4096

void pmm_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
);

/* Returns a 4KB-aligned physical address; 0 on out-of-memory. */
uint64_t pmm_alloc_page(void);

/*
 * Allocate `n_pages` PHYSICALLY contiguous pages and return the base
 * physical address (4KB aligned). 0 on failure. Useful for DMA buffers
 * that hardware sees as one linear region. O(total_pages) linear scan.
 */
uint64_t pmm_alloc_pages_contig(size_t n_pages);

/* Releases a page back to the pool. No-op for double-free. */
void pmm_free_page(uint64_t phys);

size_t   pmm_total_pages(void);
size_t   pmm_free_pages(void);
uint64_t pmm_hhdm_offset(void);
