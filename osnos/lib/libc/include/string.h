#pragma once

#include <stddef.h>

size_t strlen (const char *s);
int    strcmp (const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy (char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat (char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
char  *strchr (const char *s, int c);
char  *strrchr(const char *s, int c);

void  *memcpy (void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset (void *dst, int c, size_t n);
int    memcmp (const void *a, const void *b, size_t n);
void  *memchr (const void *s, int c, size_t n);
void  *memrchr(const void *s, int c, size_t n);

size_t strnlen(const char *s, size_t maxlen);
char  *strdup (const char *s);
char  *strndup(const char *s, size_t n);
char  *strstr (const char *hay, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn (const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/*
 * strtok / strtok_r — split `s` on any byte in `delim`. First call
 * passes the string; subsequent calls pass NULL (strtok) or thread the
 * saveptr (strtok_r). Returns NULL when no more tokens.
 */
char  *strtok  (char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **saveptr);

int    strcasecmp (const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

/*
 * strerror — short ASCII name for `errno`. Falls back to "errno=N"
 * for unknown codes. The returned pointer is a static buffer shared
 * across calls; copy if you need to keep it past the next strerror.
 */
char  *strerror(int errnum);
