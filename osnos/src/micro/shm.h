#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_shm_abi.h"
#include "../include/osnos_status.h"

/*
 * POSIX shared memory (named).
 *
 * Modelo: pool fijo de objetos shm. Cada uno tiene name, refcount,
 * size en bytes, y lista de páginas físicas (capped). El refcount
 * cuenta cada fd que lo referencia + 1 por "still linked en el
 * namespace" (esto último lo decrementa shm_unlink). Cuando llega
 * a cero, las páginas físicas se liberan.
 *
 * shm_open: crea o lookup por nombre. Bumps refcount, retorna
 *           handle (índice del pool).
 * shm_truncate: reasigna pages para hacer match con `size`. ftruncate
 *               normal del fd llama a esto. Inicial size = 0.
 * shm_get_phys_page: dado un offset, retorna la dirección física
 *                    de esa página para que sys_mmap pueda mapearla.
 * shm_ref/unref: refcount management. Cuando hits 0 + unlinked,
 *                liberar pages.
 * shm_unlink: el nombre desaparece del namespace; opens futuros con
 *             ese name no lo encuentran. Existing fds siguen vivos
 *             hasta que se cierren todos.
 */

/* Bumped from 16/256 to 32/1024 for FASE perf: oxsrv backs every
 * window with a shm so the client can draw locally and only ping
 * oxsrv via IPC_OX_PRESENT (vs 1 IPC per draw call). 32 objects
 * (one per oxsrv window × 2 mappers) × 4 MiB max covers oxsettings
 * thumbs (~1.6 MB) and even bigger windows. */
#define SHM_OBJ_MAX        32
#define SHM_PAGES_PER_OBJ  1024  /* 4 MiB max por objeto */

typedef struct shm_obj shm_obj_t;

void shm_init(void);

/* shm_open: lookup-or-create. Retorna handle opaco (puntero al
 * shm_obj). NULL si pool lleno o name inválido. `create_if_missing`:
 * O_CREAT semantics. Bumps refcount al retornar. */
shm_obj_t *shm_open(const char *name, bool create_if_missing);

/* Decrement refcount. Si llega a cero + name está unlinked, libera
 * las páginas físicas y recicla el slot. */
void shm_unref(shm_obj_t *obj);

/* shm_unlink: marca el nombre como gone. Si refcount==0, libera ya.
 * Sino, persiste hasta el último close. Retorna OK / ENOENT. */
osnos_status_t shm_unlink(const char *name);

/* Resize: alloca o libera páginas para que size_bytes coincida.
 * Errno: EINVAL (size > max), ENOMEM. */
osnos_status_t shm_truncate(shm_obj_t *obj, size_t size_bytes);

/* Tamaño actual en bytes (lo que la última truncate dejó). */
size_t shm_size(const shm_obj_t *obj);

/* Página física para un offset (debe estar adentro del size).
 * Retorna 0 si offset fuera de range o página no alocada. */
uint64_t shm_phys_page(const shm_obj_t *obj, size_t page_off);
