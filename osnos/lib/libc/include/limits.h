#pragma once

/*
 * Integer and path limits. Values match Linux x86_64 (LP64). Used
 * by every nontrivial C program plus TCC for its internal type
 * tables.
 */

#define CHAR_BIT     8

#define SCHAR_MAX    127
#define SCHAR_MIN    (-128)
#define UCHAR_MAX    255

/* char is signed on x86_64 SysV. */
#define CHAR_MIN     SCHAR_MIN
#define CHAR_MAX     SCHAR_MAX

#define SHRT_MAX     32767
#define SHRT_MIN     (-32768)
#define USHRT_MAX    65535

#define INT_MAX      2147483647
#define INT_MIN      (-INT_MAX - 1)
#define UINT_MAX     4294967295U

#define LONG_MAX     9223372036854775807L
#define LONG_MIN     (-LONG_MAX - 1L)
#define ULONG_MAX    18446744073709551615UL

#define LLONG_MAX    9223372036854775807LL
#define LLONG_MIN    (-LLONG_MAX - 1LL)
#define ULLONG_MAX   18446744073709551615ULL

#define MB_LEN_MAX   4   /* widest UTF-8 sequence */

/* POSIX path/name caps. Mirrored from src/include/osnos_limits.h on
 * the kernel side. */
#define PATH_MAX     128
#define NAME_MAX     63
#define ARG_MAX      2048
#define IOV_MAX      16
#define OPEN_MAX     32
