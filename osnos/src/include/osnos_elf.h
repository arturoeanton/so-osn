#pragma once

#include <stdint.h>

/*
 * Minimal ELF64 layout subset — only what elf_load() needs to map a
 * fixed-address (ET_EXEC) x86_64 binary into a fresh address space.
 *
 * Field names and numeric constants match the System V ABI document
 * (and therefore Linux's <elf.h>) so that future newlib / cross-tool
 * work doesn't need translation.
 */

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/* e_ident indices */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS64  2
#define ELFDATA2LSB 1

/* e_type */
#define ET_NONE     0
#define ET_REL      1
#define ET_EXEC     2
#define ET_DYN      3

/* e_machine */
#define EM_X86_64   62

/* p_type */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6
#define PT_TLS      7

/* p_flags */
#define PF_X        (1u << 0)
#define PF_W        (1u << 1)
#define PF_R        (1u << 2)

/* Section headers — needed for `readelf -S` only; not used by the
 * ELF loader (which walks PT_LOAD segments instead). */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) Elf64_Shdr;

/* sh_type — most-used subset. */
#define SHT_NULL          0
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_DYNSYM       11
#define SHT_INIT_ARRAY   14
#define SHT_FINI_ARRAY   15

/* sh_flags */
#define SHF_WRITE        (1u << 0)
#define SHF_ALLOC        (1u << 1)
#define SHF_EXECINSTR    (1u << 2)
