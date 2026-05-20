#include "extable.h"

#include <stddef.h>

typedef struct {
    uintptr_t rip_start;
    uintptr_t rip_end;
    uintptr_t recovery_rip;
} extable_entry_t;

static extable_entry_t entries[EXTABLE_MAX_ENTRIES];
static size_t          entry_count;

bool extable_register(uintptr_t rip_start,
                      uintptr_t rip_end,
                      uintptr_t recovery_rip)
{
    if (rip_start >= rip_end) return false;
    if (entry_count >= EXTABLE_MAX_ENTRIES) return false;

    entries[entry_count].rip_start    = rip_start;
    entries[entry_count].rip_end      = rip_end;
    entries[entry_count].recovery_rip = recovery_rip;
    entry_count++;
    return true;
}

bool extable_lookup(uintptr_t rip, uintptr_t *recovery_out) {
    for (size_t i = 0; i < entry_count; i++) {
        if (rip >= entries[i].rip_start && rip < entries[i].rip_end) {
            if (recovery_out) *recovery_out = entries[i].recovery_rip;
            return true;
        }
    }
    return false;
}
