#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void  *malloc (size_t size);
void  *calloc (size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free   (void *ptr);

int                atoi  (const char *s);
long               atol  (const char *s);
long long          atoll (const char *s);
long               strtol  (const char *s, char **endptr, int base);
long long          strtoll (const char *s, char **endptr, int base);
unsigned long      strtoul (const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

int      abs  (int  n);
long     labs (long n);
long long llabs(long long n);

typedef struct { int       quot; int       rem; } div_t;
typedef struct { long      quot; long      rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
div_t   div  (int       n, int       d);
ldiv_t  ldiv (long      n, long      d);
lldiv_t lldiv(long long n, long long d);

/*
 * qsort / bsearch — standard interface. `compar(a, b)` returns
 * negative / zero / positive. qsort uses insertion sort up to small
 * sizes and a simple recursive quicksort otherwise.
 */
void  qsort  (void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *));

/*
 * Environment. crt0 publishes the kernel-passed envp[] as the global
 * `environ` pointer; getenv walks that table. setenv / unsetenv /
 * putenv mutate a heap-resident copy (the initial array lives on the
 * stack so the first mutation duplicates it).
 */
extern char **environ;

char *getenv  (const char *name);
int   setenv  (const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv  (char *kv);

/*
 * atexit — registers a function to run on exit(). Up to 32 slots,
 * invoked in LIFO order. Returns 0 on success, non-zero on overflow.
 */
int   atexit  (void (*fn)(void));

__attribute__((noreturn))
void   exit   (int code);

__attribute__((noreturn))
void   abort  (void);
