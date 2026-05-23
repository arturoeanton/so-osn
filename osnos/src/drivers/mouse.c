#include "mouse.h"

#include <stdint.h>
#include <stdbool.h>

/* 8042 controller I/O ports (same as keyboard). */
#define PS2_DATA       0x60
#define PS2_STATUS     0x64
#define PS2_CMD        0x64    /* writes here are controller commands */

/* Status register bits (read from 0x64). */
#define STAT_OUTBUF    0x01    /* bit 0: output buffer has data       */
#define STAT_INBUF     0x02    /* bit 1: input buffer full (busy)     */
#define STAT_AUX_DATA  0x20    /* bit 5: byte came from AUX (mouse)   */

/* Controller commands (written to 0x64). */
#define CMD_ENABLE_AUX 0xA8    /* Enable the auxiliary device         */
#define CMD_WRITE_AUX  0xD4    /* Next byte to 0x60 goes to AUX       */

/* Mouse device commands (written to 0x60 AFTER CMD_WRITE_AUX). */
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE_STREAM 0xF4

/* Expected ACK byte from the mouse on any command. */
#define MOUSE_ACK 0xFA

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

/* Bounded wait until the controller is ready for input/output.
 * Bounded so a missing mouse fails after ~10 ms instead of locking
 * the boot. */
static bool wait_inbuf_empty(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & STAT_INBUF)) return true;
    }
    return false;
}

/* Send one byte to the mouse (auxiliary port). Returns ACK status. */
static bool mouse_command(uint8_t byte) {
    if (!wait_inbuf_empty()) return false;
    outb(PS2_CMD, CMD_WRITE_AUX);
    if (!wait_inbuf_empty()) return false;
    outb(PS2_DATA, byte);

    /* Wait for ACK on the AUX side. Loop until we either see ACK,
     * see a non-AUX byte (drop it — likely a stray keyboard scancode
     * — and keep waiting), or time out. */
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & STAT_OUTBUF)) continue;
        uint8_t b = inb(PS2_DATA);
        if (!(st & STAT_AUX_DATA)) continue;   /* not our byte */
        return b == MOUSE_ACK;
    }
    return false;
}

static bool mouse_present = false;

void mouse_init(void) {
    /* Enable the AUX device on the controller. No ACK expected from
     * the controller itself, this is a controller command. */
    if (!wait_inbuf_empty()) return;
    outb(PS2_CMD, CMD_ENABLE_AUX);

    /* Tell the mouse to load defaults (200 Hz, 1:1, streaming off). */
    if (!mouse_command(MOUSE_SET_DEFAULTS)) return;

    /* Enable streaming so movements/clicks generate packets. */
    if (!mouse_command(MOUSE_ENABLE_STREAM)) return;

    mouse_present = true;
}

/* ---- packet assembly ---- */

static uint8_t packet[3];
static int     packet_idx = 0;

static int16_t decode_axis(uint8_t byte, bool sign) {
    /* 9-bit signed: byte is the low 8 bits, sign extends from bit
     * 4 of byte 0 (passed in as `sign`). Convert to int16_t. */
    int16_t v = byte;
    if (sign) v -= 0x100;
    return v;
}

bool mouse_poll(mouse_event_t *out) {
    if (!mouse_present || !out) return false;

    /* Drain at most one byte per call so we don't starve the rest
     * of the scheduler. Caller (mouse_server_tick) is invoked once
     * per scheduler tick anyway. */
    uint8_t st = inb(PS2_STATUS);
    if (!(st & STAT_OUTBUF)) return false;
    if (!(st & STAT_AUX_DATA)) {
        /* Byte came from the keyboard half — leave it for the
         * keyboard driver's next poll. */
        return false;
    }
    uint8_t b = inb(PS2_DATA);

    /* Sync byte must have bit 3 = 1. If not, drop and resync. */
    if (packet_idx == 0 && !(b & 0x08)) {
        return false;
    }
    packet[packet_idx++] = b;
    if (packet_idx < 3) return false;

    /* Full packet assembled — decode + emit. */
    uint8_t flags = packet[0];
    int16_t dx = decode_axis(packet[1], (flags & 0x10) != 0);
    /* PS/2 reports +y as "up"; flip so dy>0 means "down" on
     * the screen — what userland naturally expects. */
    int16_t dy = -decode_axis(packet[2], (flags & 0x20) != 0);
    /* Overflow bits (bits 6, 7) — clamp to a sentinel rather than
     * trust nonsense. Simpler than computing the actual clamp. */
    if (flags & 0x40) dx = (dx < 0) ? -255 : 255;
    if (flags & 0x80) dy = (dy < 0) ? -255 : 255;

    out->dx      = dx;
    out->dy      = dy;
    out->buttons = flags & 0x07;
    out->_pad    = 0;

    packet_idx = 0;
    return true;
}
