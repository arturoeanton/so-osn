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

/*
 * Extended ELF loader con soporte dynamic linking (PT_INTERP).
 *
 * Si `interp_blob` != NULL, lo carga TAMBIÉN como ET_DYN en una base
 * fija (INTERP_LOAD_BASE), y reporta los offsets necesarios para
 * que el caller arme el auxv que el dynamic linker (`ld-musl.so`)
 * espera leer al arrancar.
 *
 * Output `result` campos:
 *   entry          — e_entry del MAIN ELF (sin offset). Si hay interp,
 *                    el AT_ENTRY va a este valor, pero la entry inicial
 *                    real (donde la CPU comienza) es `start_entry`.
 *   start_entry    — donde la CPU arranca: interp's e_entry+interp_base
 *                    si hay PT_INTERP, sino == entry.
 *   stack_top      — top del user stack (valor de RSP inicial; antes
 *                    de build_argv_block lo decrementa).
 *   phdr_user_va   — dirección USER de la program header table del
 *                    MAIN (para AT_PHDR). Calculada como
 *                    PT_LOAD_base + e_phoff_relative_to_load_segment.
 *   phnum/phentsize — del MAIN.
 *   interp_base    — load base del interpreter (== INTERP_LOAD_BASE),
 *                    0 si no hay PT_INTERP. Para AT_BASE.
 */
typedef struct {
    uint64_t entry;
    uint64_t start_entry;
    uint64_t stack_top;
    uint64_t phdr_user_va;
    uint16_t phnum;
    uint16_t phentsize;
    uint64_t interp_base;
} elf_load_result_t;

osnos_status_t elf_load_dyn(const uint8_t *main_blob, size_t main_size,
                             const uint8_t *interp_blob, size_t interp_size,
                             uint64_t **pml4_out,
                             elf_load_result_t *result);

/*
 * Inspecciona un blob ELF para extraer el path del PT_INTERP (si lo
 * tiene). Retorna OSNOS_OK + escribe path en `out_buf` (NUL-terminated)
 * si encuentra PT_INTERP; OSNOS_ENOENT si no hay. No carga nada,
 * solo parsea headers.
 */
osnos_status_t elf_get_interp(const uint8_t *data, size_t size,
                               char *out_buf, size_t out_max);
