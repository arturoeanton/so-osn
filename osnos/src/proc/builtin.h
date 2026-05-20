#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Registry of /bin entries.
 *
 * Two flavors after the FASE 7 migration:
 *
 *   Flat user blob — `user_start` non-NULL
 *     A blob of x86_64 machine code (file-scope inline asm). The
 *     kernel maps the bytes verbatim into a single user page at
 *     USER_CODE_VIRT (0x400000) and starts the task there. Used to
 *     verify the ring-3 + syscall path without involving libc / ELF.
 *
 *   User ELF — `elf_start` non-NULL
 *     A linked ELF64 ET_EXEC blob embedded in the kernel. The ELF
 *     loader parses program headers, maps every PT_LOAD at its
 *     declared p_vaddr, then runs from e_entry. Every "real" tool
 *     (cat/ls/cp/etc.) ships this way, linked against lib/libc.
 *
 * Both arrive at the same `syscall_dispatch` for every syscall.
 */
typedef struct {
    const char     *name;        /* e.g. "cat"  -> exec("/bin/cat") */
    const uint8_t  *user_start;  /* set for flat user blobs */
    const uint8_t  *user_end;
    const uint8_t  *elf_start;   /* set for user ELF blobs */
    const uint8_t  *elf_end;
    const char     *desc;        /* one-line description (cat /bin/foo) */
} builtin_t;

const builtin_t *builtin_find(const char *name);
size_t           builtin_count(void);
const builtin_t *builtin_at(size_t idx);
