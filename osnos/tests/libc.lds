/*
 * Shared linker script for every libc-linked ring-3 ELF.
 *
 * .text/.rodata go in the R+X segment at 0x400000; .data/.bss go in
 * an R+W segment one page above so the kernel can mark them writable
 * without making the code segment writable.
 *
 * The Makefile rule places crt0.S.o first in the link line so
 * _start = 0x400000 (its first byte). main and the rest of the user
 * program land after.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
