/*
 * Linker script for ring-3 ELFs linked against vendored musl libc.
 *
 * Keeps .init_array / .fini_array / .preinit_array which musl's
 * __libc_start_init walks. Adds .tdata / .tbss as a real PT_TLS so
 * __init_tls finds TLS image via auxv (well, via the PHDR walk).
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
    tls  PT_TLS  FLAGS(4);   /* R     */
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

    .init_array : {
        __init_array_start = .;
        KEEP(*(.preinit_array))
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.*)
    } :data :tls

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.gnu.hash)
        *(.hash)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.version*)
    }
}
