#include <pthread.h>

/*
 * Single-threaded pthread shim: thread-local storage degenerates
 * to a global table of void* slots. PTHREAD_KEYS_MAX (64) is more
 * than enough for our porting targets (Lua, jq, sqlite each use
 * 0–2 keys). Destructors are ignored — the task always exits with
 * the process, so per-thread cleanup never fires anyway.
 */

#define MAX_KEYS PTHREAD_KEYS_MAX

static void *tls_slots[MAX_KEYS];
static int   tls_used [MAX_KEYS];
static unsigned next_key = 0;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor;
    if (!key) return -1;
    if (next_key >= MAX_KEYS) return -1;
    unsigned k = next_key++;
    tls_used[k] = 1;
    tls_slots[k] = 0;
    *key = k;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= MAX_KEYS) return -1;
    tls_used[key] = 0;
    tls_slots[key] = 0;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= MAX_KEYS || !tls_used[key]) return -1;
    tls_slots[key] = (void *)value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= MAX_KEYS || !tls_used[key]) return 0;
    return tls_slots[key];
}
