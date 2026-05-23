#pragma once

#include <stddef.h>

/*
 * Minimal locale.h — osnos is single-locale (C / POSIX). Programs
 * that include this for things like LC_NUMERIC (Lua, TCC, etc.)
 * find the constants here; setlocale() just returns "C" for any
 * non-null name and rejects writes via NULL return.
 *
 * If we ever grow real i18n we revisit; until then it's
 * intentionally a stub that compiles cleanly.
 */

#define LC_ALL       0
#define LC_COLLATE   1
#define LC_CTYPE     2
#define LC_MONETARY  3
#define LC_NUMERIC   4
#define LC_TIME      5

struct lconv {
    char *decimal_point;    /* "." */
    char *thousands_sep;    /* ""  */
    char *grouping;         /* ""  */
    /* Currency / int-numeric fields omitted — Lua doesn't touch them. */
};

char         *setlocale(int category, const char *locale);
struct lconv *localeconv(void);
