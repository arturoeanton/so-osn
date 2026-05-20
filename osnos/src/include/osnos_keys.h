#pragma once

#include <stdint.h>

/*
 * Subset of Linux input-event-codes (linux/input-event-codes.h).
 * Numeric values match Linux exactly so /dev/input event records can be
 * forwarded unmodified to userspace once we expose them.
 */
typedef enum {
    OSNOS_KEY_NONE       = 0,
    OSNOS_KEY_BACKSPACE  = 14,
    OSNOS_KEY_ENTER      = 28,
    OSNOS_KEY_UP         = 103,
    OSNOS_KEY_LEFT       = 105,
    OSNOS_KEY_RIGHT      = 106,
    OSNOS_KEY_DOWN       = 108
} osnos_key_t;
