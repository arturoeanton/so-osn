#pragma once

#include <sys/types.h>

/*
 * mmap / munmap. Today osnos only supports the anonymous flavour
 * (`MAP_ANONYMOUS | MAP_PRIVATE`, fd == -1, offset == 0). File-
 * backed mmap returns -1 + ENOSYS. MAP_FIXED is parsed but the
 * addr hint is ignored — the kernel places the region at its own
 * bump cursor (USER_MMAP_BASE).
 */

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS    /* BSD spelling */

#define MAP_FAILED  ((void *)-1)

void *mmap (void *addr, size_t length, int prot, int flags,
            int fd, off_t offset);
int   munmap(void *addr, size_t length);

/* mprotect — no-op on osnos. We don't enforce W^X (every user page is
 * effectively RWX once mapped), so callers that want to mark JIT'd
 * pages executable get a successful no-op. Returns 0. */
int   mprotect(void *addr, size_t length, int prot);

/*
 * POSIX shared memory.
 *   shm_open(name, oflag, mode) → fd
 *   shm_unlink(name)            → 0 / -1 + errno
 *
 * Después de shm_open, usar ftruncate(fd, size) para alocar páginas y
 * mmap(NULL, size, PROT_*, MAP_SHARED, fd, 0) para mapear. Múltiples
 * procesos que abren el mismo `name` (vía O_CREAT u open posterior)
 * ven el mismo objeto subyacente — escribir desde uno aparece en el
 * otro. POSIX: el objeto persiste hasta que shm_unlink + último close
 * lo libera. */
int shm_open  (const char *name, int oflag, mode_t mode);
int shm_unlink(const char *name);
