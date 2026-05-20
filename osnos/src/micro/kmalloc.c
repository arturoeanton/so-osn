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
 */

typedef struct kblock {
    size_t        size;   /* payload bytes */
    bool          free;
    struct kblock *next;
} kblock_t;

#define KHEAP_VIRT_BASE   0xffffc00000000000ULL
#define KHEAP_INIT_PAGES  16

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static kblock_t *heap_head;
static size_t    heap_total;
static size_t    heap_used;  /* bytes of payload currently allocated */

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

    heap_total = KHEAP_INIT_PAGES * PAGE_SIZE;
    heap_used  = 0;

    heap_head        = (kblock_t *)KHEAP_VIRT_BASE;
    heap_head->size  = heap_total - sizeof(kblock_t);
    heap_head->free  = true;
    heap_head->next  = 0;
}

void *kmalloc(size_t bytes) {
    if (bytes == 0) return 0;

    size_t need = ALIGN_UP(bytes, 8);

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
        heap_used += b->size;
        return (uint8_t *)b + sizeof(kblock_t);
    }

    return 0;  /* OOM — growth is TODO */
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
