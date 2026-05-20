#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Realtek RTL8139 driver — vendor 0x10EC device 0x8139.
 *
 * Minimal PIO + DMA setup against the legacy RTL8139C(L) chip QEMU
 * emulates with `-device rtl8139`. One static RX ring (8 KiB + 16 +
 * 1500 padding, physically contiguous) and four 1-page TX buffers.
 * IRQs land on whichever line the PIC has wired for the device's
 * configured INTx (typically 11 with -M pc / PIIX3).
 *
 * Boot order: must run after pic_init + lapic_init + timer_init so the
 * 8259 + LAPIC + IDT plumbing is alive when the chip's first IRQ
 * fires. block_ata_init has no IRQ dependency and may run before or
 * after.
 *
 * Returns false on no-device-found; failure is non-fatal — the rest of
 * the kernel boots normally without networking.
 */

bool        rtl8139_init(void);
bool        rtl8139_present(void);

const uint8_t *rtl8139_mac(void);
uint16_t    rtl8139_io_base(void);
uint8_t     rtl8139_irq_line(void);

uint64_t    rtl8139_irqs(void);
uint64_t    rtl8139_rx_packets(void);
uint64_t    rtl8139_tx_packets(void);
uint64_t    rtl8139_rx_bytes(void);
uint64_t    rtl8139_tx_bytes(void);
uint64_t    rtl8139_errors(void);

/*
 * Synchronous send of a raw Ethernet frame. `buf` is copied into one of
 * four ring TX buffers and the chip is kicked. Returns false if the
 * driver isn't present, the length exceeds the TX buffer, or every TX
 * slot is still owned by the chip (all four busy).
 */
bool rtl8139_tx(const void *buf, size_t len);

/*
 * Register a callback that's invoked from IRQ context for each
 * received Ethernet frame (already trimmed of the trailing 4-byte
 * CRC). The callback runs with interrupts ENABLED so anything it does
 * with shared state has to handle re-entry; keep it short — append
 * to a queue or do a quick stateless dispatch.
 */
typedef void (*rtl8139_rx_fn)(const uint8_t *frame, size_t len);
void rtl8139_set_rx_callback(rtl8139_rx_fn fn);
