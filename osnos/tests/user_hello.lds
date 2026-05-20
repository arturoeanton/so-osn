/*
 * Linker script for a flat ring-3 ELF64 ET_EXEC.
 *
 * One PT_LOAD covering text + rodata at the canonical user code base
 * 0x400000. Section layout is unimportant — we only care that the
 * resulting ELF has a single segment starting at e_entry = _start.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
}

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
    } :text

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
