#pragma once

/*
 * alloca: stack-allocated buffer. clang/gcc support this as a
 * compiler builtin even when no libc backing exists. We expose the
 * builtin under the standard name. Calls deallocate when the
 * enclosing function returns. Use sparingly — there's no stack-
 * overflow protection.
 */

#include <stddef.h>

#define alloca(n) __builtin_alloca(n)
