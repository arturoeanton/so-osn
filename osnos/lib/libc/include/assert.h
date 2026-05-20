#pragma once

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr) \
    ((expr) ? (void)0 \
            : (fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                       __FILE__, __LINE__, #expr), \
               abort()))
#endif
