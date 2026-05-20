#include "elf.h"

#include "../include/osnos_elf.h"
#include "../micro/pmm.h"
#include "../micro/vmm.h"

#define USER_STACK_VIRT  0x7FFFE000ULL   /* 1 page below 0x80000000 */
#define USER_STACK_TOP   0x7FFFF000ULL   /* one page = 4 KiB */

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

    /* User stack: one page right below USER_STACK_TOP, RW + user. */
    uint64_t stack_phys = pmm_alloc_page();
    if (!stack_phys) {
        address_space_destroy(pml4);
        return OSNOS_ENOMEM;
    }
    if (!vmm_map(pml4, USER_STACK_VIRT, stack_phys, PTE_W | PTE_U)) {
        pmm_free_page(stack_phys);
        address_space_destroy(pml4);
        return OSNOS_ENOMEM;
    }

    *pml4_out      = pml4;
    *entry_out     = eh->e_entry;
    *stack_top_out = USER_STACK_TOP;
    return OSNOS_OK;
}
