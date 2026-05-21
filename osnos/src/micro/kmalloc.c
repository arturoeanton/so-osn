#include "kmalloc.h"

#include <stdbool.h>
#include <stdint.h>

#include "pmm.h"
#include "vmm.h"

/*
 * Block layout: every block (free or allocated) starts with a header
 * followed by its payload. Blocks form a singly-linked list in
 * address order so we can coalesce on free. `size` is the payload
 * size, NOT including the header.
 *
 *   | hdr | payload  | hdr | payload | hdr | free... |
 *
 * For an allocated block, the user receives `(uint8_t *)hdr + sizeof(hdr)`.
 * On kfree we step back by sizeof(hdr) to find the header.
 *
 * Coalesce on free: with the next block (cheap, hdr->next) and with the
 * previous block (O(n) walk from head; fine at our scale).
 *
 * Growth (FASE A): when kmalloc can't find a free block big enough, we
 * map more pages contiguous to the current heap end and append a new
 * free block. If that block is adjacent to the previous free tail it
 * merges so a series of small allocs that triggered growth can still
 * satisfy a single large request.
 */

typedef struct kblock {
    size_t        size;   /* payload bytes */
    bool          free;
    struct kblock *next;
} kblock_t;

#define KHEAP_VIRT_BASE   0xffffc00000000000ULL
#define KHEAP_INIT_PAGES  16
#define KHEAP_GROW_PAGES  16
#define KHEAP_MAX_BYTES   (4ULL * 1024 * 1024)   /* 4 MiB hard cap */

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static kblock_t *heap_head;
static size_t    heap_total;
static size_t    heap_used;  /* bytes of payload currently allocated */
static size_t    heap_peak;
static size_t    grow_events;
static size_t    grow_oom;

static void account_alloc(size_t n) {
    heap_used += n;
    if (heap_used > heap_peak) heap_peak = heap_used;
}

void kheap_init(void) {
    uint64_t *pml4 = vmm_kernel_pml4();

    for (size_t i = 0; i < KHEAP_INIT_PAGES; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return;  /* OOM at boot; nothing we can do */
        vmm_map(pml4,
                KHEAP_VIRT_BASE + i * PAGE_SIZE,
                phys,
                PTE_W);
    }

    heap_total  = KHEAP_INIT_PAGES * PAGE_SIZE;
    heap_used   = 0;
    heap_peak   = 0;
    grow_events = 0;
    grow_oom    = 0;

    heap_head        = (kblock_t *)KHEAP_VIRT_BASE;
    heap_head->size  = heap_total - sizeof(kblock_t);
    heap_head->free  = true;
    heap_head->next  = 0;
}

/*
 * Map `pages` more 4 KiB frames at the current heap end and splice the
 * region in as a new free block. Returns the newly created block, or
 * NULL on cap-reached / PMM-empty (in which case nothing is mapped).
 *
 * Page failures partway through unmap what we did and bail. We don't
 * try to recover the half-mapped region — at our scale a leak of one
 * or two pages on the path to OOM is academic.
 */
static kblock_t *heap_grow(size_t pages) {
    if (pages == 0) pages = 1;
    if (heap_total + pages * PAGE_SIZE > KHEAP_MAX_BYTES) {
        size_t room_bytes = (KHEAP_MAX_BYTES > heap_total)
                                ? KHEAP_MAX_BYTES - heap_total : 0;
        pages = room_bytes / PAGE_SIZE;
        if (pages == 0) { grow_oom++; return 0; }
    }

    uint64_t *pml4 = vmm_kernel_pml4();
    uint64_t start_va = KHEAP_VIRT_BASE + heap_total;

    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* PMM dry — back out: unmap what we added so the heap stays
             * consistent. The pages we did alloc are released. */
            for (size_t j = 0; j < i; j++) {
                vmm_unmap(pml4, start_va + j * PAGE_SIZE);
            }
            grow_oom++;
            return 0;
        }
        vmm_map(pml4, start_va + i * PAGE_SIZE, phys, PTE_W);
    }

    size_t grew_bytes = pages * PAGE_SIZE;

    /* Build a new free block over the new region. */
    kblock_t *nb = (kblock_t *)start_va;
    nb->size = grew_bytes - sizeof(kblock_t);
    nb->free = true;
    nb->next = 0;

    /* Splice at the tail. The current tail is the last block reachable
     * from head; if it's free, coalesce with the new region into one
     * contiguous block. */
    kblock_t *tail = heap_head;
    while (tail->next) tail = tail->next;

    if (tail->free) {
        /* Tail block free → merge: absorb our nb into tail. */
        tail->size += sizeof(kblock_t) + nb->size;
        /* tail->next stays NULL. */
    } else {
        tail->next = nb;
    }

    heap_total  += grew_bytes;
    grow_events += 1;
    return nb;
}

/* Single-pass first-fit; returns NULL if no block fits. */
static void *find_fit(size_t need) {
    for (kblock_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < need) continue;

        /* Split when the leftover can hold another header + a small
         * payload. Otherwise hand out the whole block (slight waste). */
        if (b->size >= need + sizeof(kblock_t) + 16) {
            kblock_t *tail = (kblock_t *)((uint8_t *)b + sizeof(kblock_t) + need);
            tail->size = b->size - need - sizeof(kblock_t);
            tail->free = true;
            tail->next = b->next;
            b->size = need;
            b->next = tail;
        }

        b->free = false;
        account_alloc(b->size);
        return (uint8_t *)b + sizeof(kblock_t);
    }
    return 0;
}

void *kmalloc(size_t bytes) {
    if (bytes == 0) return 0;

    size_t need = ALIGN_UP(bytes, 8);

    void *p = find_fit(need);
    if (p) return p;

    /* No fit — grow. Use at least KHEAP_GROW_PAGES, or whatever is
     * needed to satisfy this request plus a fresh header. */
    size_t need_bytes = need + sizeof(kblock_t);
    size_t need_pages = (need_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t pages      = (need_pages > KHEAP_GROW_PAGES) ? need_pages
                                                         : KHEAP_GROW_PAGES;
    if (!heap_grow(pages)) return 0;

    /* Retry — the grown free region must now satisfy the request. */
    return find_fit(need);
}

void kfree(void *ptr) {
    if (!ptr) return;

    kblock_t *b = (kblock_t *)((uint8_t *)ptr - sizeof(kblock_t));
    b->free = true;
    heap_used -= b->size;

    /* Coalesce with the next block if free. */
    if (b->next && b->next->free) {
        b->size += sizeof(kblock_t) + b->next->size;
        b->next  = b->next->next;
    }

    /* Coalesce with the previous block if free. O(n) walk from head. */
    if (b != heap_head) {
        kblock_t *prev = heap_head;
        while (prev && prev->next != b) prev = prev->next;
        if (prev && prev->free) {
            prev->size += sizeof(kblock_t) + b->size;
            prev->next  = b->next;
        }
    }
}

size_t kheap_total_bytes(void) { return heap_total; }
size_t kheap_used_bytes(void)  { return heap_used;  }
size_t kheap_peak_bytes(void)  { return heap_peak;  }
size_t kheap_grow_events(void) { return grow_events; }
size_t kheap_grow_oom(void)    { return grow_oom;    }
