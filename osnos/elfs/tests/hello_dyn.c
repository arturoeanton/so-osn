/*
 * hello_dyn — primer test de dynamic linking en osnos.
 *
 * Linkeado dynamic-musl (no -static): PT_INTERP apunta a
 * /lib/ld-musl-x86_64.so.1, ld-musl.so resuelve printf, exit, etc.
 * desde libc.so en /lib.
 *
 * Si el output aparece end-to-end significa que:
 *   - kernel cargó el interpreter (PT_INTERP path resuelto)
 *   - kernel armó auxv completo (AT_PHDR/PHNUM/BASE/ENTRY/RANDOM)
 *   - ld-musl.so parseó el main ELF + las relocations
 *   - el binary jumps a main() correctamente
 *   - printf resolvió contra libc.so dynamic + escribió a stdout
 */
#include <stdio.h>

int main(int argc, char **argv) {
    printf("hello from dynamic linker on osnos!\n");
    printf("argc=%d argv[0]='%s'\n", argc, argv[0] ? argv[0] : "(null)");
    return 0;
}
