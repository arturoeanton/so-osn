#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Minimal PCI configuration-space access over the legacy 0xCF8/0xCFC
 * I/O port pair. Enough to find a known (vendor, device) device and
 * read its BAR0 + IRQ line for driver init. No support for PCIe ECAM,
 * multi-function probing, or capability lists yet — those land when
 * we need more than one network/storage card.
 */

typedef struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
} pci_addr_t;

/* Iterate every bus/device/fn=0 slot looking for the given IDs.
 * Returns true with `*out` filled on first hit. */
bool     pci_find_device(uint16_t vendor, uint16_t device, pci_addr_t *out);

/* Read BAR N (0..5) raw value from PCI config offset 0x10 + 4*N. The
 * low 1-2 bits encode the BAR type (I/O vs memory) and are not masked
 * here — caller decides. */
uint32_t pci_read_bar(const pci_addr_t *a, int bar);

/* Read the device's interrupt line (8-bit IRQ number). */
uint8_t  pci_read_irq(const pci_addr_t *a);

/* Set the bus-master + I/O-space bits in the PCI command register so
 * the device can drive transactions on the bus and respond to I/O
 * port accesses. Required before talking to the BAR0 registers. */
void     pci_enable_bus_master(const pci_addr_t *a);

/* Raw 32-bit read of an aligned config-space register. */
uint32_t pci_config_read32(const pci_addr_t *a, uint8_t reg);
