#pragma once

/*
 * Kernel-side cooperative mouse task — mirror of keyboard_server.
 * Drains the PS/2 AUX FIFO each scheduler tick, decodes 3-byte
 * packets via mouse_poll, and pushes mouse_event_t values into
 * the /dev/mouse0 devfs ring buffer.
 *
 * When FASE 11+ migrates drivers to ring 3 with real IRQ delivery,
 * this becomes a ring-3 ELF and the polling goes away. Until then
 * it lives here alongside keyboard_server.
 */

void mouse_server_init(void);
void mouse_server_tick(void);
