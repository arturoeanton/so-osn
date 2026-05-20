#include "pci.h"

#include <stdbool.h>
#include <stdint.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)(dev & 0x1F) << 11)
                  | ((uint32_t)(fn  & 0x07) << 8)
                  | ((uint32_t)reg & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

static void cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                       uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)(dev & 0x1F) << 11)
                  | ((uint32_t)(fn  & 0x07) << 8)
                  | ((uint32_t)reg & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, val);
}

bool pci_find_device(uint16_t vendor, uint16_t device, pci_addr_t *out) {
    /* Bus 0 is enough for PIIX3 / single-host setups (QEMU -M pc). */
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t v = cfg_read((uint8_t)bus, dev, 0, 0x00);
            uint16_t vid = (uint16_t)(v & 0xFFFFu);
            uint16_t did = (uint16_t)(v >> 16);
            if (vid == 0xFFFF) continue;            /* slot empty */
            if (vid == vendor && did == device) {
                out->bus = (uint8_t)bus;
                out->dev = dev;
                out->fn  = 0;
                return true;
            }
        }
    }
    return false;
}

uint32_t pci_read_bar(const pci_addr_t *a, int bar) {
    if (bar < 0 || bar > 5) return 0;
    return cfg_read(a->bus, a->dev, a->fn, (uint8_t)(0x10 + bar * 4));
}

uint8_t pci_read_irq(const pci_addr_t *a) {
    uint32_t v = cfg_read(a->bus, a->dev, a->fn, 0x3C);
    return (uint8_t)(v & 0xFF);
}

void pci_enable_bus_master(const pci_addr_t *a) {
    uint32_t cmd = cfg_read(a->bus, a->dev, a->fn, 0x04);
    /* bit 0 = I/O space enable, bit 2 = bus master enable. Both are
     * required for an MMIO/PIO + DMA-capable device to function. */
    cmd |= 0x05u;
    cfg_write(a->bus, a->dev, a->fn, 0x04, cmd);
}

uint32_t pci_config_read32(const pci_addr_t *a, uint8_t reg) {
    return cfg_read(a->bus, a->dev, a->fn, reg);
}
