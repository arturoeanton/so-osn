#include "keyboard.h"

#include <stdint.h>
#include <stdbool.h>

#include "../include/osnos_keys.h"

#define PS2_DATA       0x60
#define PS2_STATUS     0x64
#define STAT_OUTBUF    0x01    /* bit 0: data available on 0x60 */
#define STAT_AUX_DATA  0x20    /* bit 5: byte is from AUX (mouse) — NOT ours */

static bool shift_down = false;
static bool ctrl_down = false;
static bool extended = false;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static const char normal_map[128] = {
    [0x01] = 27,

    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0',

    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',

    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p',

    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',

    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l',

    [0x27] = ';', [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',

    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',

    [0x33] = ',', [0x34] = '.', [0x35] = '/',

    [0x39] = ' ',
};

static const char shift_map[128] = {
    [0x01] = 27,

    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')',

    [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b',
    [0x0F] = '\t',

    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P',

    [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',

    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L',

    [0x27] = ':', [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',

    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',

    [0x33] = '<', [0x34] = '>', [0x35] = '?',

    [0x39] = ' ',
};

void keyboard_init(void) {
    shift_down = false;
    ctrl_down = false;
    extended = false;
}

bool keyboard_poll(keyboard_event_t *ev) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & STAT_OUTBUF)) {
        return false;
    }
    /* CRITICAL: leave AUX (mouse) bytes for mouse_poll. Reading them
     * here would interpret mouse deltas/buttons as keyboard scancodes
     * — that's why moving the mouse used to spew garbage into the
     * focused window (the dx/dy bytes happen to match digit/letter
     * scancodes, e.g. 0x02='1', 0x03='2', 0x10='q'). */
    if (st & STAT_AUX_DATA) {
        return false;
    }

    uint8_t scancode = inb(PS2_DATA);

    if (scancode == 0xE0) {
        extended = true;
        return false;
    }

    if (extended) {
        extended = false;

        if (scancode & 0x80) {
            return false;
        }

        ev->ascii = 0;

        switch (scancode) {
            case 0x48: ev->keycode = OSNOS_KEY_UP;    return true;
            case 0x50: ev->keycode = OSNOS_KEY_DOWN;  return true;
            case 0x4B: ev->keycode = OSNOS_KEY_LEFT;  return true;
            case 0x4D: ev->keycode = OSNOS_KEY_RIGHT; return true;
            default:                                  return false;
        }
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_down = true;
        return false;
    }

    if (scancode == 0xAA || scancode == 0xB6) {
        shift_down = false;
        return false;
    }

    if (scancode == 0x1D) {
        ctrl_down = true;
        return false;
    }

    if (scancode == 0x9D) {
        ctrl_down = false;
        return false;
    }

    if (scancode & 0x80) {
        return false;
    }

    if (scancode >= 128) {
        return false;
    }

    char c = shift_down ? shift_map[scancode] : normal_map[scancode];

    if (c == 0) {
        return false;
    }

    /*
     * Ctrl + letter -> ASCII control char (a/A -> 0x01, c/C -> 0x03, etc.)
     * Matches what real terminals deliver in cooked mode.
     */
    if (ctrl_down) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else return false;
    }

    ev->ascii = c;
    ev->keycode = 0;
    return true;
}
