#pragma once

#include <stddef.h>

#include "../include/osnos_status.h"

/*
 * Safe user/kernel memory copies. Kernel code calling syscall handlers
 * uses these to access pointers received from userland.
 *
 * Contract:
 *   - The user pointer must live entirely below OSNOS_USER_VIRT_MAX
 *     (the lower-half user range). Otherwise OSNOS_EFAULT.
 *   - If the user page is not mapped (or read-only when we try to
 *     write), the inner copy loop page-faults. The page-fault handler
 *     finds the loop in the extable and redirects RIP so the function
 *     returns OSNOS_EFAULT cleanly instead of panicking the kernel.
 *
 * uaccess_init() must be called once at boot (after extable is
 * available) to register the protected copy span.
 */
void uaccess_init(void);

osnos_status_t copy_from_user(void *dst, const void *user_src, size_t n);
osnos_status_t copy_to_user  (void *user_dst, const void *src, size_t n);
