#pragma once

/*
 * Populates the initial filesystem layout at boot: top-level directories
 * (/home, /sys, /dev, /bin) and a handful of seed files.
 *
 * Today this just calls ramfs_mkdir / ramfs_create_file. When the VFS lands
 * (FASE 2 of ROADMAP), this is where `mount("devfs", "/dev", ...)`,
 * `mount("sysfs", "/sys", ...)` etc. will be wired. The intent is that
 * ramfs_init knows nothing about filesystem layout — it only manages slots.
 */
void bootstrap_fs(void);
