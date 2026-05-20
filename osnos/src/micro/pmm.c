#include "pmm.h"

#include <stdbool.h>

/*
 * Bitmap layout:
 *   1 bit per PAGE_SIZE-aligned physical address.
 *   bit = 0  -> page is free
 *   bit = 1  -> page is used (or outside any USABLE region)
 *
 * The bitmap itself lives inside the first USABLE region that's large
 * enough; we then mark its own pages as used so allocations never hand
 * back the bitmap memory.
 *
 * `first_free_hint` is an O(1) hot pointer to "probably the next free
 * page" — keeps allocation near O(1) instead of scanning from 0 every
 * time.
 */

static uint8_t *bitmap;
static size_t   bitmap_bytes;
static size_t   total_pages;
static size_t   free_pages;
static size_t   first_free_hint;
static uint64_t hhdm;

static void bit_set(size_t page) {
    bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

static void bit_clear(size_t page) {
    bitmap[page / 8] &= (uint8_t)~(1u << (page % 8));
}

static int bit_get(size_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

void pmm_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
) {
    hhdm = hhdm_offset;

    /* Pass 1: find highest USABLE address -> sizes the bitmap. */
    uint64_t highest = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > highest) highest = end;
    }

    total_pages = (size_t)(highest / PAGE_SIZE);
    bitmap_bytes = (total_pages + 7) / 8;

    /* Pass 2: pick a region big enough for the bitmap. */
    bitmap = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->length >= bitmap_bytes) {
            bitmap = (uint8_t *)(e->base + hhdm);
            break;
        }
    }
    if (!bitmap) {
        /* No region big enough — should not happen on QEMU. */
        return;
    }

    /* Start with everything marked used. */
    for (size_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0xFF;

    /* Pass 3: mark each USABLE page free. */
    free_pages = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        for (uint64_t addr = e->base;
             addr + PAGE_SIZE <= e->base + e->length;
             addr += PAGE_SIZE) {
            size_t page = (size_t)(addr / PAGE_SIZE);
            if (bit_get(page)) {
                bit_clear(page);
                free_pages++;
            }
        }
    }

    /* Re-mark the bitmap's own pages as used. */
    uintptr_t bitmap_phys = (uintptr_t)bitmap - (uintptr_t)hhdm;
    size_t bitmap_first  = (size_t)(bitmap_phys / PAGE_SIZE);
    size_t bitmap_pages  = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t p = 0; p < bitmap_pages; p++) {
        size_t page = bitmap_first + p;
        if (page >= total_pages) break;
        if (!bit_get(page)) {
            bit_set(page);
            free_pages--;
        }
    }

    first_free_hint = 0;
}

static int64_t scan_free_from(size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
        if (!bit_get(i)) return (int64_t)i;
    }
    return -1;
}

uint64_t pmm_alloc_page(void) {
    if (!bitmap || free_pages == 0) return 0;

    int64_t idx = scan_free_from(first_free_hint, total_pages);
    if (idx < 0) {
        idx = scan_free_from(0, first_free_hint);
        if (idx < 0) return 0;  /* shouldn't happen if free_pages > 0 */
    }

    size_t page = (size_t)idx;
    bit_set(page);
    free_pages--;
    first_free_hint = page + 1;

    return (uint64_t)page * PAGE_SIZE;
}

uint64_t pmm_alloc_pages_contig(size_t n_pages) {
    if (!bitmap || n_pages == 0) return 0;
    if (free_pages < n_pages)    return 0;

    /*
     * Linear scan from the start. For boot-time DMA buffers (called
     * once or twice during driver init) the cost is irrelevant; for
     * runtime allocations the caller would need a smarter strategy.
     */
    size_t i = 0;
    while (i + n_pages <= total_pages) {
        bool ok = true;
        for (size_t k = 0; k < n_pages; k++) {
            if (bit_get(i + k)) {
                /* Skip past the offending page on next attempt. */
                i = i + k + 1;
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        for (size_t k = 0; k < n_pages; k++) bit_set(i + k);
        free_pages -= n_pages;
        if (first_free_hint < i + n_pages) first_free_hint = i + n_pages;
        return (uint64_t)i * PAGE_SIZE;
    }
    return 0;
}

void pmm_free_page(uint64_t phys) {
    if (!bitmap) return;
    if (phys & (PAGE_SIZE - 1)) return;          /* not aligned */
    size_t page = (size_t)(phys / PAGE_SIZE);
    if (page >= total_pages) return;
    if (!bit_get(page)) return;                  /* double free */

    bit_clear(page);
    free_pages++;
    if (page < first_free_hint) first_free_hint = page;
}

size_t   pmm_total_pages(void)  { return total_pages; }
size_t   pmm_free_pages(void)   { return free_pages; }
uint64_t pmm_hhdm_offset(void)  { return hhdm; }
