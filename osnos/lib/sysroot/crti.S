/*
 * crti.o stub for osnos. The classic glibc crti.o opens `_init`/`_fini`
 * function prologues that crtn.o then closes. osnos's crt0 doesn't run
 * .init/.fini sections (it calls main() directly + exit on return), so
 * these sections stay empty here. TCC's default link recipe still wants
 * to find crti.o + crtn.o on disk though — this is a placeholder so it
 * doesn't fail with "file not found".
 */

.section .init
.section .fini
