#pragma once

#include <stdint.h>

/*
 * Linux-style endian.h — host ↔ big-endian and host ↔ little-endian
 * conversion helpers in fixed widths. osnos targets x86_64 only
 * today, so "host" is always little-endian and the htole/letoh
 * helpers are identity casts. The htobe/betoh helpers swap bytes.
 *
 * Byte order constants follow the BSD/glibc convention so code that
 * checks `__BYTE_ORDER == __LITTLE_ENDIAN` keeps compiling.
 */

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#define __BYTE_ORDER    __LITTLE_ENDIAN

/* glibc-compatible aliases (no leading underscores). */
#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define PDP_ENDIAN      __PDP_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER

/* Compiler builtins for the actual swaps — clang/gcc support all. */
#define __osnos_bswap16(x) __builtin_bswap16((uint16_t)(x))
#define __osnos_bswap32(x) __builtin_bswap32((uint32_t)(x))
#define __osnos_bswap64(x) __builtin_bswap64((uint64_t)(x))

/* host ↔ big-endian */
#define htobe16(x) __osnos_bswap16(x)
#define htobe32(x) __osnos_bswap32(x)
#define htobe64(x) __osnos_bswap64(x)
#define be16toh(x) __osnos_bswap16(x)
#define be32toh(x) __osnos_bswap32(x)
#define be64toh(x) __osnos_bswap64(x)

/* host ↔ little-endian (identity on x86_64) */
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
