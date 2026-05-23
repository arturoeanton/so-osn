/*
 * /bin/readelf — minimal ELF inspector for osnos. Mirrors a tiny
 * subset of GNU readelf so we can debug binaries from inside the
 * guest without dropping back to the host's `llvm-readelf` over
 * the sd.img.
 *
 *   readelf -h FILE     ELF header (type/machine/entry/...)
 *   readelf -l FILE     program headers (PT_LOAD/INTERP/DYNAMIC/...)
 *   readelf -a FILE     all of the above
 *
 * Intentionally NOT supporting: section headers (long), dynamic
 * symbol table, relocs. Add when needed.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osnos_elf.h"

static const char *etype_name(uint16_t t) {
    switch (t) {
    case ET_NONE: return "NONE (No file type)";
    case ET_REL:  return "REL (Relocatable file)";
    case ET_EXEC: return "EXEC (Executable file)";
    case ET_DYN:  return "DYN (Shared object / PIE)";
    default:      return "?";
    }
}

static const char *machine_name(uint16_t m) {
    switch (m) {
    case EM_X86_64: return "Advanced Micro Devices X86-64";
    case 0:         return "None";
    case 3:         return "Intel 80386";
    case 40:        return "ARM";
    case 183:       return "AArch64";
    default:        return "?";
    }
}

static const char *ptype_name(uint32_t t) {
    switch (t) {
    case PT_NULL:    return "NULL";
    case PT_LOAD:    return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP:  return "INTERP";
    case PT_NOTE:    return "NOTE";
    case PT_PHDR:    return "PHDR";
    case PT_TLS:     return "TLS";
    default:         return "?";
    }
}

/* Read whole file into a malloc'd buffer; caller frees. */
static char *slurp(const char *path, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "readelf: cannot open %s\n", path);
        return 0;
    }
    /* Stat-less: just grow the buffer as we read. 64 KiB initial
     * is enough for nearly everything in /bin; we double if needed. */
    size_t cap = 64 * 1024;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return 0; }
    size_t len = 0;
    for (;;) {
        if (cap - len < 4096) {
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { free(buf); close(fd); return 0; }
            buf = nb; cap = ncap;
        }
        long n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    *size_out = len;
    return buf;
}

static void print_header(const Elf64_Ehdr *eh) {
    printf("ELF Header:\n");
    printf("  Magic:                  %02x %c%c%c (%s)\n",
           eh->e_ident[0],
           eh->e_ident[1], eh->e_ident[2], eh->e_ident[3],
           (eh->e_ident[0] == ELFMAG0 && eh->e_ident[1] == 'E' &&
            eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')
               ? "valid" : "INVALID");
    printf("  Class:                  %s\n",
           eh->e_ident[EI_CLASS] == ELFCLASS64 ? "ELF64" : "ELF32/?");
    printf("  Data:                   %s\n",
           eh->e_ident[EI_DATA] == ELFDATA2LSB ? "2's complement, LSB"
                                                 : "?");
    printf("  Version:                %d\n", eh->e_ident[EI_VERSION]);
    printf("  Type:                   %s\n", etype_name(eh->e_type));
    printf("  Machine:                %s\n", machine_name(eh->e_machine));
    printf("  Entry point:            0x%lx\n", (unsigned long)eh->e_entry);
    printf("  Start of program hdrs:  %lu (bytes into file)\n",
           (unsigned long)eh->e_phoff);
    printf("  Start of section hdrs:  %lu (bytes into file)\n",
           (unsigned long)eh->e_shoff);
    printf("  Flags:                  0x%x\n", eh->e_flags);
    printf("  ELF header size:        %d\n", eh->e_ehsize);
    printf("  Program header size:    %d\n", eh->e_phentsize);
    printf("  Number of program hdrs: %d\n", eh->e_phnum);
    printf("  Section header size:    %d\n", eh->e_shentsize);
    printf("  Number of section hdrs: %d\n", eh->e_shnum);
}

static void print_phdrs(const Elf64_Ehdr *eh, const char *data,
                         size_t data_size) {
    if (eh->e_phnum == 0) {
        printf("\nNo program headers.\n");
        return;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > data_size) {
        printf("\nProgram headers truncated.\n");
        return;
    }
    printf("\nProgram Headers (%d entries):\n", eh->e_phnum);
    printf("  Type          Offset     VirtAddr           FileSiz    MemSiz     Flg\n");
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *p =
            (const Elf64_Phdr *)(data + eh->e_phoff + i * eh->e_phentsize);
        char flg[4] = "   ";
        if (p->p_flags & PF_R) flg[0] = 'R';
        if (p->p_flags & PF_W) flg[1] = 'W';
        if (p->p_flags & PF_X) flg[2] = 'E';
        printf("  %-12s  0x%08lx 0x%016lx 0x%08lx 0x%08lx %s\n",
               ptype_name(p->p_type),
               (unsigned long)p->p_offset,
               (unsigned long)p->p_vaddr,
               (unsigned long)p->p_filesz,
               (unsigned long)p->p_memsz,
               flg);
        if (p->p_type == PT_INTERP &&
            p->p_offset + p->p_filesz <= data_size) {
            printf("      Requesting interpreter: \"%.*s\"\n",
                   (int)p->p_filesz, data + p->p_offset);
        }
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: readelf -h|-l|-a FILE\n"
            "  -h   ELF header only\n"
            "  -l   program headers only\n"
            "  -a   header + program headers\n");
}

int main(int argc, char **argv) {
    int want_h = 0, want_l = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if      (strcmp(argv[i], "-h") == 0) want_h = 1;
        else if (strcmp(argv[i], "-l") == 0) want_l = 1;
        else if (strcmp(argv[i], "-a") == 0) want_h = want_l = 1;
        else { usage(); return 1; }
        i++;
    }
    if (i >= argc) { usage(); return 1; }
    if (!want_h && !want_l) want_h = want_l = 1;   /* default = -a */

    const char *path = argv[i];
    size_t size = 0;
    char *data = slurp(path, &size);
    if (!data) return 1;
    if (size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "readelf: %s: too small to be an ELF\n", path);
        free(data);
        return 1;
    }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'      || eh->e_ident[3] != 'F') {
        fprintf(stderr, "readelf: %s: not an ELF (bad magic)\n", path);
        free(data);
        return 1;
    }

    if (want_h) print_header(eh);
    if (want_l) print_phdrs(eh, data, size);

    free(data);
    return 0;
}
