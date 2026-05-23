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

/*
 * Copy a NUL-terminated string from user space into `dst`, up to
 * `maxlen` bytes (including the NUL). Stops as soon as a NUL byte
 * is encountered, so unmapped memory past the end of the string
 * does NOT fault — unlike a fixed-size `copy_from_user(.., maxlen)`
 * which reads the full window even if the string is short.
 *
 * Returns:
 *   OSNOS_OK     — NUL found within maxlen, `dst` holds the string.
 *                  On truncation (no NUL within maxlen), `dst` is
 *                  forcibly NUL-terminated at dst[maxlen-1] and we
 *                  still return OK (POSIX-ish — let caller detect
 *                  truncation if needed).
 *   OSNOS_EFAULT — page fault before NUL was reached. Partial data
 *                  may have been written to `dst`.
 *
 * Used by sys_execve / sys_writev / any syscall that copies
 * variable-length user strings near a page boundary.
 */
osnos_status_t copy_string_from_user(char *dst, const char *user_src,
                                      size_t maxlen);
