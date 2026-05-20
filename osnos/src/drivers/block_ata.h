#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * ATA PIO block driver — primary master only.
 *
 * Talks to whatever ATA disk QEMU exposes as `-drive ...,if=ide` on the
 * Primary IDE channel (command block @ 0x1F0, control @ 0x3F6). LBA28
 * mode, 28-bit sector addressing — 128 GiB cap, more than enough for
 * the 16 MiB sd.img we ship.
 *
 * Polling I/O: no IRQs, no DMA. Cheap and tiny (~150 LOC). Good enough
 * for a hobby FS layer. When perf matters, VirtIO block lands later
 * and exposes the same surface here.
 *
 * Call block_ata_init() once at boot (after lapic_init / timer_init,
 * before any FS that wants to mount on top). If no disk responds to
 * IDENTIFY, block_ata_present() returns false and every read/write
 * returns -1 — callers must check.
 */

void        block_ata_init(void);
bool        block_ata_present(void);
uint64_t    block_ata_sector_count(void);
const char *block_ata_model(void);

/*
 * Read / write a single 512-byte sector at LBA.
 *
 * Returns 0 on success, -1 on any error (no drive, LBA out of range,
 * ERR/DF bit set, timeout). Buffer must be at least 512 bytes; on
 * read it does not need to be word-aligned but x86 unaligned 16-bit
 * I/O is fine in practice (and we'd word-align everything anyway).
 */
int  block_ata_read_sector (uint64_t lba, void *buf);
int  block_ata_write_sector(uint64_t lba, const void *buf);

#define BLOCK_ATA_SECTOR_SIZE 512
