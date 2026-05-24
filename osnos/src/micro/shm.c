#include "shm.h"

#include "pmm.h"
#include "../lib/string.h"

struct shm_obj {
    bool      used;
    bool      unlinked;          /* shm_unlink ran; no new opens */
    int       refcount;
    char      name[OSNOS_SHM_NAME_MAX];
    size_t    size_bytes;
    /* phys_pages[i] = 0 si no alocada (lazy alloc dentro del rango
     * size_bytes; ftruncate hace eager alloc). */
    uint64_t  phys_pages[SHM_PAGES_PER_OBJ];
    size_t    n_pages;           /* high watermark de páginas alocadas */
};

static shm_obj_t shm_pool[SHM_OBJ_MAX];

void shm_init(void) {
    for (int i = 0; i < SHM_OBJ_MAX; i++) {
        shm_pool[i].used     = false;
        shm_pool[i].unlinked = false;
        shm_pool[i].refcount = 0;
        shm_pool[i].size_bytes = 0;
        shm_pool[i].n_pages    = 0;
        for (int p = 0; p < SHM_PAGES_PER_OBJ; p++) {
            shm_pool[i].phys_pages[p] = 0;
        }
    }
}

static shm_obj_t *find_by_name(const char *name) {
    for (int i = 0; i < SHM_OBJ_MAX; i++) {
        if (shm_pool[i].used && !shm_pool[i].unlinked &&
            os_streq(shm_pool[i].name, name)) {
            return &shm_pool[i];
        }
    }
    return 0;
}

static shm_obj_t *alloc_obj(void) {
    for (int i = 0; i < SHM_OBJ_MAX; i++) {
        if (!shm_pool[i].used) return &shm_pool[i];
    }
    return 0;
}

static void free_pages(shm_obj_t *o) {
    for (size_t i = 0; i < o->n_pages; i++) {
        if (o->phys_pages[i]) {
            pmm_free_page(o->phys_pages[i]);
            o->phys_pages[i] = 0;
        }
    }
    o->n_pages    = 0;
    o->size_bytes = 0;
}

shm_obj_t *shm_open(const char *name, bool create_if_missing) {
    if (!name || !name[0]) return 0;

    shm_obj_t *o = find_by_name(name);
    if (o) {
        o->refcount++;
        return o;
    }
    if (!create_if_missing) return 0;

    o = alloc_obj();
    if (!o) return 0;

    o->used     = true;
    o->unlinked = false;
    o->refcount = 1;
    o->size_bytes = 0;
    o->n_pages    = 0;
    os_strlcpy(o->name, name, OSNOS_SHM_NAME_MAX);
    for (int p = 0; p < SHM_PAGES_PER_OBJ; p++) o->phys_pages[p] = 0;
    return o;
}

void shm_unref(shm_obj_t *o) {
    if (!o || !o->used) return;
    if (o->refcount > 0) o->refcount--;

    if (o->refcount == 0 && o->unlinked) {
        free_pages(o);
        o->used     = false;
        o->unlinked = false;
        o->name[0]  = 0;
    }
}

osnos_status_t shm_unlink(const char *name) {
    if (!name || !name[0]) return OSNOS_EINVAL;
    shm_obj_t *o = find_by_name(name);
    if (!o) return OSNOS_ENOENT;
    o->unlinked = true;
    if (o->refcount == 0) {
        /* Nadie con fd abierto — release ahora. */
        free_pages(o);
        o->used    = false;
        o->name[0] = 0;
    }
    return OSNOS_OK;
}

osnos_status_t shm_truncate(shm_obj_t *o, size_t size_bytes) {
    if (!o || !o->used) return OSNOS_EBADF;
    size_t pages_needed = (size_bytes + 4095) / 4096;
    if (pages_needed > SHM_PAGES_PER_OBJ) return OSNOS_EINVAL;

    /* Grow: alocar nuevas páginas + zero-init (POSIX dice ftruncate
     * extiende con ceros). */
    if (pages_needed > o->n_pages) {
        for (size_t i = o->n_pages; i < pages_needed; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                /* OOM — revert lo recién alocado. */
                for (size_t j = o->n_pages; j < i; j++) {
                    if (o->phys_pages[j]) {
                        pmm_free_page(o->phys_pages[j]);
                        o->phys_pages[j] = 0;
                    }
                }
                return OSNOS_ENOMEM;
            }
            uint8_t *kv = (uint8_t *)(phys + pmm_hhdm_offset());
            for (size_t b = 0; b < 4096; b++) kv[b] = 0;
            o->phys_pages[i] = phys;
        }
        o->n_pages = pages_needed;
    }
    /* Shrink: liberar las páginas excedentes. */
    if (pages_needed < o->n_pages) {
        for (size_t i = pages_needed; i < o->n_pages; i++) {
            if (o->phys_pages[i]) {
                pmm_free_page(o->phys_pages[i]);
                o->phys_pages[i] = 0;
            }
        }
        o->n_pages = pages_needed;
    }
    o->size_bytes = size_bytes;
    return OSNOS_OK;
}

size_t shm_size(const shm_obj_t *o) {
    if (!o || !o->used) return 0;
    return o->size_bytes;
}

uint64_t shm_phys_page(const shm_obj_t *o, size_t page_off) {
    if (!o || !o->used) return 0;
    if (page_off >= o->n_pages) return 0;
    return o->phys_pages[page_off];
}
