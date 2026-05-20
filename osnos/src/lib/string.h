#pragma once

#include <stdbool.h>
#include <stddef.h>

size_t os_strlen(const char *s);

int os_strcmp(const char *a, const char *b);

int os_strncmp(const char *a, const char *b, size_t n);

int os_streq(const char *a, const char *b);

bool os_strstarts(const char *s, const char *prefix);

char *os_strchr(const char *s, int c);

char *os_strrchr(const char *s, int c);

size_t os_strlcpy(char *dst, const char *src, size_t dst_size);

size_t os_strlcat(char *dst, const char *src, size_t dst_size);

#include <stdint.h>

/*
 * Decimal-format a uint64_t into a null-terminated string. Returns the
 * number of chars written (excluding '\0'). Truncates safely if out_size
 * is too small.
 */
size_t os_format_u64(uint64_t value, char *out, size_t out_size);

void os_memset(void *ptr, int value, size_t size);

void os_memcpy(void *dest, const void *src, size_t size);
