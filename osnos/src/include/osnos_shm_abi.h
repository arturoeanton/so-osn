#pragma once

/*
 * osnos_shm_abi.h — kernel↔userland constants para POSIX SHM.
 * Numeric values matchean Linux <sys/mman.h>.
 */

#define OSNOS_MAP_SHARED    0x01
#define OSNOS_MAP_PRIVATE   0x02
#define OSNOS_MAP_ANONYMOUS 0x20
#define OSNOS_MAP_FIXED     0x10

#define OSNOS_PROT_READ     0x1
#define OSNOS_PROT_WRITE    0x2
#define OSNOS_PROT_EXEC     0x4

#define OSNOS_SHM_NAME_MAX  64
