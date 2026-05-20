#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * FAT16 driver — read-only in 8.2, write lands in 8.4.
 *
 * Sits on top of block_ata.c. Parses the BPB at sector 0, derives the
 * region layout (FAT / root dir / data), and exposes path-based lookup,
 * directory iteration and byte-range read.
 *
 * Scope:
 *   - FAT16 only. FAT12 and FAT32 rejected at init. fat_present()
 *     returns false if anything looks off and every other call no-ops.
 *   - 8.3 short names only. LFN (Long File Name) entries are skipped
 *     during iteration; a file with no 8.3 alias is invisible.
 *   - Single mounted volume (block_ata primary master).
 *
 * Path conventions:
 *   - fat_lookup takes a path relative to the FAT root: "/README.TXT",
 *     "/" (root itself), "FOO/BAR" (leading slash optional). The
 *     VFS adapter strips the mount prefix ("/sd") before calling.
 *   - All names are case-insensitive on the way in, uppercase on disk.
 */

typedef struct {
    /* Display name. For root the string is "/". Otherwise 8.3 form
     * with a single '.' separator if there's an extension. */
    char     name[13];

    bool     is_dir;
    uint32_t size;             /* 0 for directories */
    uint16_t first_cluster;    /* 0 means "the root directory" sentinel */

    /* Location of the 32-byte raw dirent on disk. Required by fat_write
     * (FASE 8.4) for size/cluster updates and by fat_unlink for
     * marking the entry deleted. Zero for the root sentinel. */
    uint32_t dirent_lba;
    uint32_t dirent_offset;
} fat_dirent_t;

/*
 * Initialise the FAT layer. Reads sector 0, validates the BPB, fills
 * the region layout. Returns 0 on success. Safe to call before
 * block_ata_init(): it will fail cleanly.
 */
int  fat_init(void);
bool fat_present(void);

/*
 * Resolve a path to its directory entry.
 *   path = "/"            -> root sentinel (first_cluster=0, is_dir=true)
 *   path = "README.TXT"   -> root child
 *   path = "/SUB/FILE"    -> walks into SUB
 * Returns 0 on success, -1 if any component is missing / malformed.
 */
int  fat_lookup(const char *path, fat_dirent_t *out);

/*
 * Read up to `len` bytes from `de` starting at byte offset `off` into
 * `buf`. Returns # bytes copied (may be < len at EOF) or -1 on error.
 * Reading past EOF returns 0.
 */
int  fat_read_file(const fat_dirent_t *de, uint32_t off,
                   char *buf, uint32_t len);

/*
 * Iterate entries of a directory. `cursor` starts at 0; the function
 * skips deleted / LFN / volume-label slots and writes the *next* cursor
 * value to `*next_cursor`. Caller calls repeatedly until non-zero
 * return. Returns 0 on success, -1 on end-of-directory or error.
 *
 * `dir` may be the root sentinel (first_cluster=0).
 */
int  fat_readdir(const fat_dirent_t *dir, uint32_t cursor,
                 fat_dirent_t *out, uint32_t *next_cursor);

/* ----- Write-side (FASE 8.4) ----- */
/*
 * All write ops keep the on-disk FAT mirror consistent — every FAT
 * entry update is replayed across all num_fats copies before returning.
 * Crash-recovery ordering is: allocate-new-chain → write-data →
 * update-dirent → free-old-chain. A crash in the middle leaves either
 * the old state intact or an orphan chain (lost clusters; fsck'able).
 *
 * Return 0 on success, negative errno-style codes on failure:
 *   -2  ENOENT  -17 EEXIST  -20 ENOTDIR  -21 EISDIR  -22 EINVAL
 *   -28 ENOSPC  -39 ENOTEMPTY                          -5 EIO
 */
int  fat_write_path  (const char *path, const char *buf, uint32_t len);
int  fat_append_path (const char *path, const char *buf, uint32_t len);
int  fat_unlink_path (const char *path);
int  fat_mkdir_path  (const char *path);
int  fat_rmdir_path  (const char *path);
