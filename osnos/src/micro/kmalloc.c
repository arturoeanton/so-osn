#include "kmalloc.h"

#include <stdbool.h>
#include <stdint.h>

#include "pmm.h"
#include "vmm.h"

/*
 * Kernel heap.
 *
 * Two allocator paths share a single kmalloc / kfree front door:
 *
 *   - Slab (FASE B): 8 power-of-2 buckets (16, 32, 64, 128, 256, 512,
 *     1024, 2048). Each bucket carves slabs (one 4 KiB page each) into
 *     equal-sized slots; allocations are O(1) LIFO. Slabs live in a
 *     dedicated virtual region (SLAB_VIRT_BASE) so kfree can detect
 *     ownership with a single range check.
 *   - First-fit free list (FASE A): everything > 2048 bytes plus
 *     fallback when slab can't get a new page. Grows in 64 KiB chunks
 *     up to KHEAP_MAX_BYTES.
 *
 * Tracking:
 *   - heap_used = first-fit payload in use.
 *   - slab_used = bytes in live slab slots (sized by bucket).
 *   - kheap_used_bytes() reports the sum.
 *   - heap_peak tracks the high-water of the sum.
 */

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* ============================================================ */
/* First-fit allocator (FASE A: growable)                       */
/* ============================================================ */

typedef struct kblock {
    size_t        size;     /* payload bytes */
    bool          free;
    struct kblock *next;
} kblock_t;

#define KHEAP_VIRT_BASE   0xffffc00000000000ULL
#define KHEAP_INIT_PAGES  16
#define KHEAP_GROW_PAGES  16
#define KHEAP_MAX_BYTES   (32ULL * 1024 * 1024)  /* 32 MiB hard cap — sqlite ELF es 5 MB y proc_execve aloca el blob entero acá; bumpado de 4 MB en FASE 13.3 (sqlite port) */

static kblock_t *heap_head;
static size_t    heap_total;
static size_t    heap_used;      /* first-fit live payload */
static size_t    slab_used;      /* slab live slot bytes */
static size_t    heap_peak;      /* peak of (heap_used + slab_used) */
static size_t    grow_events;
static size_t    grow_oom;

static void account_alloc(size_t n, size_t *bucket) {
    *bucket += n;
    size_t total = heap_used + slab_used;
    if (total > heap_peak) heap_peak = total;
}

void kheap_init(void) {
    uint64_t *pml4 = vmm_kernel_pml4();

    for (size_t i = 0; i < KHEAP_INIT_PAGES; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return;
        vmm_map(pml4, KHEAP_VIRT_BASE + i * PAGE_SIZE, phys, PTE_W);
    }

    heap_total  = KHEAP_INIT_PAGES * PAGE_SIZE;
    heap_used   = 0;
    slab_used   = 0;
    heap_peak   = 0;
    grow_events = 0;
    grow_oom    = 0;

    heap_head        = (kblock_t *)KHEAP_VIRT_BASE;
    heap_head->size  = heap_total - sizeof(kblock_t);
    heap_head->free  = true;
    heap_head->next  = 0;
}

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
            for (size_t j = 0; j < i; j++) {
                vmm_unmap(pml4, start_va + j * PAGE_SIZE);
            }
            grow_oom++;
            return 0;
        }
        vmm_map(pml4, start_va + i * PAGE_SIZE, phys, PTE_W);
    }

    size_t grew_bytes = pages * PAGE_SIZE;

    kblock_t *nb = (kblock_t *)start_va;
    nb->size = grew_bytes - sizeof(kblock_t);
    nb->free = true;
    nb->next = 0;

    kblock_t *tail = heap_head;
    while (tail->next) tail = tail->next;

    if (tail->free) {
        tail->size += sizeof(kblock_t) + nb->size;
    } else {
        tail->next = nb;
    }

    heap_total  += grew_bytes;
    grow_events += 1;
    return nb;
}

static void *first_fit_alloc(size_t need) {
    for (kblock_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < need) continue;

        if (b->size >= need + sizeof(kblock_t) + 16) {
            kblock_t *tail = (kblock_t *)((uint8_t *)b + sizeof(kblock_t) + need);
            tail->size = b->size - need - sizeof(kblock_t);
            tail->free = true;
            tail->next = b->next;
            b->size = need;
            b->next = tail;
        }

        b->free = false;
        account_alloc(b->size, &heap_used);
        return (uint8_t *)b + sizeof(kblock_t);
    }
    return 0;
}

static void first_fit_free(void *ptr) {
    kblock_t *b = (kblock_t *)((uint8_t *)ptr - sizeof(kblock_t));
    b->free = true;
    heap_used -= b->size;

    if (b->next && b->next->free) {
        b->size += sizeof(kblock_t) + b->next->size;
        b->next  = b->next->next;
    }

    if (b != heap_head) {
        kblock_t *prev = heap_head;
        while (prev && prev->next != b) prev = prev->next;
        if (prev && prev->free) {
            prev->size += sizeof(kblock_t) + b->size;
            prev->next  = b->next;
        }
    }
}

/* ============================================================ */
/* Slab allocator (FASE B)                                      */
/* ============================================================ */

#define SLAB_VIRT_BASE   0xffffc00100000000ULL
#define SLAB_MAX_BYTES   (4ULL * 1024 * 1024)
#define SLAB_BUCKETS     8
#define SLAB_MAGIC       0x534F534C41423142ULL   /* "SOSLAB1B" */

static const size_t bucket_sizes[SLAB_BUCKETS] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

typedef struct slot_node {
    struct slot_node *next;
} slot_node_t;

typedef struct {
    slot_node_t *free_head;
    size_t       slots_total;
    size_t       slots_used;
    size_t       pages;
    uint64_t     alloc_total;
    uint64_t     free_total;
} bucket_t;

typedef struct {
    uint64_t magic;
    uint32_t bucket_idx;
    uint32_t _pad;
} slab_hdr_t;

static bucket_t buckets[SLAB_BUCKETS];
static uint64_t slab_next_va = SLAB_VIRT_BASE;
static size_t   slab_pages_total;
static size_t   slab_grow_events;
static size_t   slab_grow_oom;

static int size_to_bucket(size_t bytes) {
    for (int i = 0; i < SLAB_BUCKETS; i++) {
        if (bytes <= bucket_sizes[i]) return i;
    }
    return -1;
}

/* Map one new page at the next slab VA and slice it into slots for
 * `bidx`. Returns false on cap-reached / PMM-empty. */
static bool slab_grow(int bidx) {
    if (slab_next_va + PAGE_SIZE > SLAB_VIRT_BASE + SLAB_MAX_BYTES) {
        slab_grow_oom++;
        return false;
    }
    uint64_t phys = pmm_alloc_page();
    if (!phys) { slab_grow_oom++; return false; }
    vmm_map(vmm_kernel_pml4(), slab_next_va, phys, PTE_W);

    slab_hdr_t *hdr = (slab_hdr_t *)slab_next_va;
    hdr->magic      = SLAB_MAGIC;
    hdr->bucket_idx = (uint32_t)bidx;

    bucket_t *b     = &buckets[bidx];
    size_t   size   = bucket_sizes[bidx];
    size_t   start  = ALIGN_UP(sizeof(slab_hdr_t), 16);
    size_t   n      = (PAGE_SIZE - start) / size;

    /* Thread the new slots into the bucket's free list (LIFO push). */
    for (size_t i = 0; i < n; i++) {
        slot_node_t *node =
            (slot_node_t *)(slab_next_va + start + i * size);
        node->next = b->free_head;
        b->free_head = node;
    }

    b->slots_total   += n;
    b->pages         += 1;
    slab_pages_total += 1;
    slab_grow_events += 1;
    slab_next_va     += PAGE_SIZE;
    return true;
}

static void *slab_alloc(size_t bytes) {
    int bidx = size_to_bucket(bytes);
    if (bidx < 0) return 0;

    bucket_t *b = &buckets[bidx];
    if (!b->free_head) {
        if (!slab_grow(bidx)) return 0;
    }

    slot_node_t *node = b->free_head;
    b->free_head      = node->next;
    b->slots_used    += 1;
    b->alloc_total   += 1;
    account_alloc(bucket_sizes[bidx], &slab_used);
    return (void *)node;
}

static bool slab_owns(const void *ptr) {
    uint64_t p = (uint64_t)ptr;
    return p >= SLAB_VIRT_BASE && p < slab_next_va;
}

static void slab_free(void *ptr) {
    uint64_t page = (uint64_t)ptr & ~((uint64_t)PAGE_SIZE - 1);
    slab_hdr_t *hdr = (slab_hdr_t *)page;
    /* Defense in depth: bad magic = silently drop. The page-aligned
     * check + range check make false positives extremely unlikely. */
    if (hdr->magic != SLAB_MAGIC) return;
    if (hdr->bucket_idx >= SLAB_BUCKETS) return;

    bucket_t *b = &buckets[hdr->bucket_idx];
    slot_node_t *node = (slot_node_t *)ptr;
    node->next   = b->free_head;
    b->free_head = node;
    b->slots_used -= 1;
    b->free_total += 1;
    slab_used     -= bucket_sizes[hdr->bucket_idx];
}

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

void *kmalloc(size_t bytes) {
    if (bytes == 0) return 0;

    /* Small allocations go through slab. If slab is wedged (cap or
     * PMM dry), fall back to first-fit which has its own grow path. */
    if (bytes <= bucket_sizes[SLAB_BUCKETS - 1]) {
        void *p = slab_alloc(bytes);
        if (p) return p;
    }

    size_t need = ALIGN_UP(bytes, 8);
    void *p = first_fit_alloc(need);
    if (p) return p;

    /* Out of free blocks — try to grow. */
    size_t need_bytes = need + sizeof(kblock_t);
    size_t need_pages = (need_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t pages = (need_pages > KHEAP_GROW_PAGES) ? need_pages
                                                    : KHEAP_GROW_PAGES;
    if (!heap_grow(pages)) return 0;
    return first_fit_alloc(need);
}

void kfree(void *ptr) {
    if (!ptr) return;
    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }
    first_fit_free(ptr);
}

size_t kheap_total_bytes(void) { return heap_total; }
size_t kheap_used_bytes(void)  { return heap_used + slab_used; }
size_t kheap_peak_bytes(void)  { return heap_peak;  }
size_t kheap_grow_events(void) { return grow_events; }
size_t kheap_grow_oom(void)    { return grow_oom;    }

/* Slab introspection (FASE B). */
size_t kheap_slab_used_bytes (void) { return slab_used; }
size_t kheap_slab_pages      (void) { return slab_pages_total; }
size_t kheap_slab_grow_events(void) { return slab_grow_events; }
size_t kheap_slab_grow_oom   (void) { return slab_grow_oom; }
size_t kheap_slab_slots_used (int bucket_idx) {
    if (bucket_idx < 0 || bucket_idx >= SLAB_BUCKETS) return 0;
    return buckets[bucket_idx].slots_used;
}
size_t kheap_slab_slots_total(int bucket_idx) {
    if (bucket_idx < 0 || bucket_idx >= SLAB_BUCKETS) return 0;
    return buckets[bucket_idx].slots_total;
}
size_t kheap_slab_bucket_size(int bucket_idx) {
    if (bucket_idx < 0 || bucket_idx >= SLAB_BUCKETS) return 0;
    return bucket_sizes[bucket_idx];
}
int    kheap_slab_bucket_count(void) { return SLAB_BUCKETS; }
