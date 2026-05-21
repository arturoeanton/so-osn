#pragma once

#include <stddef.h>

/*
 * Kernel heap.
 *
 * Single-linked free list with first-fit allocation, coalescing on free.
 * Initial mapping is 16 pages (64 KiB) at KHEAP_VIRT_BASE; on a failed
 * find-fit kmalloc maps more pages and appends them to the free list
 * (FASE A — kheap growth). Hard cap at KHEAP_MAX_BYTES so a leak
 * doesn't drain the physical pool. OOM (cap reached or PMM empty)
 * returns NULL.
 *
 *   void *p = kmalloc(128);
 *   ... use p ...
 *   kfree(p);
 *
 * 8-byte alignment of returned pointers. Free-on-NULL is a no-op (libc
 * convention).
 */
void   kheap_init(void);
void  *kmalloc(size_t bytes);
void   kfree(void *ptr);

size_t kheap_total_bytes(void);  /* current mapped first-fit heap size */
size_t kheap_used_bytes(void);   /* live allocation bytes (first-fit + slab) */
size_t kheap_peak_bytes(void);   /* max used bytes ever observed */
size_t kheap_grow_events(void);  /* number of successful grow extensions */
size_t kheap_grow_oom(void);     /* grow attempts that failed (cap or PMM) */

/* Slab subsystem (FASE B) introspection. */
size_t kheap_slab_used_bytes (void);
size_t kheap_slab_pages      (void);
size_t kheap_slab_grow_events(void);
size_t kheap_slab_grow_oom   (void);
size_t kheap_slab_slots_used (int bucket_idx);
size_t kheap_slab_slots_total(int bucket_idx);
size_t kheap_slab_bucket_size(int bucket_idx);
int    kheap_slab_bucket_count(void);
