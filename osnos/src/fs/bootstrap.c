#include "bootstrap.h"

#include "../drivers/framebuffer.h"
#include "../lib/string.h"
#include "../proc/builtin.h"
#include "aliasfs.h"
#include "binfs.h"
#include "devfs.h"
#include "fat.h"
#include "fat_vfs.h"
#include "ramfs_vfs.h"
#include "sysfs.h"
#include "vfs.h"

static void seed_file(const char *path, const char *content) {
    vfs_write(path, content, os_strlen(content));
}

/* Seed only if the file doesn't already exist (so we don't trample a
 * user's edits across boots when /home lives on disk). */
static void seed_if_absent(const char *path, const char *content) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) == OSNOS_OK) return;
    seed_file(path, content);
}

/* Storage for the /home → /sd/home, /bin → /sd/bin, /lib → /sd/lib,
 * /usr → /sd/usr, /etc → /sd/etc bind mounts. Lives in BSS — the
 * VFS layer keeps pointers into here. */
static aliasfs_t home_alias_slot;
static aliasfs_t bin_alias_slot;
static aliasfs_t lib_alias_slot;
static aliasfs_t usr_alias_slot;
static aliasfs_t etc_alias_slot;

/* Walk the embedded ELF registry and write each blob into /sd/bin/<name>
 * if the file isn't already there. With FASE2-1's dir-chain extension
 * fix the full set (~60 entries) now fits, so we drop the curated
 * subset. Skipping happens via vfs_stat — subsequent boots seed
 * zero new files and return instantly. */
static void seed_disk_bin(void) {
    char path[64];
    size_t n = builtin_count();
    int    needed = 0;
    for (size_t i = 0; i < n; i++) {
        const builtin_t *b = builtin_at(i);
        if (!b || !b->elf_start || !b->elf_end) continue;
        os_strlcpy(path, "/sd/bin/", sizeof(path));
        os_strlcat(path, b->name, sizeof(path));
        vfs_stat_t st;
        if (vfs_stat(path, &st) != OSNOS_OK) needed++;
    }
    if (needed == 0) return;

    framebuffer_clear(0x000000);
    framebuffer_draw_string(
        "osnos first-boot: seeding /sd/bin (full set)...\n",
        0xffff00);

    int done = 0;
    for (size_t i = 0; i < n; i++) {
        const builtin_t *b = builtin_at(i);
        if (!b || !b->elf_start || !b->elf_end) continue;
        os_strlcpy(path, "/sd/bin/", sizeof(path));
        os_strlcat(path, b->name, sizeof(path));

        vfs_stat_t st;
        if (vfs_stat(path, &st) == OSNOS_OK) continue;

        framebuffer_draw_string("  ", 0xcccccc);
        framebuffer_draw_string(b->name, 0xcccccc);
        framebuffer_draw_string(" ", 0xcccccc);

        size_t sz = (size_t)(b->elf_end - b->elf_start);
        osnos_status_t s = vfs_write(path, (const char *)b->elf_start, sz);
        if (s == OSNOS_OK) {
            framebuffer_draw_string("ok\n", 0x00ff66);
        } else {
            char msg[24];
            os_strlcpy(msg, "FAIL (", sizeof(msg));
            char num[8];
            uint32_t code = (uint32_t)s;
            int ni = 0;
            char tmp[8];
            if (code == 0) tmp[ni++] = '0';
            while (code > 0 && ni < 7) { tmp[ni++] = (char)('0' + (code % 10)); code /= 10; }
            for (int j = ni - 1, k = 0; j >= 0; j--, k++) num[k] = tmp[j];
            num[ni] = 0;
            os_strlcat(msg, num, sizeof(msg));
            os_strlcat(msg, ")\n", sizeof(msg));
            framebuffer_draw_string(msg, 0xff5555);
        }
        done++;
        (void)done;
    }
    framebuffer_draw_string("seeded. starting shell...\n", 0xffff00);
}

void bootstrap_fs(void) {
    vfs_init();

    /* ramfs at "/" serves everything except more specific mounts below. */
    vfs_mount("/", &ramfs_vfs_ops, 0);

    /* sysfs at "/sys" — synthetic, read-only, no ramfs storage needed. */
    vfs_mount("/sys", &sysfs_vfs_ops, 0);

    /* devfs at "/dev" — synthetic character devices. */
    vfs_mount("/dev", &devfs_vfs_ops, 0);

    /* /sd — FAT16 on the ATA primary master, when present. Mounting
     * only if the parser bound to a valid BPB keeps the mount table
     * tidy when there's no disk attached. */
    bool fat_mounted = (fat_init() == 0);
    if (fat_mounted) {
        vfs_mount("/sd", &fat_vfs_ops, 0);
    }

    /*
     * /bin backing:
     *   - With a disk: dump every embedded ELF into /sd/bin (first
     *     boot only), then alias /bin → /sd/bin. From here on the
     *     binaries live on FAT — editable, replaceable, persisted.
     *   - Without a disk: synthetic binfs over the in-kernel builtin
     *     registry (read-only).
     */
    /* /bin backing:
     *   - With a disk: dump every embedded ELF into /sd/bin (first
     *     boot only — FASE2-1 fixed the FAT16 dir-chain extension so
     *     the full set fits), then alias /bin → /sd/bin. From here
     *     on the binaries live on disk and are editable / replaceable.
     *   - Diskless: synthetic binfs over the in-kernel builtin
     *     registry (read-only).
     */
    if (fat_mounted) {
        vfs_mkdir("/sd/bin");
        seed_disk_bin();
        if (aliasfs_init(&bin_alias_slot, "/bin", "/sd/bin")) {
            vfs_mount("/bin", &aliasfs_ops, &bin_alias_slot);
        }
        /* /lib + /usr aliasfs mounts: FASE 11.0 ships the TCC
         * sysroot (crt1/crti/crtn/libc.a/libtcc1.a + libc headers)
         * pre-populated at /sd/lib and /sd/usr by the build script
         * (GNUmakefile target sd.img). TCC's hardcoded search
         * paths (`/lib`, `/usr/include` from vendor/tinycc/config.h)
         * resolve here. */
        if (aliasfs_init(&lib_alias_slot, "/lib", "/sd/lib")) {
            vfs_mount("/lib", &aliasfs_ops, &lib_alias_slot);
        }
        if (aliasfs_init(&usr_alias_slot, "/usr", "/sd/usr")) {
            vfs_mount("/usr", &aliasfs_ops, &usr_alias_slot);
        }
        /* /etc → /sd/etc — populated by the build with passwd,
         * group, hosts so BusyBox getpwuid / cat /etc/passwd
         * funcionan sin que cada applet maneje paths /sd/. */
        vfs_mkdir("/sd/etc");
        if (aliasfs_init(&etc_alias_slot, "/etc", "/sd/etc")) {
            vfs_mount("/etc", &aliasfs_ops, &etc_alias_slot);
        }
        /* /etc/profile — sourced ONCE at login. System-wide env vars
         * only (PATH/HOME/HISTFILE/...) — NO banners, no PS1, no
         * interactive niceties (those go in /home/.ashrc which is
         * sourced for every interactive shell). This split mirrors
         * the bash convention: /etc/profile + ~/.profile run on
         * login, ~/.bashrc runs every interactive shell. */
        seed_if_absent("/etc/profile",
            "# osnos /etc/profile — system-wide login env. Sourced ONCE.\n"
            "# Put env vars here. Put aliases / prompt / banner in ~/.ashrc.\n"
            "export PATH=/bin\n"
            "export HOME=/home\n"
            "export HISTFILE=/home/.ash_history\n"
            "export HISTSIZE=500\n"
            "export TERM=linux\n"
            "export ENV=/home/.ashrc\n");
        /* /home/.ashrc seed lives further down — it has to wait until
         * the /home aliasfs is mounted. */
    } else {
        vfs_mount("/bin", &binfs_vfs_ops, 0);
    }

    /*
     * /home backing:
     *   - With a disk: alias /home → /sd/home. Reads, writes, mkdir,
     *     mv all resolve onto FAT16 and persist across reboots.
     *   - Without a disk: plain ramfs entry (the "/" mount handles it).
     */
    if (fat_mounted) {
        /* Make sure /sd/home exists on disk. mkdir is idempotent —
         * returns OSNOS_EEXIST if already there. */
        vfs_mkdir("/sd/home");

        if (aliasfs_init(&home_alias_slot, "/home", "/sd/home")) {
            vfs_mount("/home", &aliasfs_ops, &home_alias_slot);
        }

        /* First-boot seed lives on disk; idempotent on subsequent boots. */
        seed_if_absent("/home/README.TXT",
            "Welcome to osnos.\nYour home lives on FAT16 — files persist across reboots.\n");
        seed_if_absent("/home/HELLO.TXT", "Hello from FAT-backed /home!\n");
        /* Sample shell rc — user-editable. shellsrv replays each
         * line at boot (FASE 10.4). Default seeds a useful env so
         * scripts that read $HOME / $PATH / $SHELL work out of box. */
        seed_if_absent("/home/.oshrc",
            "# osnos shell startup — runs at every shellsrv boot.\n"
            "# Edit to add your own commands / env vars.\n"
            "export PATH=/bin\n"
            "export HOME=/home\n"
            "export SHELL=/bin/shellsrv\n"
            "export OSNAME=osnos\n");
        /* /home/.ashrc — bash-style "rc file" para BusyBox ash.
         * Sourced en cada shell interactiva via $ENV=/home/.ashrc
         * (definido en /etc/profile). Aquí van prompt, aliases y
         * banner — análogo a ~/.bashrc en sistemas Linux. El usuario
         * puede editarlo libremente con `ovi /home/.ashrc`. */
        seed_if_absent("/home/.ashrc",
            "# osnos ~/.ashrc — sourced every interactive ash session.\n"
            "# Edit freely: prompt, aliases, banner, anything per-shell.\n"
            "export PS1='osnos:\\w# '\n"
            "alias ll='ls -l'\n"
            "alias la='ls -la'\n"
            "alias l='ls -CF'\n"
            "alias ..='cd ..'\n"
            "alias h='history'\n"
            "alias cls=clear\n"
            "/bin/banner osnos 2>/dev/null\n"
            "echo 'BusyBox ash on osnos — help for builtins, ls /bin for commands.'\n");
        /* Ox window-system settings (FASE 12). oxsrv reads this at
         * boot; /bin/oxsettings rewrites it. */
        seed_if_absent("/home/.oxrc",
            "current_wallpaper=samurai\n");
        /* Ensure /home/wallpapers/ exists even when no PPMs were
         * shipped on the disk image (rare — sd.img seeds them, but
         * a hand-edited disk might lack the dir). vfs_mkdir is
         * idempotent (EEXIST is OK). */
        vfs_mkdir("/home/wallpapers");
    } else {
        /* Diskless: same ramfs-backed /home as before. */
        vfs_mkdir("/home");
        seed_file("/home/README.TXT",
            "Welcome to osnos.\nThis is a tiny RAM filesystem (no disk attached).\n");
        seed_file("/home/HELLO.TXT", "Hello from ramfs!\n");
        seed_file("/home/.oshrc",
            "# osnos shell startup (RAM-only — won't persist across reboots)\n"
            "export PATH=/bin\n"
            "export HOME=/home\n"
            "export SHELL=/bin/shellsrv\n"
            "export OSNAME=osnos\n");
        seed_file("/home/.oxrc", "current_wallpaper=samurai\n");
        vfs_mkdir("/home/wallpapers");
    }
}
