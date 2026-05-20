#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Exception fixup table.
 *
 * Kernel code that touches user memory (copy_*_user, future iret-to-user
 * stubs) registers the address range that may fault. When the page fault
 * handler sees RIP inside a registered span, it rewrites the iret frame's
 * RIP to the recovery address so the function "returns" with EFAULT
 * instead of panicking the kernel.
 *
 * The table is small and global; lookup is linear. We expect a handful of
 * spans (one per uaccess helper, eventually one per syscall return path).
 */

#define EXTABLE_MAX_ENTRIES 16

bool extable_register(uintptr_t rip_start,
                      uintptr_t rip_end,
                      uintptr_t recovery_rip);

/*
 * If `rip` falls within any registered span [rip_start, rip_end),
 * stores the recovery RIP in *recovery_out and returns true.
 * Otherwise returns false.
 */
bool extable_lookup(uintptr_t rip, uintptr_t *recovery_out);
