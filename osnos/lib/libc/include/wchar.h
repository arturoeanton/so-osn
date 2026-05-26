#pragma once

/*
 * Minimal <wchar.h> — only enough for libtomcrypt's DER UTF-8 encoder
 * to compile. We don't actually use wide-character I/O, so the standard
 * library of wide functions (wprintf, wcslen, etc.) is intentionally
 * absent. tlse/libtomcrypt just needs wchar_t + WCHAR_MAX visible.
 */

#include <stddef.h>     /* wchar_t comes from the freestanding stddef */
#include <stdint.h>

#ifndef WCHAR_MAX
#define WCHAR_MAX  0x7FFFFFFF
#endif
#ifndef WCHAR_MIN
#define WCHAR_MIN  (-WCHAR_MAX - 1)
#endif

typedef long wint_t;

#ifndef WEOF
#define WEOF ((wint_t)(-1))
#endif
