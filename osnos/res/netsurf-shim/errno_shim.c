/* ox_errno_shim.c — only used when ox.c is recompiled against musl
 * (oxnetsurf, etc.). mini-libc declares `extern int errno;` as a
 * plain global symbol, while musl exposes `(*__errno_location())`.
 * When the ox_*.c sources are pulled into a musl-linked binary,
 * their references to `errno` resolve to neither — link fails with
 * "undefined symbol: errno".
 *
 * Provide the global here so those references link. The musl side
 * keeps using its own __errno_location for everything else; this
 * symbol exists only to satisfy the shim's writes (oxsrv error
 * channel) and reads (EAGAIN check on poll/recv). The two stores
 * are independent — a refactor to use musl's __errno_location
 * is cheap once we need real cross-libc errno consistency.
 *
 * The shim is included in liboxshim.a (NetSurf-side ox client).
 * Building it into libosnos_c.a is harmless because mini-libc
 * already declares the same symbol elsewhere; the linker dedups.
 */
int errno = 0;
