#pragma once

#include <stddef.h>

/*
 * Kernel heap.
 *
 * Single-linked free list with first-fit allocation, coalescing on free.
 * Built on PMM + VMM: initial heap is 16 pages (64 KiB) mapped at
 * KHEAP_VIRT_BASE. Growth is TODO — for our current sizes this is
 * plenty. OOM returns NULL.
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

size_t kheap_total_bytes(void);
size_t kheap_used_bytes(void);
