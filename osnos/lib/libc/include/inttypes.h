#pragma once

#include <stdint.h>

/*
 * inttypes.h — printf/scanf format macros for the fixed-width types
 * in stdint.h. The macro is a string literal that goes inside a
 * format string: printf("v = %" PRIu64 "\n", x).
 *
 * On osnos (x86_64), int32_t is "int", int64_t is "long", uintptr_t
 * is "unsigned long", and ptrdiff_t is "long". The macros below
 * reflect that — same as glibc x86_64.
 */

#define __PRI_8     "hh"
#define __PRI_16    "h"
#define __PRI_32    ""
#define __PRI_64    "l"
#define __PRI_PTR   "l"
#define __PRI_MAX   "ll"

/* Decimal */
#define PRId8       __PRI_8  "d"
#define PRId16      __PRI_16 "d"
#define PRId32      __PRI_32 "d"
#define PRId64      __PRI_64 "d"
#define PRIdMAX     __PRI_MAX "d"
#define PRIdPTR     __PRI_PTR "d"

#define PRIi8       __PRI_8  "i"
#define PRIi16      __PRI_16 "i"
#define PRIi32      __PRI_32 "i"
#define PRIi64      __PRI_64 "i"
#define PRIiMAX     __PRI_MAX "i"
#define PRIiPTR     __PRI_PTR "i"

#define PRIu8       __PRI_8  "u"
#define PRIu16      __PRI_16 "u"
#define PRIu32      __PRI_32 "u"
#define PRIu64      __PRI_64 "u"
#define PRIuMAX     __PRI_MAX "u"
#define PRIuPTR     __PRI_PTR "u"

#define PRIo8       __PRI_8  "o"
#define PRIo16      __PRI_16 "o"
#define PRIo32      __PRI_32 "o"
#define PRIo64      __PRI_64 "o"
#define PRIoMAX     __PRI_MAX "o"
#define PRIoPTR     __PRI_PTR "o"

#define PRIx8       __PRI_8  "x"
#define PRIx16      __PRI_16 "x"
#define PRIx32      __PRI_32 "x"
#define PRIx64      __PRI_64 "x"
#define PRIxMAX     __PRI_MAX "x"
#define PRIxPTR     __PRI_PTR "x"

#define PRIX8       __PRI_8  "X"
#define PRIX16      __PRI_16 "X"
#define PRIX32      __PRI_32 "X"
#define PRIX64      __PRI_64 "X"
#define PRIXMAX     __PRI_MAX "X"
#define PRIXPTR     __PRI_PTR "X"

/* scanf — same scheme. */
#define SCNd8       __PRI_8  "d"
#define SCNd16      __PRI_16 "d"
#define SCNd32      __PRI_32 "d"
#define SCNd64      __PRI_64 "d"
#define SCNdMAX     __PRI_MAX "d"
#define SCNdPTR     __PRI_PTR "d"

#define SCNu8       __PRI_8  "u"
#define SCNu16      __PRI_16 "u"
#define SCNu32      __PRI_32 "u"
#define SCNu64      __PRI_64 "u"
#define SCNuMAX     __PRI_MAX "u"
#define SCNuPTR     __PRI_PTR "u"

#define SCNx8       __PRI_8  "x"
#define SCNx16      __PRI_16 "x"
#define SCNx32      __PRI_32 "x"
#define SCNx64      __PRI_64 "x"
#define SCNxMAX     __PRI_MAX "x"
#define SCNxPTR     __PRI_PTR "x"
