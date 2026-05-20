#pragma once

#include <stddef.h>

#include "osnos_limits.h"

/*
 * Path value type. Carries the buffer and its length explicitly to avoid the
 * common null-term ambiguities when paths are sliced (e.g. extracting a
 * parent directory). Intended for use across the future VFS boundary.
 *
 * Currently a sketch: no functions are exported yet. The VFS layer (FASE 2)
 * will populate this.
 */
typedef struct {
    char buf[OSNOS_PATH_MAX];
    size_t len;
} osnos_path_t;
