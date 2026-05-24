/*
 * glob.h — stub mínimo. osnos no implementa glob() de verdad; el shell
 * (BusyBox ash) ya expande patrones antes de invocar a las apps, así
 * que la mayoría de los callers no lo necesitan en la práctica. pdpmake
 * lo usa para `-include foo*.mk`; le devolvemos GLOB_NOMATCH y listo.
 */
#pragma once
#include <stddef.h>

#define GLOB_NOSORT   0x0020
#define GLOB_NOMATCH  3
#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;

static inline int glob(const char *pat, int flags, int (*errfn)(const char *, int), glob_t *g) {
    (void)pat; (void)flags; (void)errfn;
    if (g) { g->gl_pathc = 0; g->gl_pathv = 0; g->gl_offs = 0; }
    return GLOB_NOMATCH;
}

static inline void globfree(glob_t *g) { (void)g; }
