#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_status.h"

/*
 * Parse an in-memory ELF64 ET_EXEC blob and lay it out in a fresh
 * address space.
 *
 *   data, size   : pointer to the blob (kernel image) and its length
 *   pml4_out     : on success receives the new PML4 (HHDM pointer)
 *   entry_out    : on success receives the binary's e_entry
 *   stack_top_out: on success receives the address one byte past the
 *                  user stack top (i.e. the value to put in user RSP)
 *
 * On failure all partial allocations are released and an osnos_status_t
 * is returned. Recognised failures:
 *
 *   OSNOS_EINVAL  — bad ELF magic / class / endian / machine / type
 *   OSNOS_ENOMEM  — out of physical pages or PML4 slots
 *
 * Constraints (kept small until newlib lands):
 *   - PT_LOAD segments must lie in the lower half (below USER_VIRT_MAX)
 *   - p_align is checked but ignored beyond requiring it to divide
 *     PAGE_SIZE evenly (or be zero)
 *   - Only PT_LOAD is consumed; PT_TLS / PT_DYNAMIC are not supported
 *     yet (the loader will return EINVAL if e_type is ET_DYN)
 *   - A single user-writable stack page is allocated at a fixed
 *     virtual address right above the highest PT_LOAD region
 */
osnos_status_t elf_load(const uint8_t *data,
                        size_t          size,
                        uint64_t      **pml4_out,
                        uint64_t       *entry_out,
                        uint64_t       *stack_top_out);
