#include "fat.h"

#include "../drivers/block_ata.h"
#include "../lib/string.h"
#include "../micro/kmalloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Dirent layout (32 bytes):
 *   0..10   8.3 short name (8 name + 3 ext, space-padded)
 *   11      attribute byte
 *   12..21  reserved / case / create-time
 *   22..23  write time           (we ignore)
 *   24..25  write date           (we ignore)
 *   26..27  first cluster (low)  — FAT16 uses only this half
 *   28..31  file size (LE)
 */
#define DIRENT_SIZE         32

#define ATTR_RO             0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOL            0x08
#define ATTR_DIR            0x10
#define ATTR_ARCH           0x20
#define ATTR_LFN            0x0F    /* RO|HIDDEN|SYS|VOL — special tag */

#define FAT16_EOF_MIN       0xFFF8
#define FAT16_BAD           0xFFF7

#define SECTOR_SIZE         512

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint32_t total_sectors;
    uint16_t fat_size_sectors;

    /* Derived. */
    uint32_t fat_lba;             /* first FAT */
    uint32_t root_dir_lba;        /* fixed-region root dir start */
    uint32_t root_dir_sectors;
    uint32_t data_lba;            /* logical LBA of cluster #2 - 2*spc */
} fat_state_t;

static fat_state_t fs;
static bool fs_ready = false;

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint32_t cluster_to_lba(uint16_t cluster) {
    /* Data region starts at the LBA computed from cluster 2. */
    return fs.data_lba + (uint32_t)(cluster - 2) * fs.sectors_per_cluster;
}

/*
 * Look up the FAT entry for `cluster`. *next receives the 16-bit value
 * (next cluster, 0=free, or 0xFFF8..0xFFFF=EOF). Returns 0 on success.
 */
static int fat_get_entry(uint16_t cluster, uint16_t *next) {
    uint32_t byte_off = (uint32_t)cluster * 2;
    uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
    uint32_t in_sec   = byte_off % SECTOR_SIZE;

    uint8_t buf[SECTOR_SIZE];
    if (block_ata_read_sector(sec, buf) != 0) return -1;
    *next = rd16(buf + in_sec);
    return 0;
}

int fat_init(void) {
    fs_ready = false;

    if (!block_ata_present()) return -1;

    uint8_t boot[SECTOR_SIZE];
    if (block_ata_read_sector(0, boot) != 0) return -1;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return -1;

    fs.bytes_per_sector    = rd16(boot + 0x0B);
    fs.sectors_per_cluster = boot[0x0D];
    fs.reserved_sectors    = rd16(boot + 0x0E);
    fs.num_fats            = boot[0x10];
    fs.root_entries        = rd16(boot + 0x11);
    uint16_t small_total   = rd16(boot + 0x13);
    fs.fat_size_sectors    = rd16(boot + 0x16);
    uint32_t big_total     = rd32(boot + 0x20);

    if (fs.bytes_per_sector != SECTOR_SIZE)     return -1;
    if (fs.sectors_per_cluster == 0)            return -1;
    if (fs.reserved_sectors == 0)               return -1;
    if (fs.num_fats == 0)                       return -1;
    if (fs.fat_size_sectors == 0)               return -1; /* FAT32 */
    if (fs.root_entries == 0)                   return -1; /* FAT32 */

    fs.total_sectors = small_total ? small_total : big_total;
    if (fs.total_sectors == 0) return -1;

    fs.fat_lba          = fs.reserved_sectors;
    fs.root_dir_lba     = fs.fat_lba + (uint32_t)fs.num_fats * fs.fat_size_sectors;
    fs.root_dir_sectors = (fs.root_entries * DIRENT_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
    fs.data_lba         = fs.root_dir_lba + fs.root_dir_sectors;

    /* The FAT type is defined strictly by the cluster count, not by
     * any string in the BPB. FAT16 range: [4085, 65525). */
    uint32_t data_sectors    = fs.total_sectors - fs.data_lba;
    uint32_t total_clusters  = data_sectors / fs.sectors_per_cluster;
    if (total_clusters < 4085 || total_clusters >= 65525) return -1;

    fs_ready = true;
    return 0;
}

bool fat_present(void) { return fs_ready; }

/* ---------------------------------------------------------------- */
/* Name conversion                                                  */
/* ---------------------------------------------------------------- */

static char upcase(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/*
 * Pack a path component into the 11-byte on-disk form.
 *   "readme.txt"  -> "README  TXT"
 *   "foo"         -> "FOO        "
 * Returns true on success; false if the component is too long or
 * contains a forbidden character.
 */
static bool name_to_83(const char *in, char *out11) {
    for (int i = 0; i < 11; i++) out11[i] = ' ';

    int i = 0;
    while (*in && *in != '.') {
        if (i >= 8) return false;
        char c = *in++;
        /* Spaces are invalid in 8.3 names — caller falls back to LFN. */
        if (c == ' ') return false;
        out11[i++] = upcase(c);
    }
    if (*in == '.') {
        in++;
        int j = 0;
        while (*in) {
            char c = *in++;
            /* A second '.' or trailing space rules out plain 8.3. */
            if (c == '.' || c == ' ') return false;
            if (j >= 3) return false;
            out11[8 + j++] = upcase(c);
        }
    }
    return true;
}

/*
 * Render an 11-byte on-disk name as "readme.txt" / "READMEX.TXT".
 * `out` must hold at least 13 bytes. Trailing spaces are trimmed
 * from base and ext.
 *
 * `nt_flags` is the byte at dirent offset 0x0C (the "NTReserved"
 * field repurposed by Windows 95+ to store case info):
 *   bit 0x08 = base is lowercase
 *   bit 0x10 = ext  is lowercase
 * mtools (mformat/mcopy) sets these when copying a name like
 * "hello" into a SFN-only slot. Without honoring them, readdir
 * returns "HELLO" and case-sensitive matchers (our glob, strcmp
 * lookups) miss the file even though it's there.
 */
static void name_from_83_ext(const uint8_t *raw, uint8_t nt_flags, char *out) {
    int base_lower = (nt_flags & 0x08) != 0;
    int ext_lower  = (nt_flags & 0x10) != 0;
    int o = 0;
    int base_end = 8;
    while (base_end > 0 && raw[base_end - 1] == ' ') base_end--;
    for (int i = 0; i < base_end; i++) {
        char c = (char)raw[i];
        if (base_lower && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[o++] = c;
    }

    int ext_end = 11;
    while (ext_end > 8 && raw[ext_end - 1] == ' ') ext_end--;
    if (ext_end > 8) {
        out[o++] = '.';
        for (int i = 8; i < ext_end; i++) {
            char c = (char)raw[i];
            if (ext_lower && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[o++] = c;
        }
    }
    out[o] = 0;
}

/* Legacy wrapper used where the NT case-flags aren't available
 * (mostly path-lookup code paths that already upcase before
 * comparing). Defaults to all-uppercase. */
static void name_from_83(const uint8_t *raw, char *out) {
    name_from_83_ext(raw, 0, out);
}

static void parse_dirent(const uint8_t *raw, uint32_t lba, uint32_t off,
                          fat_dirent_t *out) {
    /* Byte 0x0C carries the NT case-flags written by mtools/Windows
     * — without honouring them, lowercase short names like `hello`
     * come back as `HELLO` and case-sensitive matchers miss them. */
    uint8_t nt_flags = raw[0x0C];
    name_from_83_ext(raw, nt_flags, out->name);
    /* short_name keeps the on-disk uppercase form for path lookups
     * that already upcase their query. */
    name_from_83(raw, out->short_name);
    out->is_dir        = (raw[11] & ATTR_DIR) != 0;
    out->first_cluster = rd16(raw + 26);
    out->size          = rd32(raw + 28);
    out->dirent_lba    = lba;
    out->dirent_offset = off;
}

static bool dirent_is_valid(const uint8_t *e) {
    if (e[0] == 0xE5) return false;                          /* deleted */
    if ((e[11] & ATTR_LFN) == ATTR_LFN) return false;        /* LFN slot */
    if (e[11] & ATTR_VOL) return false;                      /* volume label */
    return true;
}

/* ================================================================ */
/* LFN (Long File Name) support — Microsoft extension to FAT        */
/* ================================================================ */
/*
 * Each LFN slot holds 13 UCS-2 chars distributed across non-contiguous
 * byte offsets (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30). Slots
 * appear in REVERSE order on disk: the first slot encountered when
 * scanning the dir forward has the highest sequence number, ORed with
 * 0x40 to mark "last-in-chain". Slots count down to seq=1, which sits
 * immediately before the 8.3 dirent the LFN names. Every slot carries
 * an 8-bit checksum derived from the 8.3 short name; legacy OSs that
 * don't understand LFN see only the 8.3 alias.
 */

#define LFN_SEQ_LAST          0x40
#define LFN_SEQ_MASK          0x1F
#define LFN_CHARS_PER_SLOT    13
/* (FAT_NAME_MAX-1)/13 rounded up = 5; gives us 65 char headroom for a
 * 63-char name (+ NUL). LFN actually allows up to 20 slots / 260 chars
 * but we truncate at the VFS layer cap anyway. */
#define LFN_MAX_SEQ           5

/* Byte offsets inside a 32-byte LFN slot where the 13 UCS-2 chars
 * live. Each occupies two bytes (LE). */
static const uint8_t lfn_char_byte_offsets[LFN_CHARS_PER_SLOT] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

/* The "rotate-right-add" checksum every LFN slot carries at byte 13.
 * Recomputed from the 11-byte 8.3 name; mismatch invalidates the LFN
 * sequence and the reader falls back to the short name. */
static uint8_t lfn_checksum_11(const uint8_t name11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) | (sum >> 1)) + name11[i];
    }
    return sum;
}

/* Pull up to 13 ASCII chars out of an LFN slot. Stops at 0x0000 (NUL)
 * or 0xFFFF (padding). UCS-2 with high byte != 0 degrades to '?' so
 * the name is still printable. Returns the number of chars written. */
static int lfn_extract_chars(const uint8_t slot[DIRENT_SIZE], char out[LFN_CHARS_PER_SLOT]) {
    int count = 0;
    for (int i = 0; i < LFN_CHARS_PER_SLOT; i++) {
        uint8_t lo = slot[lfn_char_byte_offsets[i]];
        uint8_t hi = slot[lfn_char_byte_offsets[i] + 1];
        if (lo == 0x00 && hi == 0x00) break;
        if (lo == 0xFF && hi == 0xFF) break;
        out[count++] = (hi != 0) ? '?' : (char)lo;
    }
    return count;
}

/*
 * LFN accumulator. fat_readdir keeps one of these on the stack; it
 * collects partial slots as the dir is walked forward and is finalised
 * against the trailing 8.3 entry's checksum.
 */
typedef struct {
    bool     active;            /* set after seeing a slot with 0x40 */
    uint8_t  total_slots;
    uint8_t  next_seq;          /* next sequence number we expect to see */
    uint8_t  checksum;
    char     name[FAT_NAME_MAX];
} lfn_accum_t;

static void lfn_accum_reset(lfn_accum_t *a) {
    a->active = false;
    a->total_slots = 0;
    a->next_seq = 0;
    a->checksum = 0;
    a->name[0] = 0;
}

/* Feed one LFN slot into the accumulator. Returns true on a clean
 * merge, false if the slot is orphan / corrupted (accumulator is reset
 * in that case so the caller can keep scanning). */
static bool lfn_accum_consume(lfn_accum_t *a, const uint8_t slot[DIRENT_SIZE]) {
    uint8_t seq_byte = slot[0];
    uint8_t seq      = seq_byte & LFN_SEQ_MASK;
    bool    is_last  = (seq_byte & LFN_SEQ_LAST) != 0;
    uint8_t cksum    = slot[13];

    if (is_last) {
        lfn_accum_reset(a);
        if (seq == 0 || seq > LFN_MAX_SEQ) return false;
        a->active      = true;
        a->total_slots = seq;
        a->next_seq    = seq;
        a->checksum    = cksum;
    } else {
        if (!a->active) return false;                         /* orphan */
        if (cksum != a->checksum) { lfn_accum_reset(a); return false; }
        if (seq   != a->next_seq) { lfn_accum_reset(a); return false; }
    }

    char chunk[LFN_CHARS_PER_SLOT];
    int  n = lfn_extract_chars(slot, chunk);

    size_t base = (size_t)(seq - 1) * LFN_CHARS_PER_SLOT;
    for (int i = 0; i < n; i++) {
        if (base + (size_t)i < FAT_NAME_MAX - 1) {
            a->name[base + (size_t)i] = chunk[i];
        }
    }

    /* The 0x40-tagged slot is where the name ENDS. Place a NUL right
     * after the chars we extracted from it. */
    if (is_last) {
        size_t end = base + (size_t)n;
        if (end > FAT_NAME_MAX - 1) end = FAT_NAME_MAX - 1;
        a->name[end] = 0;
    }

    a->next_seq--;
    return true;
}

/* After an 8.3 dirent is seen, decide whether the preceding LFN
 * sequence is well-formed AND checksum-matches the 8.3 name. Returns
 * true and copies the long name into out_name; false otherwise. */
static bool lfn_accum_finalize(const lfn_accum_t *a,
                                const uint8_t name11[11],
                                char *out_name) {
    if (!a->active)                                 return false;
    if (a->next_seq != 0)                           return false;
    if (a->checksum != lfn_checksum_11(name11))     return false;

    for (size_t i = 0; i < FAT_NAME_MAX; i++) {
        out_name[i] = a->name[i];
        if (a->name[i] == 0) break;
    }
    return true;
}

/* Case-insensitive ASCII compare. Used by fat_lookup so users can type
 * "myFile.txt" and match a stored "MYFILE.TXT" (8.3) or a stored
 * "MyFile.TXT" (LFN). */
static bool name_iequal(const char *a, const char *b) {
    while (*a && *b) {
        if (upcase(*a) != upcase(*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ---------------------------------------------------------------- */
/* Directory iteration                                              */
/* ---------------------------------------------------------------- */

/*
 * The root dir lives in a fixed-size region right after the FAT(s).
 * The "global linear entry index" (cursor) covers exactly
 * root_dir_sectors * (SECTOR_SIZE/DIRENT_SIZE) slots.
 *
 * A non-root dir's storage is a cluster chain whose contents are an
 * array of dirent slots. We linearise the chain so the cursor stays a
 * plain integer.
 */

#define ENTRIES_PER_SECTOR  (SECTOR_SIZE / DIRENT_SIZE)

/*
 * Locate slot `idx` in `dir` and copy its raw 32 bytes. Does NOT
 * interpret the marker byte — callers decide.
 *   0 → slot exists; lba/off filled
 *   1 → idx is past the directory's allocated storage (root region
 *       exhausted, or cluster chain truly ended). Caller may then
 *       choose to extend the dir.
 *  -1 → I/O error
 */
static int dir_slot_locate(const fat_dirent_t *dir, uint32_t idx,
                            uint8_t slot_out[DIRENT_SIZE],
                            uint32_t *lba_out, uint32_t *off_out) {
    uint8_t buf[SECTOR_SIZE];

    if (dir->first_cluster == 0) {
        uint32_t total = fs.root_dir_sectors * ENTRIES_PER_SECTOR;
        if (idx >= total) return 1;

        uint32_t s = idx / ENTRIES_PER_SECTOR;
        uint32_t i = idx % ENTRIES_PER_SECTOR;
        uint32_t lba = fs.root_dir_lba + s;
        if (block_ata_read_sector(lba, buf) != 0) return -1;

        uint8_t *e = buf + i * DIRENT_SIZE;
        for (uint32_t k = 0; k < DIRENT_SIZE; k++) slot_out[k] = e[k];
        *lba_out = lba;
        *off_out = i * DIRENT_SIZE;
        return 0;
    }

    uint32_t entries_per_cluster = fs.sectors_per_cluster * ENTRIES_PER_SECTOR;
    uint32_t cluster_idx = idx / entries_per_cluster;
    uint32_t in_cluster  = idx % entries_per_cluster;

    uint16_t cluster = dir->first_cluster;
    for (uint32_t c = 0; c < cluster_idx; c++) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) return 1;
        uint16_t next;
        if (fat_get_entry(cluster, &next) != 0) return -1;
        cluster = next;
    }
    if (cluster < 2 || cluster >= FAT16_EOF_MIN) return 1;

    uint32_t s = in_cluster / ENTRIES_PER_SECTOR;
    uint32_t i = in_cluster % ENTRIES_PER_SECTOR;
    uint32_t lba = cluster_to_lba(cluster) + s;
    if (block_ata_read_sector(lba, buf) != 0) return -1;
    uint8_t *e = buf + i * DIRENT_SIZE;
    for (uint32_t k = 0; k < DIRENT_SIZE; k++) slot_out[k] = e[k];
    *lba_out = lba;
    *off_out = i * DIRENT_SIZE;
    return 0;
}

/* read_dir_slot: same as locate, plus 0x00 first-byte → 1 (end-of-dir).
 * This is what iteration / lookup want — bail at the first 0x00 marker
 * because everything past it is guaranteed unused. */
static int read_dir_slot(const fat_dirent_t *dir, uint32_t idx,
                          uint8_t slot_out[DIRENT_SIZE],
                          uint32_t *lba_out, uint32_t *off_out) {
    int rc = dir_slot_locate(dir, idx, slot_out, lba_out, off_out);
    if (rc != 0) return rc;
    if (slot_out[0] == 0x00) return 1;
    return 0;
}

int fat_readdir(const fat_dirent_t *dir, uint32_t cursor,
                fat_dirent_t *out, uint32_t *next_cursor) {
    if (!fs_ready) return -1;
    if (!dir->is_dir) return -1;

    lfn_accum_t accum;
    lfn_accum_reset(&accum);

    uint32_t idx = cursor;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = read_dir_slot(dir, idx, slot, &lba, &off);
        if (rc == 1) return -1;          /* end of directory */
        if (rc < 0)  return -1;
        idx++;

        if (slot[0] == 0xE5) {            /* deleted; reset any in-flight LFN */
            lfn_accum_reset(&accum);
            continue;
        }
        if ((slot[11] & ATTR_LFN) == ATTR_LFN) {
            /* Accumulator handles orphan / restart internally. */
            lfn_accum_consume(&accum, slot);
            continue;
        }
        if (slot[11] & ATTR_VOL) {        /* volume label */
            lfn_accum_reset(&accum);
            continue;
        }

        /* Regular 8.3 dirent. parse_dirent fills out->name with the
         * short name; if a valid LFN sequence with matching checksum
         * preceded, overwrite with the decoded long name. */
        parse_dirent(slot, lba, off, out);

        char long_name[FAT_NAME_MAX];
        if (lfn_accum_finalize(&accum, slot, long_name)) {
            size_t i;
            for (i = 0; i + 1 < FAT_NAME_MAX && long_name[i]; i++) {
                out->name[i] = long_name[i];
            }
            out->name[i] = 0;
        }
        *next_cursor = idx;
        return 0;
    }
}

/* ---------------------------------------------------------------- */
/* Path lookup                                                      */
/* ---------------------------------------------------------------- */

/*
 * Match path components against both the LFN long name and the 8.3
 * alias (case-insensitive). Walks via fat_readdir so LFN sequences are
 * already collapsed into a single entry with its display name set.
 */
static int dir_find_by_name(const fat_dirent_t *dir, const char *want,
                             fat_dirent_t *out) {
    uint32_t cursor = 0;
    fat_dirent_t ent;
    uint32_t next;
    while (fat_readdir(dir, cursor, &ent, &next) == 0) {
        cursor = next;
        /* Match against the LFN long name OR the 8.3 alias. Windows
         * and POSIX-on-FAT both allow either form, and our fsck/test
         * paths rely on the alias being reachable. */
        if (name_iequal(ent.name, want))       { *out = ent; return 0; }
        if (name_iequal(ent.short_name, want)) { *out = ent; return 0; }
    }
    return -1;
}

static void root_sentinel(fat_dirent_t *out) {
    out->name[0] = '/';
    out->name[1] = 0;
    out->is_dir = true;
    out->size = 0;
    out->first_cluster = 0;
    out->dirent_lba = 0;
    out->dirent_offset = 0;
}

int fat_lookup(const char *path, fat_dirent_t *out) {
    if (!fs_ready) return -1;
    if (!path)     return -1;

    while (*path == '/') path++;
    if (*path == 0) {
        root_sentinel(out);
        return 0;
    }

    fat_dirent_t cur;
    root_sentinel(&cur);

    while (*path) {
        char comp[FAT_NAME_MAX];
        size_t n = 0;
        while (*path && *path != '/' && n + 1 < sizeof(comp)) {
            comp[n++] = *path++;
        }
        comp[n] = 0;
        while (*path == '/') path++;
        if (n == 0) continue;

        if (!cur.is_dir) return -1;

        /* dir_find_by_name matches case-insensitively against either
         * the LFN long name (if present) or the 8.3 alias, so users
         * can `cat` files by their original capitalised name. */
        fat_dirent_t next;
        if (dir_find_by_name(&cur, comp, &next) != 0) return -1;
        cur = next;
    }

    *out = cur;
    return 0;
}

/* ---------------------------------------------------------------- */
/* File read                                                        */
/* ---------------------------------------------------------------- */

int fat_read_file(const fat_dirent_t *de, uint32_t off,
                  char *buf, uint32_t len) {
    if (!fs_ready) return -1;
    if (de->is_dir) return -1;
    if (off >= de->size) return 0;
    if (len > de->size - off) len = de->size - off;
    if (len == 0) return 0;

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * SECTOR_SIZE;
    uint32_t clusters_to_skip = off / cluster_size;
    uint32_t in_cluster_off   = off % cluster_size;

    uint16_t cluster = de->first_cluster;
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) return -1;
        uint16_t next;
        if (fat_get_entry(cluster, &next) != 0) return -1;
        cluster = next;
    }

    uint32_t total = 0;
    uint8_t  sbuf[SECTOR_SIZE];

    while (total < len) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) break;

        uint32_t lba_start = cluster_to_lba(cluster);
        uint32_t sec_in    = in_cluster_off / SECTOR_SIZE;
        uint32_t sec_off   = in_cluster_off % SECTOR_SIZE;

        while (total < len && sec_in < fs.sectors_per_cluster) {
            if (block_ata_read_sector(lba_start + sec_in, sbuf) != 0)
                return -1;
            uint32_t chunk = SECTOR_SIZE - sec_off;
            if (chunk > len - total) chunk = len - total;
            for (uint32_t i = 0; i < chunk; i++) {
                buf[total + i] = (char)sbuf[sec_off + i];
            }
            total += chunk;
            sec_off = 0;
            sec_in++;
        }
        in_cluster_off = 0;

        if (total < len) {
            uint16_t next;
            if (fat_get_entry(cluster, &next) != 0) return -1;
            cluster = next;
        }
    }

    return (int)total;
}

/* ================================================================ */
/* Write side (FASE 8.4)                                            */
/* ================================================================ */

/* errno-style returns; sign-flipped to fit the int convention. */
#define FAT_OK         0
#define FAT_EIO        -5
#define FAT_ENOENT     -2
#define FAT_EEXIST     -17
#define FAT_ENOTDIR    -20
#define FAT_EISDIR     -21
#define FAT_EINVAL     -22
#define FAT_ENOSPC     -28
#define FAT_ENOTEMPTY  -39

/*
 * Write `value` into FAT entry for `cluster`, replicating across every
 * FAT copy (mirror). Without the mirror update, the on-disk image
 * fails fsck and any OS that mounts the second FAT first sees stale
 * chains.
 */
static int fat_set_entry(uint16_t cluster, uint16_t value) {
    uint8_t buf[SECTOR_SIZE];
    for (uint8_t fat_idx = 0; fat_idx < fs.num_fats; fat_idx++) {
        uint32_t fat_base = fs.fat_lba +
                            (uint32_t)fat_idx * fs.fat_size_sectors;
        uint32_t byte_off = (uint32_t)cluster * 2;
        uint32_t sec      = fat_base + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (block_ata_read_sector(sec, buf) != 0) return -1;
        buf[in_sec]     = (uint8_t)(value);
        buf[in_sec + 1] = (uint8_t)(value >> 8);
        if (block_ata_write_sector(sec, buf) != 0) return -1;
    }
    return 0;
}

/* Find first free FAT entry (== 0) starting at cluster 2 and claim it
 * by marking it as EOF. Returns the cluster number, or 0 if disk full. */
static uint16_t fat_alloc_cluster(void) {
    uint32_t max_entries = (uint32_t)fs.fat_size_sectors * SECTOR_SIZE / 2;
    if (max_entries > 65525) max_entries = 65525;

    uint8_t buf[SECTOR_SIZE];
    uint32_t cached_sec = (uint32_t)-1;

    for (uint32_t c = 2; c < max_entries; c++) {
        uint32_t byte_off = c * 2;
        uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (sec != cached_sec) {
            if (block_ata_read_sector(sec, buf) != 0) return 0;
            cached_sec = sec;
        }
        uint16_t val = (uint16_t)buf[in_sec] | ((uint16_t)buf[in_sec + 1] << 8);
        if (val == 0) {
            if (fat_set_entry((uint16_t)c, 0xFFFF) != 0) return 0;
            return (uint16_t)c;
        }
    }
    return 0;
}

/* Walk chain, mark every entry as free (0). Stops at EOF or invalid. */
static int fat_free_chain(uint16_t first) {
    uint16_t cur = first;
    while (cur >= 2 && cur < FAT16_EOF_MIN) {
        uint16_t next;
        if (fat_get_entry(cur, &next) != 0) return -1;
        if (fat_set_entry(cur, 0) != 0) return -1;
        cur = next;
    }
    return 0;
}

/* Allocate a chain of `count` linked clusters. *first_out gets the head.
 * On any failure frees what was allocated so far. count==0 → first=0. */
static int fat_alloc_chain(uint32_t count, uint16_t *first_out) {
    *first_out = 0;
    if (count == 0) return 0;

    uint16_t first = 0;
    uint16_t prev  = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint16_t c = fat_alloc_cluster();
        if (c == 0) {
            if (first != 0) fat_free_chain(first);
            return FAT_ENOSPC;
        }
        if (first == 0) {
            first = c;
        } else if (fat_set_entry(prev, c) != 0) {
            fat_free_chain(first);
            return FAT_EIO;
        }
        prev = c;
    }
    *first_out = first;
    return 0;
}

/* Copy `len` bytes from `src` into the cluster chain starting at
 * `first`, padding the final sector with zeros. Caller pre-allocated
 * enough clusters. */
static int write_data_into_chain(uint16_t first, const char *src, uint32_t len) {
    uint16_t cluster = first;
    uint32_t written = 0;
    uint8_t  sbuf[SECTOR_SIZE];

    while (written < len) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) return FAT_EIO;

        uint32_t lba_start = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < fs.sectors_per_cluster && written < len; s++) {
            uint32_t chunk = SECTOR_SIZE;
            if (chunk > len - written) chunk = len - written;

            for (uint32_t k = 0; k < chunk; k++) {
                sbuf[k] = (uint8_t)src[written + k];
            }
            for (uint32_t k = chunk; k < SECTOR_SIZE; k++) sbuf[k] = 0;

            if (block_ata_write_sector(lba_start + s, sbuf) != 0) return FAT_EIO;
            written += chunk;
        }

        if (written < len) {
            uint16_t next;
            if (fat_get_entry(cluster, &next) != 0) return FAT_EIO;
            cluster = next;
        }
    }
    return 0;
}

/* ----- dirent slot writes ----- */

static int read_modify_write_dirent(uint32_t lba, uint32_t off,
                                     const uint8_t raw[DIRENT_SIZE]) {
    uint8_t buf[SECTOR_SIZE];
    if (block_ata_read_sector(lba, buf) != 0) return FAT_EIO;
    for (uint32_t i = 0; i < DIRENT_SIZE; i++) buf[off + i] = raw[i];
    if (block_ata_write_sector(lba, buf) != 0) return FAT_EIO;
    return 0;
}

static void build_dirent_raw(uint8_t raw[DIRENT_SIZE],
                              const char *name11, uint8_t attr,
                              uint16_t first_cluster, uint32_t size) {
    for (int i = 0; i < DIRENT_SIZE; i++) raw[i] = 0;
    for (int i = 0; i < 11; i++) raw[i] = (uint8_t)name11[i];
    raw[11] = attr;
    /* 12..21 left zero — case, create-time, last-access-date.
     * 22..25 left zero — write time/date (we have no clock yet). */
    raw[26] = (uint8_t)(first_cluster);
    raw[27] = (uint8_t)(first_cluster >> 8);
    raw[28] = (uint8_t)(size);
    raw[29] = (uint8_t)(size >> 8);
    raw[30] = (uint8_t)(size >> 16);
    raw[31] = (uint8_t)(size >> 24);
}

/* Resolve `parent_path` (which may end with '/') and split out the
 * last component as `base_out`. Examples:
 *   "/foo/bar"  → parent dir from path "/foo", base "bar"
 *   "/bar"      → root, base "bar"
 *   "/"         → invalid (no base)
 */
static int split_parent_base(const char *path,
                              char parent_path[64],
                              char base_out[64]) {
    /* Skip leading slashes. */
    while (*path == '/') path++;
    if (*path == 0) return FAT_EINVAL;

    /* Find last '/' in the remainder. */
    const char *last_slash = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        parent_path[0] = '/';
        parent_path[1] = 0;
        int i;
        for (i = 0; i < 62 && path[i]; i++) base_out[i] = path[i];
        base_out[i] = 0;
        return base_out[0] ? 0 : FAT_EINVAL;
    }

    /* parent = "/" + path[..last_slash) */
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len + 2 > 64) return FAT_EINVAL;
    parent_path[0] = '/';
    for (size_t i = 0; i < parent_len; i++) parent_path[1 + i] = path[i];
    parent_path[1 + parent_len] = 0;

    const char *base = last_slash + 1;
    int i;
    for (i = 0; i < 62 && base[i]; i++) base_out[i] = base[i];
    base_out[i] = 0;
    return base_out[0] ? 0 : FAT_EINVAL;
}

/* ----- LFN write helpers ----- */

/* Mark the dirent at `idx` (linear slot index) within `dir` as deleted
 * (0xE5 first byte). */
static int delete_slot_by_idx(const fat_dirent_t *dir, uint32_t idx) {
    uint8_t slot[DIRENT_SIZE];
    uint32_t lba, off;
    if (dir_slot_locate(dir, idx, slot, &lba, &off) != 0) return -1;

    uint8_t sb[SECTOR_SIZE];
    if (block_ata_read_sector(lba, sb) != 0) return -1;
    sb[off] = 0xE5;
    if (block_ata_write_sector(lba, sb) != 0) return -1;
    return 0;
}

/*
 * Walk `parent` forward looking for the 8.3 dirent that physically
 * lives at (target_lba, target_off). On success returns its linear
 * slot index plus the count of immediately-preceding LFN slots that
 * carry the matching checksum. Stops at end-of-directory storage
 * (rc=1 from dir_slot_locate) — a missing target is an FS-level
 * inconsistency.
 */
static int locate_target_and_lfn(const fat_dirent_t *parent,
                                   uint32_t target_lba, uint32_t target_off,
                                   uint32_t *target_idx_out,
                                   uint32_t *lfn_count_out) {
    uint32_t idx = 0;
    uint32_t lfn_count = 0;
    bool     lfn_active = false;
    uint8_t  lfn_cksum  = 0;

    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = dir_slot_locate(parent, idx, slot, &lba, &off);
        if (rc < 0)  return -1;
        if (rc == 1) return -1;

        if (lba == target_lba && off == target_off) {
            *target_idx_out = idx;
            if (lfn_active && lfn_cksum == lfn_checksum_11(slot)) {
                *lfn_count_out = lfn_count;
            } else {
                *lfn_count_out = 0;
            }
            return 0;
        }

        idx++;

        if (slot[0] == 0xE5) {
            lfn_active = false;
            lfn_count = 0;
            continue;
        }
        if ((slot[11] & ATTR_LFN) == ATTR_LFN) {
            uint8_t sb0 = slot[0];
            bool is_last = (sb0 & LFN_SEQ_LAST) != 0;
            uint8_t cs = slot[13];
            if (is_last) {
                lfn_active = true;
                lfn_count = 1;
                lfn_cksum = cs;
            } else if (lfn_active && cs == lfn_cksum) {
                lfn_count++;
            } else {
                lfn_active = false;
                lfn_count = 0;
            }
            continue;
        }
        if (slot[11] & ATTR_VOL) {
            lfn_active = false;
            lfn_count = 0;
            continue;
        }
        /* Some other valid 8.3, not our target. */
        lfn_active = false;
        lfn_count = 0;
    }
}

/* Find `count` consecutive free slots (0xE5 or 0x00) in `dir`. Returns
 * 0 on success with lba_arr / off_arr filled in slot order. FAT_ENOSPC
 * if the directory has no run that long before its storage ends. */
static int find_free_dir_slots_run(const fat_dirent_t *dir,
                                     uint32_t count,
                                     uint32_t *lba_arr,
                                     uint32_t *off_arr) {
    uint32_t idx = 0;
    uint32_t run = 0;

    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = dir_slot_locate(dir, idx, slot, &lba, &off);
        if (rc < 0)  return FAT_EIO;
        if (rc == 1) return FAT_ENOSPC;

        if (slot[0] == 0xE5 || slot[0] == 0x00) {
            lba_arr[run] = lba;
            off_arr[run] = off;
            run++;
            if (run == count) return 0;
        } else {
            run = 0;
        }
        idx++;
    }
}

/* Check if an exact 8.3 name already exists in `dir`. Used during 8.3
 * alias generation to detect ~N collisions. */
static bool dir_has_8_3(const fat_dirent_t *dir, const char alias11[11]) {
    uint32_t idx = 0;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = read_dir_slot(dir, idx, slot, &lba, &off);
        if (rc != 0) return false;
        idx++;
        if (!dirent_is_valid(slot)) continue;
        bool match = true;
        for (int i = 0; i < 11; i++) {
            if (slot[i] != (uint8_t)alias11[i]) { match = false; break; }
        }
        if (match) return true;
    }
}

/* Generate an 8.3 alias for `long_name`, packed into `alias11` (no NUL,
 * space-padded). Walks N=1..99 picking the first BASE~N.EXT that
 * doesn't collide in `parent`. Returns FAT_ENOSPC if 99 attempts fail. */
static int gen_short_alias(const fat_dirent_t *parent,
                             const char *long_name,
                             char alias11[11]) {
    size_t total = 0;
    while (long_name[total]) total++;

    /* Find LAST '.' for ext split. */
    const char *ext = 0;
    for (size_t i = total; i > 0; i--) {
        if (long_name[i - 1] == '.') { ext = long_name + i; break; }
    }
    size_t base_len = ext ? (size_t)((ext - 1) - long_name) : total;
    size_t ext_len  = ext ? total - (size_t)(ext - long_name) : 0;

    /* Filter into uppercase alphanumeric. Non-alphanumerics other than
     * spaces / dots become '_'. Spaces and dots are dropped entirely. */
    char base_filtered[6];
    int  base_pos = 0;
    for (size_t i = 0; i < base_len && base_pos < 6; i++) {
        char c = long_name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        base_filtered[base_pos++] = c;
    }
    if (base_pos == 0) {
        base_filtered[0] = 'F';
        base_pos = 1;
    }

    char ext_filtered[3];
    int  ext_pos = 0;
    if (ext) {
        for (size_t i = 0; i < ext_len && ext_pos < 3; i++) {
            char c = ext[i];
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
            ext_filtered[ext_pos++] = c;
        }
    }

    for (int n = 1; n <= 99; n++) {
        int suffix_digits = (n < 10) ? 1 : 2;
        int base_keep = 6 - suffix_digits;       /* room for '~' + digits */
        if (base_keep > base_pos) base_keep = base_pos;

        for (int i = 0; i < 11; i++) alias11[i] = ' ';
        int pos = 0;
        for (int i = 0; i < base_keep; i++) alias11[pos++] = base_filtered[i];
        alias11[pos++] = '~';
        if (suffix_digits == 2) alias11[pos++] = (char)('0' + (n / 10));
        alias11[pos++] = (char)('0' + (n % 10));
        for (int i = 0; i < ext_pos; i++) alias11[8 + i] = ext_filtered[i];

        if (!dir_has_8_3(parent, alias11)) return 0;
    }
    return FAT_ENOSPC;
}

/* Pack one LFN slot. `name_total_len` is the strlen of the full long
 * name; `seq` is 1-based and identifies which 13-char chunk this slot
 * carries. `is_last` (the highest-seq slot) gets the 0x40 marker and
 * sees its chars NUL-terminated + 0xFFFF padded as needed. */
static void build_lfn_slot(uint8_t out[DIRENT_SIZE],
                             const char *long_name,
                             size_t name_total_len,
                             uint8_t seq,
                             bool is_last,
                             uint8_t cksum) {
    for (int i = 0; i < DIRENT_SIZE; i++) out[i] = 0;
    out[0]  = (uint8_t)(seq | (is_last ? LFN_SEQ_LAST : 0));
    out[11] = ATTR_LFN;
    out[12] = 0;
    out[13] = cksum;
    out[26] = 0;
    out[27] = 0;

    size_t base = (size_t)(seq - 1) * LFN_CHARS_PER_SLOT;
    bool   past_nul = false;
    for (int i = 0; i < LFN_CHARS_PER_SLOT; i++) {
        uint8_t bo = lfn_char_byte_offsets[i];
        size_t  name_idx = base + (size_t)i;
        uint16_t ch;
        if (past_nul) {
            ch = 0xFFFF;
        } else if (name_idx < name_total_len) {
            ch = (uint8_t)long_name[name_idx];
        } else if (name_idx == name_total_len) {
            ch = 0x0000;
            past_nul = true;
        } else {
            ch = 0xFFFF;
            past_nul = true;
        }
        out[bo]     = (uint8_t)ch;
        out[bo + 1] = (uint8_t)(ch >> 8);
    }
}

/*
 * Naming / layout info for a new entry. Built by compute_entry_naming
 * from the long name + parent dir. write_entry_with_naming consumes it
 * along with a prebuilt 32-byte 8.3 dirent (with first_cluster, size,
 * attr already set) to do the actual on-disk install.
 */
typedef struct {
    char     alias11[11];      /* on-disk 8.3 name bytes */
    bool     needs_lfn;
    char     long_name[FAT_NAME_MAX];
    uint32_t n_lfn_slots;      /* zero when needs_lfn==false */
} entry_naming_t;

static int compute_entry_naming(const fat_dirent_t *parent,
                                  const char *base,
                                  entry_naming_t *out) {
    out->needs_lfn = false;
    out->n_lfn_slots = 0;
    out->long_name[0] = 0;

    if (name_to_83(base, out->alias11)) return 0;

    /* Falls back to LFN + generated alias. */
    size_t base_len = 0;
    while (base[base_len]) base_len++;
    if (base_len == 0 || base_len > FAT_NAME_MAX - 1) return FAT_EINVAL;
    for (size_t i = 0; i < base_len; i++) out->long_name[i] = base[i];
    out->long_name[base_len] = 0;

    int rc = gen_short_alias(parent, out->long_name, out->alias11);
    if (rc != 0) return rc;

    out->needs_lfn = true;
    out->n_lfn_slots = (uint32_t)((base_len + LFN_CHARS_PER_SLOT - 1)
                                    / LFN_CHARS_PER_SLOT);
    if (out->n_lfn_slots > LFN_MAX_SEQ) return FAT_EINVAL;
    return 0;
}

/*
 * Install a new entry described by `naming` (with the 32-byte 8.3
 * payload already built by the caller — typically via build_dirent_raw)
 * into `parent`. Reserves N+1 consecutive slots; writes LFN slots (in
 * reverse seq order on disk so the 0x40-marked highest-seq slot lands
 * first) then the 8.3 payload. Cleans up any partial state on failure.
 */
/* Allocate one fresh cluster and append it to `dir`'s existing chain.
 * Used to extend a subdir that ran out of slot space — fixes the
 * ENOSPC-after-~9-files limit we hit when seeding /sd/bin in
 * bootstrap. Not valid for the FAT12/16 root dir (first_cluster==0
 * → fixed-size root area). */
static int extend_dir_chain(const fat_dirent_t *dir) {
    if (dir->first_cluster == 0) return FAT_ENOSPC;

    uint16_t cur = dir->first_cluster;
    uint16_t next;
    for (;;) {
        if (cur < 2 || cur >= FAT16_EOF_MIN) return FAT_EIO;
        if (fat_get_entry(cur, &next) != 0)  return FAT_EIO;
        if (next >= FAT16_EOF_MIN) break;
        cur = next;
    }

    uint16_t new_c = fat_alloc_cluster();
    if (new_c == 0) return FAT_ENOSPC;

    /* Zero the new cluster so the first byte of every slot reads
     * 0x00 — that's how read_dir_slot detects "end of dir". */
    uint8_t zero[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) zero[i] = 0;
    uint32_t lba0 = cluster_to_lba(new_c);
    for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
        if (block_ata_write_sector(lba0 + s, zero) != 0) {
            return FAT_EIO;
        }
    }

    if (fat_set_entry(cur, new_c) != 0) return FAT_EIO;
    return 0;
}

static int write_entry_with_naming(const fat_dirent_t *parent,
                                     const entry_naming_t *naming,
                                     const uint8_t dirent_raw[DIRENT_SIZE]) {
    uint32_t total_slots = naming->needs_lfn ? (naming->n_lfn_slots + 1) : 1;
    uint32_t slot_lbas[LFN_MAX_SEQ + 1];
    uint32_t slot_offs[LFN_MAX_SEQ + 1];

    int rc = find_free_dir_slots_run(parent, total_slots, slot_lbas, slot_offs);
    if (rc == FAT_ENOSPC && parent->first_cluster != 0) {
        /* Subdir ran out of slot space — try growing the chain by one
         * cluster and looking again. Keep trying a few times in case
         * the slot run we need is larger than one cluster. */
        for (int attempt = 0; attempt < 4; attempt++) {
            int ext = extend_dir_chain(parent);
            if (ext != 0) return ext;
            rc = find_free_dir_slots_run(parent, total_slots, slot_lbas, slot_offs);
            if (rc != FAT_ENOSPC) break;
        }
    }
    if (rc != 0) return rc;

    if (naming->needs_lfn) {
        uint8_t cksum = lfn_checksum_11((const uint8_t *)naming->alias11);
        size_t  name_len = 0;
        while (naming->long_name[name_len]) name_len++;

        for (uint32_t i = 0; i < naming->n_lfn_slots; i++) {
            /* slot at index i takes seq = (n - i): the FIRST slot on
             * disk has the highest seq with 0x40 set. */
            uint8_t seq = (uint8_t)(naming->n_lfn_slots - i);
            bool    is_last = (i == 0);
            uint8_t raw[DIRENT_SIZE];
            build_lfn_slot(raw, naming->long_name, name_len, seq, is_last, cksum);

            if (read_modify_write_dirent(slot_lbas[i], slot_offs[i], raw) != 0) {
                /* Roll back partial LFN writes. */
                for (uint32_t j = 0; j < i; j++) {
                    uint8_t sb[SECTOR_SIZE];
                    if (block_ata_read_sector(slot_lbas[j], sb) == 0) {
                        sb[slot_offs[j]] = 0xE5;
                        block_ata_write_sector(slot_lbas[j], sb);
                    }
                }
                return FAT_EIO;
            }
        }
    }

    uint32_t last_lba = slot_lbas[total_slots - 1];
    uint32_t last_off = slot_offs[total_slots - 1];
    if (read_modify_write_dirent(last_lba, last_off, dirent_raw) != 0) {
        /* 8.3 write failed — undo any LFN slots. */
        if (naming->needs_lfn) {
            for (uint32_t j = 0; j < naming->n_lfn_slots; j++) {
                uint8_t sb[SECTOR_SIZE];
                if (block_ata_read_sector(slot_lbas[j], sb) == 0) {
                    sb[slot_offs[j]] = 0xE5;
                    block_ata_write_sector(slot_lbas[j], sb);
                }
            }
        }
        return FAT_EIO;
    }
    return 0;
}

/* ----- public write API ----- */

int fat_unlink_path(const char *path) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t de;
    if (fat_lookup(path, &de) != 0) return FAT_ENOENT;
    if (de.is_dir) return FAT_EISDIR;
    if (de.dirent_lba == 0) return FAT_EINVAL;       /* root sentinel */

    /* Locate the entry inside its parent so we know which (if any) LFN
     * slots precede it and need to be deleted together. */
    char parent_path[64];
    char base[64];
    if (split_parent_base(path, parent_path, base) != 0) return FAT_EINVAL;
    fat_dirent_t parent;
    if (fat_lookup(parent_path, &parent) != 0) return FAT_EIO;

    uint32_t target_idx = 0, lfn_count = 0;
    if (locate_target_and_lfn(&parent, de.dirent_lba, de.dirent_offset,
                                &target_idx, &lfn_count) != 0) {
        return FAT_EIO;
    }

    if (fat_free_chain(de.first_cluster) != 0) return FAT_EIO;

    /* Delete 8.3 first (file disappears from listing). A crash before
     * the LFN slots are cleared leaves only cosmetic orphan LFN slots
     * that fsck reports. */
    if (delete_slot_by_idx(&parent, target_idx) != 0) return FAT_EIO;
    for (uint32_t i = 0; i < lfn_count; i++) {
        delete_slot_by_idx(&parent, target_idx - lfn_count + i);
    }
    return 0;
}

/*
 * Common path for write/append: prepare `*existing` (whether the target
 * exists, and where its dirent lives). Returns FAT_OK and fills the
 * out params; on missing-path returns FAT_OK with `exists=false` and
 * the parent_dir/name11 fields populated for create.
 */
typedef struct {
    bool             exists;
    fat_dirent_t     de;            /* valid only when exists==true */
    fat_dirent_t     parent_dir;    /* valid in both cases */
    entry_naming_t   naming;        /* valid when exists==false */
} write_target_t;

static int resolve_write_target(const char *path, write_target_t *out) {
    fat_dirent_t de;
    if (fat_lookup(path, &de) == 0) {
        if (de.is_dir) return FAT_EISDIR;
        if (de.dirent_lba == 0) return FAT_EINVAL;
        out->exists     = true;
        out->de         = de;
        out->parent_dir.is_dir = true;
        out->parent_dir.first_cluster = 0;   /* unused on overwrite */
        return 0;
    }

    char parent_path[64];
    char base[64];
    int rc = split_parent_base(path, parent_path, base);
    if (rc != 0) return rc;

    if (fat_lookup(parent_path, &out->parent_dir) != 0) return FAT_ENOENT;
    if (!out->parent_dir.is_dir) return FAT_ENOTDIR;

    /* Compute naming: 8.3-only if name_to_83 succeeds, else LFN + alias. */
    rc = compute_entry_naming(&out->parent_dir, base, &out->naming);
    if (rc != 0) return rc;
    out->exists = false;
    return 0;
}

/* Write/replace file content. Strategy:
 *   1. Allocate the new chain (if any data).
 *   2. Write data into the chain.
 *   3. Update / create the dirent so it points at the new chain.
 *   4. Free the old chain (overwrite case only).
 * Crash between step 1 and step 3 leaks clusters but never corrupts
 * the visible filesystem.
 */
int fat_write_path(const char *path, const char *buf, uint32_t len) {
    if (!fs_ready) return FAT_EIO;

    write_target_t t;
    int rc = resolve_write_target(path, &t);
    if (rc != 0) return rc;

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * SECTOR_SIZE;
    uint32_t clusters_needed = (len + cluster_size - 1) / cluster_size;

    uint16_t new_chain = 0;
    rc = fat_alloc_chain(clusters_needed, &new_chain);
    if (rc != 0) return rc;

    if (len > 0) {
        rc = write_data_into_chain(new_chain, buf, len);
        if (rc != 0) {
            fat_free_chain(new_chain);
            return rc;
        }
    }

    if (t.exists) {
        uint16_t old_chain = t.de.first_cluster;
        uint8_t raw[DIRENT_SIZE];
        uint8_t sb[SECTOR_SIZE];
        if (block_ata_read_sector(t.de.dirent_lba, sb) != 0) {
            fat_free_chain(new_chain);
            return FAT_EIO;
        }
        for (uint32_t i = 0; i < DIRENT_SIZE; i++) {
            raw[i] = sb[t.de.dirent_offset + i];
        }
        raw[26] = (uint8_t)(new_chain);
        raw[27] = (uint8_t)(new_chain >> 8);
        raw[28] = (uint8_t)(len);
        raw[29] = (uint8_t)(len >> 8);
        raw[30] = (uint8_t)(len >> 16);
        raw[31] = (uint8_t)(len >> 24);
        rc = read_modify_write_dirent(t.de.dirent_lba, t.de.dirent_offset, raw);
        if (rc != 0) {
            fat_free_chain(new_chain);
            return rc;
        }
        if (old_chain >= 2 && old_chain < FAT16_EOF_MIN) {
            fat_free_chain(old_chain);
        }
        return 0;
    }

    /* Create a fresh dirent — LFN + 8.3 if the name didn't fit 8.3,
     * just 8.3 otherwise. write_entry_with_naming handles both. */
    uint8_t raw[DIRENT_SIZE];
    build_dirent_raw(raw, t.naming.alias11, ATTR_ARCH, new_chain, len);
    rc = write_entry_with_naming(&t.parent_dir, &t.naming, raw);
    if (rc != 0) {
        fat_free_chain(new_chain);
        return rc;
    }
    return 0;
}

/* Append: cheap implementation. Read existing → tack on → rewrite.
 * Bounded by the 8 KiB scratch buffer; bigger appends fail. Enough
 * for `echo … >>` use-cases. */
/* Heap-allocated append scratch: previously a static 8 KiB buffer
 * which silently capped any file at 8 KiB total — broke TCC writing
 * its ~50 KiB output ELF in 8 KiB chunks (second chunk returned
 * ENOSPC, TCC ignored it, the disk ELF was truncated mid-header).
 * Caller is sys_write which already bounds total at 4 MiB. */
#define FAT_APPEND_LIMIT (4 * 1024 * 1024)

int fat_append_path(const char *path, const char *buf, uint32_t len) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t de;
    int looked_up = fat_lookup(path, &de);

    uint32_t existing_size = 0;
    if (looked_up == 0) {
        if (de.is_dir) return FAT_EISDIR;
        existing_size = de.size;
    }
    uint64_t total = (uint64_t)existing_size + (uint64_t)len;
    if (total > FAT_APPEND_LIMIT) return FAT_ENOSPC;

    char *scratch = (char *)kmalloc(total > 0 ? (size_t)total : 1);
    if (!scratch) return FAT_EIO;

    if (existing_size > 0) {
        int n = fat_read_file(&de, 0, scratch, existing_size);
        if (n < 0 || (uint32_t)n != existing_size) {
            kfree(scratch);
            return FAT_EIO;
        }
    }
    for (uint32_t i = 0; i < len; i++) {
        scratch[existing_size + i] = buf[i];
    }
    int rc = fat_write_path(path, scratch, (uint32_t)total);
    kfree(scratch);
    return rc;
}

/* mkdir: create empty subdir cluster with "." and ".." entries. */
int fat_mkdir_path(const char *path) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t existing;
    if (fat_lookup(path, &existing) == 0) return FAT_EEXIST;

    char parent_path[64], base[64];
    int rc = split_parent_base(path, parent_path, base);
    if (rc != 0) return rc;

    fat_dirent_t parent;
    if (fat_lookup(parent_path, &parent) != 0) return FAT_ENOENT;
    if (!parent.is_dir) return FAT_ENOTDIR;

    entry_naming_t naming;
    rc = compute_entry_naming(&parent, base, &naming);
    if (rc != 0) return rc;

    uint16_t dir_cluster = fat_alloc_cluster();
    if (dir_cluster == 0) return FAT_ENOSPC;

    /* Write . and .. + zeros to the whole cluster (zero = end-of-dir). */
    uint8_t sec[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sec[i] = 0;

    char dot11[11]    = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    char dotdot11[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };

    uint8_t dot_raw[DIRENT_SIZE];
    uint8_t dotdot_raw[DIRENT_SIZE];
    build_dirent_raw(dot_raw, dot11, ATTR_DIR, dir_cluster, 0);
    /* ".." → parent's first cluster (0 means "the root", FAT convention). */
    build_dirent_raw(dotdot_raw, dotdot11, ATTR_DIR, parent.first_cluster, 0);

    for (uint32_t i = 0; i < DIRENT_SIZE; i++) sec[i] = dot_raw[i];
    for (uint32_t i = 0; i < DIRENT_SIZE; i++) sec[DIRENT_SIZE + i] = dotdot_raw[i];

    /* Cluster spans `sectors_per_cluster` sectors. Write . / .. in the
     * first sector; zero the rest. */
    uint32_t lba_start = cluster_to_lba(dir_cluster);
    if (block_ata_write_sector(lba_start, sec) != 0) {
        fat_free_chain(dir_cluster);
        return FAT_EIO;
    }
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sec[i] = 0;
    for (uint32_t s = 1; s < fs.sectors_per_cluster; s++) {
        if (block_ata_write_sector(lba_start + s, sec) != 0) {
            fat_free_chain(dir_cluster);
            return FAT_EIO;
        }
    }

    /* Install the dirent in the parent. write_entry_with_naming handles
     * both plain-8.3 and LFN+8.3 cases. */
    uint8_t raw[DIRENT_SIZE];
    build_dirent_raw(raw, naming.alias11, ATTR_DIR, dir_cluster, 0);
    rc = write_entry_with_naming(&parent, &naming, raw);
    if (rc != 0) {
        fat_free_chain(dir_cluster);
        return rc;
    }
    return 0;
}

/* rmdir: empty subdir, then unlink-like teardown. */
int fat_rmdir_path(const char *path) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t de;
    if (fat_lookup(path, &de) != 0) return FAT_ENOENT;
    if (!de.is_dir) return FAT_ENOTDIR;
    if (de.dirent_lba == 0) return FAT_EINVAL;       /* root */

    /* Scan dir entries; any valid one other than "." / ".." → not empty. */
    uint32_t idx = 0;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = read_dir_slot(&de, idx, slot, &lba, &off);
        if (rc < 0) return FAT_EIO;
        if (rc == 1) break;
        idx++;
        if (!dirent_is_valid(slot)) continue;
        /* "." / ".." — name11 first byte is '.' and (slot[1]==' ' or '.') */
        if (slot[0] == '.' && (slot[1] == ' ' || slot[1] == '.')) continue;
        return FAT_ENOTEMPTY;
    }

    /* Locate inside parent to find preceding LFN slots, same as unlink. */
    char rparent_path[64];
    char rbase[64];
    if (split_parent_base(path, rparent_path, rbase) != 0) return FAT_EINVAL;
    fat_dirent_t rparent;
    if (fat_lookup(rparent_path, &rparent) != 0) return FAT_EIO;

    uint32_t r_target_idx = 0, r_lfn_count = 0;
    if (locate_target_and_lfn(&rparent, de.dirent_lba, de.dirent_offset,
                                &r_target_idx, &r_lfn_count) != 0) {
        return FAT_EIO;
    }

    if (fat_free_chain(de.first_cluster) != 0) return FAT_EIO;

    if (delete_slot_by_idx(&rparent, r_target_idx) != 0) return FAT_EIO;
    for (uint32_t i = 0; i < r_lfn_count; i++) {
        delete_slot_by_idx(&rparent, r_target_idx - r_lfn_count + i);
    }
    return 0;
}

/*
 * Rename / move within the FAT volume.
 *   - Same parent → O(1): overwrite the 11 name bytes in src's dirent.
 *   - Cross-directory → reuse the existing FAT chain by copying src's
 *     dirent (with the new name) into a free slot in dst's parent,
 *     then marking src's dirent 0xE5.
 *
 * The cross-dir order is deliberate: src-deleted FIRST, then dst
 * written. A crash mid-rename leaks the file (visible to fsck) rather
 * than cross-linking it (both directories pointing at the same chain).
 * We pre-reserve the dst slot before deleting src so ENOSPC fails
 * fast without losing data.
 *
 * Overwrite of an existing dst is rejected (EEXIST). The caller is
 * expected to unlink dst first if it wants Unix-style overwrite.
 * Renaming a path to itself (case-insensitive, same on-disk dirent)
 * is a no-op success.
 */
int fat_rename_path(const char *src, const char *dst) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t src_de;
    if (fat_lookup(src, &src_de) != 0) return FAT_ENOENT;
    if (src_de.dirent_lba == 0) return FAT_EINVAL;   /* root */

    /* If dst resolves to the SAME on-disk dirent as src (different
     * spellings, casing, trailing slashes), no-op. */
    fat_dirent_t dst_de;
    if (fat_lookup(dst, &dst_de) == 0) {
        if (dst_de.dirent_lba == src_de.dirent_lba &&
            dst_de.dirent_offset == src_de.dirent_offset) {
            return 0;
        }
        return FAT_EEXIST;
    }

    char dst_parent_path[64], dst_base[64];
    int rc = split_parent_base(dst, dst_parent_path, dst_base);
    if (rc != 0) return rc;

    fat_dirent_t dst_parent;
    if (fat_lookup(dst_parent_path, &dst_parent) != 0) return FAT_ENOENT;
    if (!dst_parent.is_dir) return FAT_ENOTDIR;

    char src_parent_path[64], src_base[64];
    rc = split_parent_base(src, src_parent_path, src_base);
    if (rc != 0) return rc;
    fat_dirent_t src_parent;
    if (fat_lookup(src_parent_path, &src_parent) != 0) return FAT_EIO;

    /* Locate the src 8.3 entry inside its parent so we know about
     * preceding LFN slots (deleted at the end, regardless of path). */
    uint32_t src_target_idx = 0, src_lfn_count = 0;
    if (locate_target_and_lfn(&src_parent,
                                src_de.dirent_lba, src_de.dirent_offset,
                                &src_target_idx, &src_lfn_count) != 0) {
        return FAT_EIO;
    }

    entry_naming_t naming;
    rc = compute_entry_naming(&dst_parent, dst_base, &naming);
    if (rc != 0) return rc;

    bool same_parent = (src_parent.first_cluster == dst_parent.first_cluster);

    /* Fast path: same parent + new fits 8.3 + src had no LFN slots.
     * Just rewrite the 11 name bytes in place — one sector RMW. */
    if (same_parent && !naming.needs_lfn && src_lfn_count == 0) {
        uint8_t sb[SECTOR_SIZE];
        if (block_ata_read_sector(src_de.dirent_lba, sb) != 0) return FAT_EIO;
        for (int i = 0; i < 11; i++) {
            sb[src_de.dirent_offset + i] = (uint8_t)naming.alias11[i];
        }
        if (block_ata_write_sector(src_de.dirent_lba, sb) != 0) return FAT_EIO;
        return 0;
    }

    /*
     * General path: build the new 8.3 from src's bytes (preserving
     * first_cluster / size / attr), drop the new alias into it, then
     * insert a fresh LFN+8.3 group in dst_parent. Once that lands,
     * delete the old src group.
     *
     * Order: write-new FIRST → brief cross-link window where both src
     * and dst point at the same chain. Then delete src 8.3 (cross-link
     * resolves) and finally any src LFN slots (cosmetic orphan if a
     * crash hits between). fsck recovers cleanly from both states.
     */
    uint8_t sb_src[SECTOR_SIZE];
    if (block_ata_read_sector(src_de.dirent_lba, sb_src) != 0) return FAT_EIO;

    uint8_t new_raw[DIRENT_SIZE];
    for (int i = 0; i < DIRENT_SIZE; i++) {
        new_raw[i] = sb_src[src_de.dirent_offset + i];
    }
    for (int i = 0; i < 11; i++) new_raw[i] = (uint8_t)naming.alias11[i];

    rc = write_entry_with_naming(&dst_parent, &naming, new_raw);
    if (rc != 0) return rc;

    /* Delete old 8.3 first (file disappears from src's directory),
     * then preceding LFN slots. */
    if (delete_slot_by_idx(&src_parent, src_target_idx) != 0) return FAT_EIO;
    for (uint32_t i = 0; i < src_lfn_count; i++) {
        delete_slot_by_idx(&src_parent, src_target_idx - src_lfn_count + i);
    }
    return 0;
}

/* ================================================================ */
/* fsck (FASE 8.5) — read-only audit                                */
/* ================================================================ */

#define FSCK_MAX_DEPTH       16
#define FSCK_MAX_CHAIN_HOPS  65525  /* upper bound on FAT16 cluster count */

/*
 * Cluster reachability bitmap. One bit per FAT entry index; bit set ⇔
 * cluster was visited while walking a dirent chain. After the dir
 * tree walk, any FAT entry whose value is non-zero and non-bad but
 * whose bit stayed clear is a leak.
 *
 * 65525 bits ≈ 8 KiB BSS — small enough to keep static and large enough
 * to cover the full FAT16 cluster space.
 */
#define FSCK_BITMAP_BYTES    ((65525 + 7) / 8)
static uint8_t fsck_used_map[FSCK_BITMAP_BYTES];

static void fsck_clear_used(void) {
    for (uint32_t i = 0; i < FSCK_BITMAP_BYTES; i++) fsck_used_map[i] = 0;
}

static bool fsck_is_used(uint16_t cluster) {
    return (fsck_used_map[cluster / 8] >> (cluster % 8)) & 1u;
}

static void fsck_set_used(uint16_t cluster) {
    fsck_used_map[cluster / 8] |= (uint8_t)(1u << (cluster % 8));
}

/*
 * Walk `first`'s cluster chain. Marks every visited cluster in the
 * bitmap and counts the chain length. A revisit (cross-link OR cycle)
 * bumps `*crosslinks` and stops — the fsck_is_used check is what makes
 * the walk safe against corrupted FATs that loop back on themselves.
 * Bad / out-of-range references bump `*bad_refs`.
 */
static uint32_t fsck_walk_chain(uint16_t first,
                                  uint32_t *crosslinks,
                                  uint32_t *bad_refs) {
    uint32_t len = 0;
    uint16_t cur = first;

    for (uint32_t hops = 0; hops < FSCK_MAX_CHAIN_HOPS; hops++) {
        if (cur < 2 || cur >= FAT16_EOF_MIN) break;
        if (cur == FAT16_BAD) { (*bad_refs)++; break; }
        if (fsck_is_used(cur)) { (*crosslinks)++; break; }

        fsck_set_used(cur);
        len++;

        uint16_t next;
        if (fat_get_entry(cur, &next) != 0) { (*bad_refs)++; break; }
        cur = next;
    }
    return len;
}

typedef struct {
    fat_dirent_t  dir;
    uint32_t      cursor;
} fsck_frame_t;

typedef struct {
    uint32_t file_count;
    uint32_t dir_count;
    uint32_t crosslinks;
    uint32_t bad_refs;
    uint32_t size_errors;
    uint32_t deep_skip;
} fsck_walk_t;

/*
 * Iterative DFS over the directory tree. The fsck_frame_t × MAX_DEPTH
 * stack lives in whichever task calls fsck (≈ 1.3 KiB; safe on the
 * 16 KiB kernel kstack). Subdirectories beyond MAX_DEPTH are skipped
 * and counted in `deep_skip` so the report can mention it.
 */
static void fsck_walk_tree(fsck_walk_t *w) {
    fsck_frame_t stack[FSCK_MAX_DEPTH];
    int top = 0;
    stack[0].cursor = 0;
    stack[0].dir.is_dir = true;
    stack[0].dir.first_cluster = 0;   /* root sentinel */

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * SECTOR_SIZE;

    while (top >= 0) {
        fat_dirent_t ent;
        uint32_t next;
        int rc = fat_readdir(&stack[top].dir, stack[top].cursor, &ent, &next);
        if (rc != 0) { top--; continue; }
        stack[top].cursor = next;

        /* Skip "." and ".." — they'd re-mark already-used clusters and
         * trigger phantom cross-links. */
        if (ent.name[0] == '.' &&
            (ent.name[1] == 0 ||
             (ent.name[1] == '.' && ent.name[2] == 0))) {
            continue;
        }

        if (ent.is_dir) {
            w->dir_count++;
            if (ent.first_cluster >= 2) {
                fsck_walk_chain(ent.first_cluster,
                                  &w->crosslinks, &w->bad_refs);
            }
            if (top + 1 < FSCK_MAX_DEPTH) {
                top++;
                stack[top].dir    = ent;
                stack[top].cursor = 0;
            } else {
                w->deep_skip++;
            }
        } else {
            w->file_count++;
            uint32_t len = 0;
            if (ent.size > 0 && ent.first_cluster >= 2) {
                len = fsck_walk_chain(ent.first_cluster,
                                        &w->crosslinks, &w->bad_refs);
            }
            /* size==0 should have first_cluster==0. size>0 needs a
             * chain that covers it without leaving an unused trailing
             * cluster (i.e. size > (len-1) * cluster_size). */
            if (ent.size == 0 && ent.first_cluster != 0) {
                w->size_errors++;
            } else if (ent.size > 0 && len == 0) {
                w->size_errors++;
            } else if (ent.size > len * cluster_size) {
                w->size_errors++;
            } else if (len > 0 &&
                       ent.size <= (len - 1) * cluster_size) {
                w->size_errors++;
            }
        }
    }
}

static int fsck_cluster_counts(uint32_t *free_c, uint32_t *used_c,
                                 uint32_t *bad_c) {
    *free_c = 0;
    *used_c = 0;
    *bad_c  = 0;

    uint32_t max_entries = (uint32_t)fs.fat_size_sectors * SECTOR_SIZE / 2;
    if (max_entries > 65525) max_entries = 65525;

    uint8_t buf[SECTOR_SIZE];
    uint32_t cached_sec = (uint32_t)-1;

    for (uint32_t c = 2; c < max_entries; c++) {
        uint32_t byte_off = c * 2;
        uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (sec != cached_sec) {
            if (block_ata_read_sector(sec, buf) != 0) return -1;
            cached_sec = sec;
        }
        uint16_t v = (uint16_t)buf[in_sec] | ((uint16_t)buf[in_sec + 1] << 8);
        if (v == 0)              (*free_c)++;
        else if (v == FAT16_BAD) (*bad_c)++;
        else                     (*used_c)++;
    }
    return 0;
}

static uint32_t fsck_count_leaks(void) {
    uint32_t leaks = 0;
    uint32_t max_entries = (uint32_t)fs.fat_size_sectors * SECTOR_SIZE / 2;
    if (max_entries > 65525) max_entries = 65525;

    uint8_t buf[SECTOR_SIZE];
    uint32_t cached_sec = (uint32_t)-1;

    for (uint32_t c = 2; c < max_entries; c++) {
        uint32_t byte_off = c * 2;
        uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (sec != cached_sec) {
            if (block_ata_read_sector(sec, buf) != 0) return leaks;
            cached_sec = sec;
        }
        uint16_t v = (uint16_t)buf[in_sec] | ((uint16_t)buf[in_sec + 1] << 8);
        if (v == 0 || v == FAT16_BAD) continue;
        if (!fsck_is_used((uint16_t)c)) leaks++;
    }
    return leaks;
}

/*
 * Compare FAT[0] sector-by-sector against every other FAT copy.
 * `*divergent_sectors` is summed across all mirrors — e.g. with
 * num_fats=2 and 3 differing sectors, the result is 3.
 */
static int fsck_check_mirror(uint32_t *divergent_sectors) {
    *divergent_sectors = 0;
    if (fs.num_fats < 2) return 0;

    uint8_t buf_a[SECTOR_SIZE], buf_b[SECTOR_SIZE];
    for (uint32_t s = 0; s < fs.fat_size_sectors; s++) {
        uint32_t lba_a = fs.fat_lba + s;
        if (block_ata_read_sector(lba_a, buf_a) != 0) return -1;
        for (uint8_t k = 1; k < fs.num_fats; k++) {
            uint32_t lba_b = fs.fat_lba +
                             (uint32_t)k * fs.fat_size_sectors + s;
            if (block_ata_read_sector(lba_b, buf_b) != 0) return -1;
            for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
                if (buf_a[i] != buf_b[i]) { (*divergent_sectors)++; break; }
            }
        }
    }
    return 0;
}

/* ----- public entry point ----- */

static void emit_label_num(char *out, size_t out_size,
                            const char *label, uint64_t n) {
    char num[24];
    os_strlcat(out, label, out_size);
    os_format_u64(n, num, sizeof(num));
    os_strlcat(out, num, out_size);
    os_strlcat(out, "\n", out_size);
}

static void emit_count_or_none(char *out, size_t out_size,
                                const char *label, uint32_t n) {
    char num[24];
    os_strlcat(out, label, out_size);
    if (n == 0) {
        os_strlcat(out, "none\n", out_size);
    } else {
        os_format_u64(n, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, "\n", out_size);
    }
}

void fat_fsck_report(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;

    if (!fs_ready) {
        os_strlcpy(out, "fsck: FAT not mounted\n", out_size);
        return;
    }

    uint32_t free_c = 0, used_c = 0, bad_c = 0;
    if (fsck_cluster_counts(&free_c, &used_c, &bad_c) != 0) {
        os_strlcpy(out, "fsck: FAT read failed\n", out_size);
        return;
    }

    fsck_clear_used();

    fsck_walk_t w = { 0, 0, 0, 0, 0, 0 };
    fsck_walk_tree(&w);

    uint32_t leaks = fsck_count_leaks();

    uint32_t divergent = 0;
    int mirror_rc = fsck_check_mirror(&divergent);

    os_strlcat(out, "fsck /sd (FAT16)\n", out_size);
    os_strlcat(out, "================\n", out_size);

    emit_label_num(out, out_size, "files:         ", w.file_count);
    emit_label_num(out, out_size, "dirs:          ", w.dir_count);
    emit_label_num(out, out_size, "clusters free: ", free_c);
    emit_label_num(out, out_size, "clusters used: ", used_c);
    emit_label_num(out, out_size, "clusters bad:  ", bad_c);

    os_strlcat(out, "\nmirror:        ", out_size);
    if (fs.num_fats < 2) {
        os_strlcat(out, "n/a (single FAT)\n", out_size);
    } else if (mirror_rc != 0) {
        os_strlcat(out, "read error\n", out_size);
    } else if (divergent == 0) {
        os_strlcat(out, "OK\n", out_size);
    } else {
        char num[24];
        os_strlcat(out, "DIVERGENT (sectors=", out_size);
        os_format_u64(divergent, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, ")\n", out_size);
    }

    emit_count_or_none(out, out_size, "cross-links:   ",  w.crosslinks);
    emit_count_or_none(out, out_size, "leaks:         ",  leaks);
    emit_count_or_none(out, out_size, "size mismatch: ",  w.size_errors);
    emit_count_or_none(out, out_size, "bad refs:      ",  w.bad_refs);

    if (w.deep_skip) {
        char num[24];
        os_strlcat(out, "\nNOTE: ", out_size);
        os_format_u64(w.deep_skip, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, " subdir(s) past depth ", out_size);
        os_format_u64(FSCK_MAX_DEPTH, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, " skipped\n", out_size);
    }

    os_strlcat(out, "\n(read-only audit)\n", out_size);
}
