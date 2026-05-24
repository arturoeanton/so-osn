#include "elf.h"

#include "../include/osnos_elf.h"
#include "../micro/pmm.h"
#include "../micro/vmm.h"

/*
 * User stack: 64 KiB (16 pages) sitting right below 0x80000000.
 * Larger ELFs (TCC, real script interpreters, etc.) blow through
 * the original 4 KiB easily. Each task gets its own private set —
 * no sharing — so the per-task cost is paid only by ring-3 tasks.
 */
#define USER_STACK_PAGES 16
#define USER_STACK_SIZE  (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_TOP   0x80000000ULL
#define USER_STACK_VIRT  (USER_STACK_TOP - USER_STACK_SIZE)

static int validate_ehdr(const Elf64_Ehdr *eh, size_t blob_size) {
    if (blob_size < sizeof(Elf64_Ehdr))             return 0;
    if (eh->e_ident[EI_MAG0]  != ELFMAG0)            return 0;
    if (eh->e_ident[EI_MAG1]  != ELFMAG1)            return 0;
    if (eh->e_ident[EI_MAG2]  != ELFMAG2)            return 0;
    if (eh->e_ident[EI_MAG3]  != ELFMAG3)            return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64)         return 0;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB)        return 0;
    if (eh->e_type            != ET_EXEC)            return 0;
    if (eh->e_machine         != EM_X86_64)          return 0;
    if (eh->e_phentsize       != sizeof(Elf64_Phdr)) return 0;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > blob_size)
                                                      return 0;
    return 1;
}

/* Validación menos estricta — acepta ET_EXEC y ET_DYN (interpreter
 * + futuro PIE). */
static int validate_ehdr_loose(const Elf64_Ehdr *eh, size_t blob_size) {
    if (blob_size < sizeof(Elf64_Ehdr))             return 0;
    if (eh->e_ident[EI_MAG0]  != ELFMAG0)            return 0;
    if (eh->e_ident[EI_MAG1]  != ELFMAG1)            return 0;
    if (eh->e_ident[EI_MAG2]  != ELFMAG2)            return 0;
    if (eh->e_ident[EI_MAG3]  != ELFMAG3)            return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64)         return 0;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB)        return 0;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return 0;
    if (eh->e_machine         != EM_X86_64)          return 0;
    if (eh->e_phentsize       != sizeof(Elf64_Phdr)) return 0;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > blob_size)
                                                      return 0;
    return 1;
}

static uint64_t flags_to_pte(uint32_t pflags) {
    /* User access is always implied; we OR PTE_W for writable segments.
     * x86_64 has no per-page X bit unless NX is enabled, which we do not
     * yet manage, so PF_X is effectively a no-op (every mapped page is
     * executable). When NX lands we will start clearing the NX bit on
     * PF_X segments and setting it on the rest. */
    uint64_t pte = PTE_U;
    if (pflags & PF_W) pte |= PTE_W;
    return pte;
}

static size_t min_size_t(size_t a, size_t b) { return a < b ? a : b; }

osnos_status_t elf_load(const uint8_t *data,
                        size_t          size,
                        uint64_t      **pml4_out,
                        uint64_t       *entry_out,
                        uint64_t       *stack_top_out)
{
    if (!data || !pml4_out || !entry_out || !stack_top_out) return OSNOS_EFAULT;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (!validate_ehdr(eh, size)) return OSNOS_EINVAL;

    uint64_t *pml4 = address_space_create();
    if (!pml4) return OSNOS_ENOMEM;

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(data + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        if (ph->p_vaddr + ph->p_memsz >= OSNOS_USER_VIRT_MAX) {
            address_space_destroy(pml4);
            return OSNOS_EINVAL;
        }
        if (ph->p_offset + ph->p_filesz > size) {
            address_space_destroy(pml4);
            return OSNOS_EINVAL;
        }

        uint64_t pte_flags = flags_to_pte(ph->p_flags);

        /*
         * Walk the segment one page at a time. Each page is allocated
         * fresh from the PMM, mapped into the user PML4, and filled
         * with the corresponding slice of the file image (the tail of
         * the segment may live entirely in p_memsz > p_filesz, which
         * stays zeroed).
         */
        uint64_t seg_start = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
                             & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                address_space_destroy(pml4);
                return OSNOS_ENOMEM;
            }
            uint8_t *page = (uint8_t *)(phys + pmm_hhdm_offset());
            for (size_t b = 0; b < PAGE_SIZE; b++) page[b] = 0;

            /* Compute the overlap between [va, va+PAGE_SIZE) and the
             * file-resident slice [p_vaddr, p_vaddr+p_filesz). */
            uint64_t page_lo = va;
            uint64_t page_hi = va + PAGE_SIZE;
            uint64_t file_lo = ph->p_vaddr;
            uint64_t file_hi = ph->p_vaddr + ph->p_filesz;

            if (file_hi > page_lo && file_lo < page_hi) {
                uint64_t copy_va_lo = page_lo > file_lo ? page_lo : file_lo;
                uint64_t copy_va_hi = page_hi < file_hi ? page_hi : file_hi;
                size_t   copy_n     = (size_t)(copy_va_hi - copy_va_lo);
                size_t   src_off    = (size_t)(ph->p_offset
                                              + (copy_va_lo - ph->p_vaddr));
                copy_n = min_size_t(copy_n, size - src_off);

                const uint8_t *src = data + src_off;
                uint8_t       *dst = page + (copy_va_lo - page_lo);
                for (size_t b = 0; b < copy_n; b++) dst[b] = src[b];
            }

            if (!vmm_map(pml4, va, phys, pte_flags)) {
                pmm_free_page(phys);
                address_space_destroy(pml4);
                return OSNOS_ENOMEM;
            }
        }
    }

    /* User stack: USER_STACK_PAGES pages below USER_STACK_TOP, RW+U.
     * Allocated page-by-page so we get distinct frames; mapped
     * contiguous virtual range so the program sees one big stack. */
    for (uint64_t va = USER_STACK_VIRT; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t stack_phys = pmm_alloc_page();
        if (!stack_phys) {
            address_space_destroy(pml4);
            return OSNOS_ENOMEM;
        }
        if (!vmm_map(pml4, va, stack_phys, PTE_W | PTE_U)) {
            pmm_free_page(stack_phys);
            address_space_destroy(pml4);
            return OSNOS_ENOMEM;
        }
    }

    *pml4_out      = pml4;
    *entry_out     = eh->e_entry;
    *stack_top_out = USER_STACK_TOP;
    return OSNOS_OK;
}

/* ============================================================== */
/* Dynamic linking: elf_get_interp + elf_load_dyn                 */
/* ============================================================== */

/* Base donde mappeamos el dynamic linker (ld-musl.so/libc.so).
 * Debe estar fuera del rango habitual del main ELF (0x400000) y de
 * la zona mmap (0x20000000+), pero antes del stack (0x80000000). */
#define INTERP_LOAD_BASE  0x40000000ULL

/* Forward decl. */
static osnos_status_t load_phdrs_into(uint64_t *pml4,
                                       const uint8_t *blob, size_t size,
                                       const Elf64_Phdr *phdrs, uint16_t phnum,
                                       uint64_t load_offset);

osnos_status_t elf_get_interp(const uint8_t *data, size_t size,
                               char *out_buf, size_t out_max)
{
    if (!data || !out_buf || out_max == 0) return OSNOS_EFAULT;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (!validate_ehdr_loose(eh, size)) return OSNOS_EINVAL;

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(data + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_INTERP) continue;
        if (phdrs[i].p_offset + phdrs[i].p_filesz > size) return OSNOS_EINVAL;
        if (phdrs[i].p_filesz >= out_max)                  return OSNOS_ENAMETOOLONG;
        const uint8_t *src = data + phdrs[i].p_offset;
        size_t n = (size_t)phdrs[i].p_filesz;
        for (size_t k = 0; k < n; k++) out_buf[k] = (char)src[k];
        out_buf[n] = 0;
        /* Strip trailing NUL if file included it. */
        while (n > 0 && out_buf[n-1] == 0) n--;
        out_buf[n] = 0;
        return OSNOS_OK;
    }
    return OSNOS_ENOENT;
}

/* Carga todos los PT_LOAD del blob en `pml4` aplicando `load_offset`
 * a cada p_vaddr. Helper común para MAIN (offset=0) y INTERP (offset=
 * INTERP_LOAD_BASE). */
static osnos_status_t load_phdrs_into(uint64_t *pml4,
                                       const uint8_t *blob, size_t size,
                                       const Elf64_Phdr *phdrs, uint16_t phnum,
                                       uint64_t load_offset)
{
    for (uint16_t i = 0; i < phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        uint64_t vaddr = ph->p_vaddr + load_offset;
        if (vaddr + ph->p_memsz >= OSNOS_USER_VIRT_MAX) return OSNOS_EINVAL;
        if (ph->p_offset + ph->p_filesz > size)         return OSNOS_EINVAL;

        uint64_t pte_flags = flags_to_pte(ph->p_flags);
        uint64_t seg_start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (vaddr + ph->p_memsz + PAGE_SIZE - 1)
                             & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            /* Si la page ya está mappeada (PT_LOADs overlapeando entre
             * MAIN y INTERP — no debería pasar con bases bien
             * separadas, pero defensive), saltar. */
            if (vmm_lookup(pml4, va)) continue;
            uint64_t phys = pmm_alloc_page();
            if (!phys) return OSNOS_ENOMEM;
            uint8_t *page = (uint8_t *)(phys + pmm_hhdm_offset());
            for (size_t b = 0; b < PAGE_SIZE; b++) page[b] = 0;

            uint64_t page_lo = va;
            uint64_t page_hi = va + PAGE_SIZE;
            uint64_t file_lo = vaddr;
            uint64_t file_hi = vaddr + ph->p_filesz;

            if (file_hi > page_lo && file_lo < page_hi) {
                uint64_t copy_va_lo = page_lo > file_lo ? page_lo : file_lo;
                uint64_t copy_va_hi = page_hi < file_hi ? page_hi : file_hi;
                size_t   copy_n     = (size_t)(copy_va_hi - copy_va_lo);
                size_t   src_off    = (size_t)(ph->p_offset
                                              + (copy_va_lo - vaddr));
                if (src_off + copy_n > size) copy_n = size - src_off;
                const uint8_t *src = blob + src_off;
                uint8_t       *dst = page + (copy_va_lo - page_lo);
                for (size_t b = 0; b < copy_n; b++) dst[b] = src[b];
            }

            if (!vmm_map(pml4, va, phys, pte_flags)) {
                pmm_free_page(phys);
                return OSNOS_ENOMEM;
            }
        }
    }
    return OSNOS_OK;
}

osnos_status_t elf_load_dyn(const uint8_t *main_blob, size_t main_size,
                             const uint8_t *interp_blob, size_t interp_size,
                             uint64_t **pml4_out,
                             elf_load_result_t *result)
{
    if (!main_blob || !pml4_out || !result) return OSNOS_EFAULT;

    const Elf64_Ehdr *meh = (const Elf64_Ehdr *)main_blob;
    if (!validate_ehdr_loose(meh, main_size)) return OSNOS_EINVAL;
    /* Por ahora MAIN debe ser ET_EXEC (PIE/ET_DYN main viene después). */
    if (meh->e_type != ET_EXEC) return OSNOS_EINVAL;

    const Elf64_Phdr *mphdrs = (const Elf64_Phdr *)(main_blob + meh->e_phoff);

    uint64_t *pml4 = address_space_create();
    if (!pml4) return OSNOS_ENOMEM;

    /* Cargar MAIN con offset 0. */
    osnos_status_t s = load_phdrs_into(pml4, main_blob, main_size,
                                        mphdrs, meh->e_phnum, 0);
    if (s != OSNOS_OK) { address_space_destroy(pml4); return s; }

    /* Cargar INTERP en INTERP_LOAD_BASE si fue provisto. */
    uint64_t interp_base  = 0;
    uint64_t interp_entry = 0;
    if (interp_blob && interp_size > 0) {
        const Elf64_Ehdr *ieh = (const Elf64_Ehdr *)interp_blob;
        if (!validate_ehdr_loose(ieh, interp_size) || ieh->e_type != ET_DYN) {
            address_space_destroy(pml4);
            return OSNOS_EINVAL;
        }
        const Elf64_Phdr *iphdrs = (const Elf64_Phdr *)(interp_blob + ieh->e_phoff);
        s = load_phdrs_into(pml4, interp_blob, interp_size,
                             iphdrs, ieh->e_phnum, INTERP_LOAD_BASE);
        if (s != OSNOS_OK) { address_space_destroy(pml4); return s; }
        interp_base  = INTERP_LOAD_BASE;
        interp_entry = INTERP_LOAD_BASE + ieh->e_entry;
    }

    /* User stack. */
    for (uint64_t va = USER_STACK_VIRT; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t stack_phys = pmm_alloc_page();
        if (!stack_phys) { address_space_destroy(pml4); return OSNOS_ENOMEM; }
        if (!vmm_map(pml4, va, stack_phys, PTE_W | PTE_U)) {
            pmm_free_page(stack_phys);
            address_space_destroy(pml4);
            return OSNOS_ENOMEM;
        }
    }

    /* Buscar dónde quedó el program-header del MAIN en USER memory.
     * Linux convention: si hay PT_PHDR, su p_vaddr es la respuesta.
     * Sino, los phdrs están al comienzo del primer PT_LOAD (p_vaddr
     * + e_phoff - p_offset). */
    uint64_t phdr_user_va = 0;
    for (uint16_t i = 0; i < meh->e_phnum; i++) {
        if (mphdrs[i].p_type == PT_PHDR) {
            phdr_user_va = mphdrs[i].p_vaddr;
            break;
        }
    }
    if (phdr_user_va == 0) {
        for (uint16_t i = 0; i < meh->e_phnum; i++) {
            if (mphdrs[i].p_type == PT_LOAD &&
                meh->e_phoff >= mphdrs[i].p_offset &&
                meh->e_phoff <  mphdrs[i].p_offset + mphdrs[i].p_filesz)
            {
                phdr_user_va = mphdrs[i].p_vaddr +
                                (meh->e_phoff - mphdrs[i].p_offset);
                break;
            }
        }
    }

    *pml4_out         = pml4;
    result->entry        = meh->e_entry;
    result->start_entry  = interp_entry ? interp_entry : meh->e_entry;
    result->stack_top    = USER_STACK_TOP;
    result->phdr_user_va = phdr_user_va;
    result->phnum        = meh->e_phnum;
    result->phentsize    = meh->e_phentsize;
    result->interp_base  = interp_base;
    return OSNOS_OK;
}
