#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * UART 16550 driver — COM1 by default (port 0x3F8).
 *
 * Used for two things:
 *   1. Dual-console output: framebuffer_write_bytes + panic handlers
 *      also call serial_puts so the host can capture everything via
 *      QEMU's -serial mon:stdio / -serial file:.
 *   2. Optional headless input: serial_input_server (kernel task)
 *      polls serial_try_getc and feeds bytes to tty_input().
 *
 * Polling-only. No IRQ wiring (cooperative kernel task does the
 * polling at scheduler-tick frequency, ~10 ms — imperceptible).
 */

#define SERIAL_COM1     0x3F8

/* Initialize the UART at `port` for 38400 8N1 with 16550 FIFOs on,
 * IRQs disabled (polling mode). Safe to call before any other init
 * — uses only direct I/O port writes, no memory, no IRQ. */
void serial_init(uint16_t port);

/* Write one byte, spinning until LSR THRE (transmit hold empty).
 * '\n' is auto-converted to "\r\n" for terminal-friendly output. */
void serial_putc(char c);

/* Write n bytes. Equivalent to a loop of serial_putc; provided so
 * call sites in framebuffer.c / panic handlers can be one-liners. */
void serial_puts(const char *s, size_t n);

/* Non-blocking receive. Returns true if a byte was available and
 * stored in *out; false if the UART had nothing ready. Called by
 * serial_input_server every tick. */
bool serial_try_getc(uint8_t *out);
