/*
 * ar.h — Unix archive (.a) file header layout. pdpmake usa esto para
 * targets tipo `lib.a(obj.o)` (Makefile rule sobre miembro de archivo).
 * En osnos no es usado en práctica — proveemos la struct para que
 * compile, pero ningún Makefile real depende de esto.
 */
#pragma once
#include <sys/types.h>

#define ARMAG    "!<arch>\n"
#define SARMAG   8

#define ARFMAG   "`\n"

struct ar_hdr {
    char ar_name[16];   /* file member name */
    char ar_date[12];   /* modification time */
    char ar_uid[6];     /* owner uid (decimal) */
    char ar_gid[6];     /* owner gid (decimal) */
    char ar_mode[8];    /* file mode (octal) */
    char ar_size[10];   /* file size (decimal) */
    char ar_fmag[2];    /* should be ARFMAG */
};
