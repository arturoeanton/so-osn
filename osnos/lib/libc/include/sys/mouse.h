#pragma once

#include <stdint.h>

/*
 * User-facing mirror of the kernel `mouse_event_t` (struct layout
 * must stay binary-compatible with `src/drivers/mouse.h`).
 *
 * Each read from /dev/mouse0 returns one of these on success.
 * Movement is delta-only (PS/2 protocol):
 *   dx > 0 → right, dx < 0 → left
 *   dy > 0 → down,  dy < 0 → up
 * Buttons bitmask:
 *   bit 0 = left, bit 1 = right, bit 2 = middle.
 * Wheel: signed delta (+1 = up/away, -1 = down/toward). Requires
 * IntelliMouse-compatible PS/2 (auto-detected at boot). 0 if device
 * lacks a wheel or the magic-knock sequence didn't take.
 */
typedef struct {
    int16_t dx;
    int16_t dy;
    uint8_t buttons;
    int8_t  wheel;
} mouse_event_t;

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04
