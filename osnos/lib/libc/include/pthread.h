#pragma once

/*
 * pthread.h — single-threaded stub for osnos. ALL operations are
 * no-ops or trivial: mutexes are sequencers (no contention possible
 * because there's only one thread), pthread_key is a global void*
 * slot, etc.
 *
 * Ported software that uses pthreads for "safety" (Lua/jq locking
 * around their global allocator state) Just Works — they grab
 * the no-op mutex and proceed. We're not multi-threaded yet so
 * race conditions are physically impossible.
 *
 * When osnos grows real threads (probably FASE 13+), replace
 * these with real implementations and recompile everything.
 */

#include <stddef.h>

/* ---- Mutexes (no-op) ---- */
typedef int pthread_mutex_t;
typedef int pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER 0

static inline int pthread_mutex_init(pthread_mutex_t *m,
                                       const pthread_mutexattr_t *a) {
    (void)a; if (m) *m = 0; return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m; return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    (void)m; return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    (void)m; return 0;
}
static inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    (void)m; return 0;
}

/* ---- Thread-local storage (global slot in single-thread world) ---- */
#define PTHREAD_KEYS_MAX 64
typedef unsigned pthread_key_t;

int   pthread_key_create   (pthread_key_t *key, void (*destructor)(void *));
int   pthread_key_delete   (pthread_key_t key);
int   pthread_setspecific  (pthread_key_t key, const void *value);
void *pthread_getspecific  (pthread_key_t key);

/* ---- once init (single-thread → just guard with a flag) ---- */
typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT 0
static inline int pthread_once(pthread_once_t *guard, void (*init)(void)) {
    if (!*guard) { *guard = 1; if (init) init(); }
    return 0;
}
