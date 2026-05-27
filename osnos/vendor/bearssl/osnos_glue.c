/*
 * osnos_glue.c — small glue layer linking BearSSL into osnos.
 *
 *   1. br_prng_seeder_system stub. The default seeder lives in
 *      sysrng.c which we excluded (it hard-includes <x86intrin.h>).
 *      ssl_engine.c calls this at init to bind a system seeder, then
 *      we override with br_ssl_engine_inject_entropy ourselves.
 *   2. osnos_getrandom() — wraps SYS_GETRANDOM so callers can get
 *      entropy without dragging in libc internals.
 */

#include <stdint.h>
#include <stddef.h>

#include "inc/bearssl.h"

/* The real br_prng_seeder_system (in sysrng.c) returns a function
 * pointer (br_prng_seeder) that the engine then calls to seed its
 * DRBG. We return NULL — the engine init tolerates "no seeder
 * available" and skips its automatic seeding. We then call
 * br_ssl_engine_inject_entropy() ourselves with a getrandom-derived
 * seed before starting the handshake. */
br_prng_seeder br_prng_seeder_system(const char **name) {
    if (name) *name = "osnos-stub";
    return 0;
}

/* getrandom syscall — Linux #318, osnos kernel supports it. */
long osnos_getrandom(void *buf, unsigned long len) {
    long r;
    register long r10 __asm__("r10") = 0;     /* flags = 0 */
    __asm__ volatile (
        "syscall"
        : "=a"(r)
        : "0"((long)318), "D"((long)buf), "S"((long)len), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}
