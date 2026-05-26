#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * PS/2 mouse driver — polled, cooperative.
 *
 * QEMU's `-M pc` always provides a PS/2 mouse on the auxiliary port
 * of the 8042 controller (IRQ 12, but we don't wire that up — we
 * drain the controller FIFO via polling, same as the keyboard driver).
 *
 * Each event is the canonical 3-byte PS/2 packet decoded into our
 * mouse_event_t. The driver tracks packet sync (bit 3 of byte 0 is
 * always 1) and resyncs by dropping bytes until that holds.
 *
 * Coordinate convention:
 *   dx > 0 → cursor moves right
 *   dy > 0 → cursor moves DOWN (we invert PS/2's "up is +y"
 *            so userland can directly add deltas to a screen-y).
 *
 * Buttons bitmap matches the PS/2 protocol byte-0 layout but in
 * its own bit-3-cleared form:
 *   bit 0 = left
 *   bit 1 = right
 *   bit 2 = middle
 */

typedef struct {
    int16_t dx;
    int16_t dy;
    uint8_t buttons;
    int8_t  wheel;         /* +1 = wheel up (away), -1 = down. 0 if no wheel device. */
} mouse_event_t;

/* Initialize the 8042 auxiliary port: enable AUX, set defaults
 * (200 samples/sec, 1:1 scaling), enable streaming. Each command
 * waits for ACK (0xFA) with a bounded poll so a missing mouse
 * fails fast instead of hanging. */
void mouse_init(void);

/* Non-blocking poll: drain one byte from the controller FIFO if
 * AUX_DATA is ready; once 3 bytes accumulate, emit *out and return
 * true. Returns false if no full packet is available yet. */
bool mouse_poll(mouse_event_t *out);
