/*
 * strings.h — funciones legacy de string (case-insensitive y BSD).
 * En osnos delegamos a las versiones case-sensitive donde aplica;
 * el case-insensitive es una implementación inline. Suficiente para
 * apps tipo pdpmake que solo necesitan tener el header.
 */
#pragma once
#include <stddef.h>
#include <string.h>

/* strcasecmp / strncasecmp ya viven en <string.h> (mini-libc no-pristine
 * setup). Solo re-declarar acá no aporta. bzero/bcopy son legacy BSD;
 * los proveemos como wrappers inline para apps que los esperan. */

static inline void bzero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static inline void bcopy(const void *src, void *dst, size_t n) {
    memcpy(dst, src, n);
}
