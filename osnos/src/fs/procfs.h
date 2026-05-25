#pragma once

#include "vfs.h"

/*
 * Synthetic /proc para apps Linux (top, ps, free, uptime, htop, ...).
 *
 * Mount en "/proc". Lectura on-demand del task table + pmm + timer.
 *
 * Entries top-level:
 *   /proc/meminfo    — MemTotal/MemFree/MemAvailable/Buffers/Cached (formato Linux)
 *   /proc/uptime     — "<uptime_seconds> <idle_seconds>\n"
 *   /proc/loadavg    — "0.00 0.00 0.00 1/N pid\n"
 *   /proc/cpuinfo    — single cpu summary
 *   /proc/stat       — "cpu USR NICE SYS IDLE ...\n" (placeholder counters)
 *   /proc/version    — uname-style string
 *   /proc/self       — symlink-like a /proc/<my_pid> (devuelve mismo contenido)
 *   /proc/<pid>/     — directorio por task
 *     ├─ cmdline     — argv joined with NUL ('\0'), terminado en NUL extra
 *     ├─ comm        — task name + '\n'
 *     ├─ stat        — pid (name) state ppid pgid sid ... (formato Linux)
 *     └─ status      — multi-line Name/State/Pid/PPid/...
 *
 * Solo lectura. Mkdir/unlink/rename = EROFS.
 */
extern const vfs_ops_t procfs_vfs_ops;
