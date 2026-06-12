# STATUS — osnos

Current state of the project and phase log. This document is the
**source of truth on what works today**. For the layered
architecture see [`ARCH.md`](ARCH.md); for the pitch / overview see
the root [`README.md`](../README.md); for tutorials on how to extend
see [`CREATE_BUILTINS.es.md`](CREATE_BUILTINS.es.md) and
[`CREATE_ELF.es.md`](CREATE_ELF.es.md) (Spanish).

Spanish counterpart: [`STATUS.es.md`](STATUS.es.md).

Conventions:
- OK = works / closed / verified
- WARN = functional with known limitations
- TODO = pending / not implemented
- **FASE X — CLOSED** = roadmap phase done
- **FASE X — PENDING** = future phase, documented plan

---

## Executive summary

OSnOS is a hobby x86_64 microkernel written from scratch. It boots
with Limine, runs in QEMU, and ships:

- **Ring-0 kernel** with ELF loader (ET_EXEC + ET_DYN/PT_INTERP),
  its own 4-level paging, multi-backend VFS (ramfs + FAT16 + devfs
  + sysfs + binfs + aliasfs), preemptive scheduler (50 ms quantum
  in CPL=3), 64-slot IPC queue, POSIX line discipline, **~80
  syscalls** that match Linux x86_64
  (read/write/open/fork/clone/execve/wait/sigaction/pipe/mmap/shm/
  socket/AF_UNIX/select/poll/...).
- **Ring-3 servers**: console (`consrv`), keyboard (`kbdsrv`),
  mouse feeder, shell (`busybox sh` since FASE 13.1).
- **Two libcs**: mini-libc (`lib/libc/`) for small programs, and
  **musl 1.2.5** (FASE 13.0) — now also **shared library**
  (`/lib/libc.so` + `/lib/ld-musl-x86_64.so.1`) for dynamic
  linking (FASE 14.4).
- **BusyBox 1.36.1** linked against musl (FASE 13.1): ~60 applets
  reachable via aliases in `/home/.ashrc`. Default shell is
  `busybox sh` with persistent history.
- **Five languages / full self-hosting**: C (TCC 0.9.27, FASE
  11.0), Lua 5.4.7 (FASE 11.2), jq 1.7.1 (FASE 11.3), SQL via
  SQLite 3.45.2 (FASE 13.3), **POSIX make (pdpmake) —
  `cd /home && make hello && ./hello` builds with tcc from
  inside** (FASE 14.1).
- **Ox mini-X window system** (FASE 12.0): BeOS/Haiku-style server
  + **15 GUI apps** (notepad, calc, term, files, top, settings,
  hex editor, browser, sqliteview, log viewer, mem, ipc, net,
  **real NetSurf**, JS runner Duktape).
- **Duktape 2.7 JS runtime + oxjs runner** (FASE 12.4): bindings
  `ox.window` (rect/text/present/clear/poll/log); apps
  `quadratic.js` (interactive parabola) + `snake.js` (game) wired
  to the menu. Fifth-and-sixth language in the fleet
  (C/Lua/jq/SQL/make + **JS**).
- **NetSurf v1 port** (FASE 12.4): `libwapcaplet + libparserutils
  + libnsutils + libnslog + libhubbub + libdom` built as `.a`
  (Stage 1-3). `oxnetsurf.elf` 3.1 MB linked with musl + BearSSL
  + real DOM -> navigates HTTP/HTTPS, HTML5 parsing, walks the
  DOM with clickable links, scrollable Ox render. Skips libcss +
  netsurf-core (no real box-model layout yet).
- **liboxshim** (FASE 12.4): mini-libc `ox.c/ox_font.c/ox_icons.c
  /ox_text.c/ox_ui.c` recompiled against musl + explicit overrides
  of `shm_open`/`shm_unlink`/`connect` (musl's wrappers assume
  `/dev/shm/` and POSIX blocking; osnos is direct-syscall +
  non-blocking with EINPROGRESS-retry). Without this shim,
  oxnetsurf only painted the chrome and aborted the first fetch.
- **lighttpd 1.4.76** (FASE 14.5) — real webserver serving
  HTTP/1.1 from `/home`; `curl http://localhost:8088/` -> 200 OK
  (default hostfwd port since FASE 15.1; override with `make run HTTP_PORT=…`).
- **Real networking** (FASE 14-misc-3):
  `nslookup google.com -> 142.251.x.x` (DNS UDP resolver via musl
  `getaddrinfo`) and `ping 8.8.8.8 -> 64 bytes ttl=255` (SOCK_RAW
  + ICMP echo). New syscalls: `recvmsg`/`sendmsg`,
  `getsockname`/`getpeername`/`getsockopt` stubs. `SOCK_CLOEXEC`
  + `SOCK_NONBLOCK` bundled flags now supported.
- **Modern POSIX IPC** (FASE 14.2-14.4): AF_UNIX SOCK_STREAM,
  `shm_open` + `mmap(MAP_SHARED, fd)` with cross-fork shared
  memory, dynamic linking via `ld-musl.so` (`.so`-linked apps
  actually run).
- **Dual serial console** (UART 16550 COM1) for headless / CI
  boot.
- **21/21 automated tests** via `/bin/alltest` (includes
  unixtest, shmtest, hello_dyn). Each test has a 60s timeout so
  a hang does not block the suite.
- **Recent notable bug fixes**: (a) `rdmsr FS_BASE` in
  `sys_fork`/`sys_clone` (the stale snapshot caused a NULL deref
  in musl `__post_Fork`'s `__get_tp`); (b) `kill_pending` no
  longer force-exits if the app installed a handler (lighttpd
  graceful shutdown via Ctrl+C); (c) `TIOCSPGRP`/`TIOCGPGRP`
  ioctls so `tcsetpgrp` routes Ctrl+C to the correct pgid;
  (d) `SA_SIGINFO` handlers receive rsi/rdx = NULL (used to be
  garbage -> page fault); (e) execve resets `sa_handler[]` to
  SIG_DFL (POSIX); (f) `sys_execve` preserves argv boundaries;
  (g) `sys_read`/`write` AF_INET dispatch; (h) **IPC_OX_PRESENT
  always marks dirty** (FASE 12.2) — the legacy
  `if (g_wins[slot].dirty)` check was obsolete after the SHM
  refactor, draws never set it -> composite never fired (root
  cause of "Settings without thumbs" + "mouse lag after close");
  (i) **PTE_SHM bit** (FASE 12.2) — `address_space_destroy` no
  longer frees pages belonging to `shm_obj` to the PMM (was a
  latent double-free + corruption of oxsrv's backing after the
  client exited); (j) **IPC_PROC_EXITED scoped to children of
  shellsrv** (FASE 12.2) — used to send to SERVER_SHELL on EVERY
  task exit; Ox apps (parent=oxsrv) filled the shellsrv IPC queue
  (blocked on read stdin, never drained) -> cursor collapsed from
  30Hz to 4Hz after 3 closes due to scheduler thrash of spurious
  wake-block cycles.

**One-sentence pitch**: hobby x86_64 OS that runs BusyBox +
SQLite + Lua + jq + TCC + **make + lighttpd + Duktape JS +
NetSurf**, all natively compiled against musl (static and
dynamic), with working AF_UNIX + POSIX SHM + dynamic linking +
DNS + ICMP, and its own BeOS-style mini-X window system with 15
GUI apps — all on top of a microkernel written from scratch.

---

## What works today

### Boot + base architecture
| Subsystem | Status | Notes |
|---|---|---|
| Limine boot + linear framebuffer | OK | BIOS legacy (`-M pc`) |
| GDT + IDT + TSS (ring 0/3) | OK | SYSCALL + INT80 dual entry |
| PMM (bitmap) + VMM 4-level paging + kheap | OK | kheap cap 32 MiB |
| PIT @ 100 Hz + LAPIC | OK | 50 ms scheduling quantum in CPL=3 |
| `copy_from_user`/`copy_to_user` + extable | OK | Faulting user ptr -> EFAULT, no panic |
| FPU/SSE setup + per-task FXSAVE/FXRSTOR | OK | Concurrent FP across tasks is safe |
| Dual-console 16550 UART | OK | Headless boot, panic logs persist |

### Microkernel
| Subsystem | Status | Notes |
|---|---|---|
| Task table (16 slots) + preemptive scheduler | OK | longjmp resume pattern |
| **`block_restart_syscall` pattern** in sys_read / sys_poll | OK | Blocks via iret rewind, not longjmp with rax=0 (FASE 13.1) |
| **`fs_base` save/restore on task switch + live rdmsr on fork** | OK | Per-task TLS pointer; rdmsr in sys_fork/sys_clone avoids stale snapshot that NULL-derefed musl `__post_Fork` |
| Per-task fd table (16 fds) + OFD pool (128) | OK | POSIX shared offsets |
| pipe / dup / dup2 / fcntl | OK | FD_CLOEXEC per-fd |
| Anonymous `mmap`/`munmap` + brk/sbrk | OK | mmap_regions tracking + shm_backed flag |
| **`mmap(MAP_SHARED, fd_shm)` with fork preserving shared pages** | OK | Fork fixup re-maps parent's phys pages (FASE 14.3) |
| Signal delivery (sigaction, sigreturn, EINTR) | OK | Sigframe in user stack; rdi/rsi/rdx = sig/NULL/NULL (SA_SIGINFO compatible) |
| **`kill_pending` honors user handler** | OK | Ctrl+C in apps with SIGINT handler triggers graceful shutdown, not force-exit (FASE 14.5 polish) |
| **`sa_handler[]` reset to SIG_DFL in execve** | OK | POSIX violation fix — child used to inherit handlers whose pointers lived in the old binary's text |
| Automatic SIGCHLD + waitpid + WIFEXITED/SIGNALED | OK | TASK_ZOMBIE state |
| Process groups + sessions + Ctrl+C fan-out to pgid | OK | WUNTRACED/WCONTINUED; **`TIOCSPGRP`/`TIOCGPGRP`** ioctls (FASE 14.5 polish) |
| PTY pairs (`/dev/ptmx` + `/dev/pts/N`, pool 8) | OK | Canon/raw, ECHO, TIOCS* ioctls |
| **POSIX TTY line discipline** + consistent echo + backspace | OK | Echo via `framebuffer_write_bytes`, same path as apps (FASE 13.3 fix) |
| **Real `SYS_CLONE`** (`CLONE_VM`, `CLONE_VFORK`, `SIGCHLD`) | OK | For musl `posix_spawn`; pml4 sharing via lookup-refcount (FASE 14.1) |
| **`sys_execve` preserves argv boundaries** | OK | Array-based path (no flat-join + re-tokenize) — `sh -c "echo HELLO"` now sees 3 correct argv (FASE 14.1) |
| **`sys_read`/`write` dispatch AF_INET + AF_UNIX** | OK | Read/write directly on stream sockets (lighttpd uses this path) |
| **Permissive `sys_setsockopt`** | OK | SOL_SOCKET/IPPROTO_TCP/IPPROTO_IP = no-op success (accepts TCP_NODELAY etc.) (FASE 14.5) |
| **Full auxv** (AT_PHDR/PHENT/PHNUM/BASE/ENTRY/RANDOM) | OK | So ld-musl.so can parse (FASE 14.4) |
| 64 x 1024 B IPC queue + service registry | OK | Routing by SID or direct pid |
| `init-respawn` watchdog for servers | OK | consrv/kbdsrv/busybox auto-restart |

### Filesystem
| Subsystem | Status | Notes |
|---|---|---|
| VFS with longest-prefix backend dispatch | OK | 16 mount slots (was 8 pre-FASE-13.1) |
| ramfs (`/`) 32 slots x 128 B path + 512 B data | OK | |
| sysfs (`/sys`) read-only synthetic | OK | task table, ipc count, mem stats |
| devfs (`/dev`) with fb0/input0/mouse0/tty/ttyS0/ptmx/pts | OK | FBIOGET/FBIO_BLIT ioctls on fb0 |
| binfs (`/bin`) diskless fallback | OK | On top of the kernel builtin registry |
| **FAT16** read/write/append + dir-chain extension + sector cache | OK | 32 MiB sd.img, persistent |
| **aliasfs** bind-mount style (`/bin -> /sd/bin`, `/home -> /sd/home`, `/etc -> /sd/etc`, `/lib -> /sd/lib`, `/usr -> /sd/usr`) | OK | Read/write transparent to FAT |
| Offset-native VFS reads (`vfs_read_at`) | OK | O(count) instead of O(file_size) |
| `sys_stat` byte-by-byte path copy | OK | Doesn't fault on short paths (FASE 13.1 fix) |
| Syscalls: open/openat/close/read/write/lseek/fstat/stat/lstat/newfstatat/getdents64/access/mkdir/rmdir/unlink/rename/chdir/getcwd/dup/dup2/fcntl/fsync/ftruncate/ioctl/select/poll/pipe | OK | Linux x86_64 compatible |

### Networking
| Subsystem | Status | Notes |
|---|---|---|
| RTL8139 driver + ARP + IPv4 + ICMP + UDP + TCP | OK | PCI bus scan |
| POSIX sockets (socket/bind/listen/accept/connect/send/recv/select) | OK | Direct read/write supported (FASE 14.5) |
| **`SOCK_CLOEXEC` + `SOCK_NONBLOCK` flag bundle in `socket(2)` type arg** | OK | musl `res_msend` passes `SOCK_DGRAM\|SOCK_CLOEXEC\|SOCK_NONBLOCK` (=0x80802) — used to reject with EAFNOSUPPORT and break `getaddrinfo` (FASE 14.6) |
| **`recvmsg(2)` / `sendmsg(2)`** (Linux 46/47) — single-iovec | OK | musl's resolver uses recvmsg; without this, nslookup never matched (FASE 14.6) |
| **`getsockname` / `getpeername` / `getsockopt` stubs** | OK | Return OK + zero — enough for musl post-connect (FASE 14.6) |
| **`sys_recvfrom` UDP path preserves `src_ip/src_port`** | OK | Used to route via `sock_recv` which discarded the peer -> musl resolver rejected the reply (FASE 14.6) |
| **`SOCK_RAW` + ICMP echo (ping)** | OK | `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)`; ip_handle mirrors to raw socket pool; `ping 8.8.8.8 -> "64 bytes from 8.8.8.8 ttl=255"` (FASE 14.6) |
| DNS resolver + getaddrinfo (via slirp 10.0.2.3) | OK | `nslookup google.com -> 142.251.128.46` (FASE 14.6) |
| `/bin/httpd` serving FAT16 over HTTP | OK | hostfwd 8088 (default, overridable) |
| **`/bin/lighttpd` 1.4.76 real webserver** | OK | poll-based, 10 builtin mods, serves `/home` (FASE 14.5) |
| **`SIOCGIFCONF` + 7 SIOC* ioctls** | OK | `ifconfig` shows eth0 (10.0.2.15 + MAC 52:54:00:12:34:56) + lo |
| Demos (`/bin/tcpclient`, `udptest`, `echotcp`, `selectserver`, `udp_send`, `udp_connect`) | OK | |

### Modern POSIX IPC + dynamic linking (FASE 14)
| Subsystem | Status | Notes |
|---|---|---|
| **AF_UNIX SOCK_STREAM** (`/bin/unixtest` smoke) | OK | Pool 32 sockets + 16 bound paths + 4 KiB ring buffers; no abstract namespace (FASE 14.2) |
| **POSIX `shm_open` + `mmap(MAP_SHARED, fd)`** (`/bin/shmtest`) | OK | Pool 16 objects x 256 pages; cross-fork shared memory verified (FASE 14.3) |
| **Dynamic linking via `ld-musl-x86_64.so.1`** (`/bin/hello_dyn`) | OK | PT_INTERP + full auxv; `.so`-linked apps run (FASE 14.4) |
| **`/lib/libc.so` + `/lib/ld-musl-x86_64.so.1`** staged in sd.img | OK | musl rebuilt with shared support; same binary is libc.so and the dynamic linker |

### Userland — shell + commands
| Component | Status | Notes |
|---|---|---|
| **`/bin/busybox` (1.36.1, musl-linked)** | OK | Default shell + ~60 applets (FASE 13.1) |
| **Persistent history `/home/.ash_history`** | OK | `FEATURE_EDITING_SAVEHISTORY=y` + `SAVE_ON_EXIT=y`, cross-reboot |
| **`/etc/profile` + `/home/.ashrc`** (.bashrc style) | OK | Banner + PS1 + applet aliases |
| **`vi awk sed find diff patch hexdump more dd df du stat readlink realpath base64 md5sum sha1sum sha256sum cksum bc dc xargs tac factor fold expand rev strings timeout`** | OK | Via aliases in `.ashrc` |
| `/bin/shellsrv` (legacy custom shell) | OK | Diskless fallback if `/bin/busybox` is missing |
| Native coreutils (~60 ELFs) — `ls cat cp mv rm mkdir touch echo wc head tail grep sort uniq cut tr seq yes tee env pwd which printf date uname basename dirname clear tree banner ...` | OK | Mini-libc-linked |
| `/bin/less` with `/pattern` highlight + `n`/`N` | OK | Pipe-mode (`cat foo \| less`) drains stdin + dup2 /dev/tty |
| `/bin/ovi` modal vim-style editor | OK | hjkl, i/a/o, x/dd, :w/:q |
| `/bin/readelf -a/-l/-S/-h` | OK | ELF header + phdr + shdr inspector |
| `/bin/poweroff` + `/bin/reboot` | OK | ACPI S5 + 8042 reset |
| `tail -f` (`/bin/tail`) | OK | Poll loop 200 ms with EAGAIN/EINTR |
| `/bin/term` + `/bin/minishell` | OK | Interactive PTY sub-shell (POSIX showcase) |

### Self-hosting (5 languages + make)
| Component | Status | Notes |
|---|---|---|
| **`/bin/tcc` — TinyCC 0.9.27** | OK | C compiler; produces static ELFs runnable against `/lib/libc.a` (FASE 11.0) |
| **`/bin/lua` — Lua 5.4.7** | OK | REPL + scripts (FASE 11.2) |
| **`/bin/jq` — jq 1.7.1** | OK | JSON filter/transformer (FASE 11.3) |
| **`/bin/sqlite3` — SQLite 3.45.2** | OK | Full SQL engine + `/home/demo.db` (15 books + view + indices) preseeded (FASE 13.3) |
| **`/bin/make` — pdpmake 1.4.1 (POSIX make)** | OK | `cd /home && make hello && ./hello` builds with tcc end-to-end (FASE 14.1) |

### Window system (Ox)
| Component | Status | Notes |
|---|---|---|
| **`/bin/oxsrv`** — ring-3 server ~1900 LOC | OK | SHM-backed compositor + cursor + z-order + Adwaita-dark root menu (FASE 12.0/12.2) |
| **SHM-backed window backings** | OK | Client + oxsrv share a mmap MAP_SHARED of the same shm_obj; `ox_draw_*` are local writes, `ox_present` = 1 IPC. From ~1270 IPCs per render to 1 (FASE 12.2) |
| **PTE_SHM bit in PTEs** | OK | `address_space_destroy` skips SHM pages (owned by shm_obj, not the task). Without this, client exit corrupted the framebuffer (FASE 12.2) |
| **Diagnostics heartbeat** | OK | iters/Hz, ev/s(m/k/i), full breakdown (alloc/destroy/raise/reload), t_full/t_dirty/t_destroy in ms with max, avg_px of the dirty rect (FASE 12.2) |
| **Ox client API** (`lib/libc/ox.{c,h}`) mini-Xlib style | OK | window_create/draw_rect/draw_text/draw_image/present/poll_event; draws are local writes to the mmap'd backing |
| `/bin/oxnotepad` text editor with argv path | OK | |
| `/bin/oxcalc` 4-function calculator | OK | |
| `/bin/oxterm` PTY + uxsh sub-shell + full ANSI parser (SGR truecolor, cursor pos, erase) | OK | (FASE 12.1) |
| `/bin/oxfiles` click-to-open file browser | OK | (FASE 12.1) |
| `/bin/oxtop` process viewer (kill by PID) | OK | (FASE 12.2) |
| `/bin/oxsettings` wallpaper picker | OK | Edits `/home/.oxrc`, IPC_OX_RELOAD_SETTINGS; thumb grid 200x120 from `/home/wallpapers/thumbs/` |
| `/bin/oxlog` log viewer (`oxlog /home/lighttpd.log`, F5 reload) | OK | (FASE 12.3) |
| `/bin/oxmem` dual-panel memstats + tasks | OK | `/sys/meminfo + /sys/tasks`, 1s refresh (FASE 12.3) |
| `/bin/oxipc` dual-panel services + IPC queue | OK | `/sys/services + /sys/tasks` (FASE 12.3) |
| `/bin/oxnet` dual-panel net + arp | OK | `/sys/net + /sys/arp` (FASE 12.3) |
| `/bin/oxhexedit` hex viewer/editor with cursor + paging | OK | (FASE 12.3) |
| `/bin/oxbrowser` simple HTTP/HTTPS browser (Lynx-like) | OK | Custom resolver + BearSSL X.509 no-anchor; URL bar + history + back; renders plain text + manual anchor link parse (no real DOM) (FASE 12.3) |
| `/bin/oxsqliteview` SQL grid with file-picker + .schema + Export CSV | OK | Toolbar with path input + `...` filepicker; reload without relaunching (FASE 12.3) |
| **`/bin/oxnetsurf`** HTML browser with real DOM (libhubbub + libdom) | OK | musl-linked 3.1 MB; HTTP/HTTPS via BearSSL; HTML5 parsing; clickable links; no box-model layout (Stage 4 libcss pending). Wired to the BeOS menu (FASE 12.4) |
| **`/bin/oxjs` Duktape 2.7 JS runner** | OK | musl-linked ~600 KB; `ox.window.{rect,text,present,clear,poll,log}` bindings; flags `oxjs N` load script #N from `/home/*.js`. Apps `quadratic.js` + `snake.js` shipped. (FASE 12.4) |
| **WM BeOS-style titlebar (yellow tab + close/min/zoom)** | OK | Yellow tab `#FCE06D` BeOS R5 style; three 12x12 square buttons in the tab (zoom/min/close), drag-to-move, click-to-focus, correct hit-test (FASE 12.4) |
| **WM deskbar tile-strip top-right + HH:MM clock** | OK | BeOS R5 style; Yaru-colored icons + 3-letter tile per window; 60s clock refresh (FASE 12.4) |
| JPG -> PPM build-time wallpapers (`wallpaper1.jpg` default + 9 more) | OK | `tools/gen_wallpapers.sh` produces PPM P6 + 200x120 thumbnails |
| FB ioctls: `FBIOGET_VSCREENINFO`, `FBIO_BLIT` (Linux-compat) | OK | |

### Known limitations
- TODO SMP (multi-core)
- TODO Copy-on-write for fork (today full page copy; shm-backed regions DO share physical pages)
- TODO File-backed mmap of regular files (only anonymous + MAP_SHARED on shm fd)
- TODO Real X11/tinyX (Ox is its own IPC protocol; the ioctls + `<linux/fb.h>` are ready for a future port)
- TODO IPv6, epoll, kqueue, sendfile, inotify
- TODO PIE main / ET_DYN executable with random load offset (only the interpreter is ET_DYN)
- TODO RTLD_LAZY / dlopen / dlsym / transitive DT_NEEDED (musl exposes them but we haven't loaded additional libs)
- TODO `epoll_*` syscalls (lighttpd uses the `poll` fallback)
- WARN lighttpd with `&` in background fails with a strange busybox `sh: can't open '/dev/null'` — workaround: run foreground
- WARN TTY global shared across tasks (no real per-PTY) — mitigated with tcsetattr anti-clobber and echo via shared path
- WARN Single HW FP state for multiple tasks — per-task FXSAVE/FXRSTOR implemented but not extensively tested
- WARN sqlite3 with SQL in argv has residual argv passing issues (workaround: stdin redirect)
- WARN Clean sqlite3 exit can page-fault in musl atexit cleanup (cosmetic; doesn't affect ash thanks to the FS_BASE fix)
- WARN `SA_SIGINFO` handlers receive `siginfo_t *` = NULL (we don't populate the struct; apps with NULL-check are fine, apps that assume non-null may fault)
- WARN **oxnetsurf v1 — no box-model layout**: HTML5 parsing + real DOM (libhubbub + libdom) but rendering linearizes everything into flat columns (list of text + links). Stage 4 (libcss + netsurf-core) is out of scope until there's time.
- WARN **musl-linked Ox apps need `liboxshim`**: musl's `shm_open` wrappers point at `/dev/shm/` and `connect` doesn't retry on `EINPROGRESS` — the shim overrides with the correct syscall direction (519/520 SHM, retry-on-EINPROGRESS for connect). New musl-linked apps in Ox must be listed in the group with `NS_OX_SHIM_A` before `MUSL_LIB`.
- WARN **oxnetsurf outbound network limited**: outbound TCP connect to modern internet hosts sometimes stays in extended `EINPROGRESS` (~ping rate); the retry shim mitigates but doesn't guarantee sub-second completion. Same root cause as outbound `wget` (kernel non-blocking connect without completion via poll/select all-the-way). Real fix (B): kernel notifies via SIGURG or socket-readable when SYN_ACK arrives.

---

## Phase log (reverse chronological)

**Most recent first**. Each entry describes work, decisions, and
notable bugs encountered. For the full Spanish narrative see
[`STATUS.es.md`](STATUS.es.md).

| Phase | Subsystem | Approx LOC |
|------|-----------|-----------|
| **FASE 12.4 — Duktape JS runtime + NetSurf v1 port + BeOS-style WM + liboxshim** | (1) **`vendor/duktape/` (Duktape 2.7.0)** — ~30K LOC of the ECMAScript E5/E5.1 JS engine in single-header `duktape.c + duktape.h + duk_config.h`. Built against musl. Output `/bin/oxjs` ~600 KB static ELF. **Sixth self-host language**: C / Lua / jq / SQL / make / **JS**. (2) **`elfs/gui/oxjs.c` (~350 LOC)** — JS runner with `ox.window.*` bindings: `rect(x,y,w,h,rgb)`, `text(x,y,str,rgb)`, `clear(rgb)`, `present()`, `poll() -> ev_obj{type, x, y, kind, ascii, keycode}`, `log(...)`. Detects numeric argv to select script (`oxjs 3` = snake.js, `oxjs 4` = quadratic.js); otherwise defaults to `/home/oxjs/<arg>.js`. (3) **JS apps**: `/home/oxjs/snake.js` (~120 LOC) — classic game with 20x20 grid, growable tail, score, arrow keys; `/home/oxjs/quadratic.js` (~80 LOC) — visualizes `f(x)=ax²+bx+c` with interactive coefficient sliders (drag mouse). (4) **oxsrv menu entries** `JS: Snake / JS: Quadratic` -> `oxjs 3 / oxjs 4`. (5) **`vendor/netsurf/` Stage 1-3 (build only)** — NetSurf libraries vendored and built as `.a`: **libwapcaplet** (string interning, 21 KB), **libparserutils** (charset codecs, 384 KB), **libnsutils** (base64/time, 19 KB), **libnslog** (logger with bison/flex filter parser, 198 KB), **libhubbub** (HTML5 tokeniser + tree-builder, 1.2 MB), **libdom** (DOM tree + hubbub binding, 4.1 MB). Total ~6 MB of `.a` ready to use. Skip libcss + netsurf-core (saved hundreds of hours; v1 without box-model). (6) **Autogen pipeline in GNUmakefile**: perl generates `aliases.inc` (libparserutils charset table) + `entities.inc` (libhubbub HTML entities); gperf + sed generate `autogenerated-element-type.c` (libhubbub static element map); bison 3.8.2 (autodetect `/opt/homebrew/opt/bison/bin/bison`, macOS' 2.3 doesn't support `%destructor`) + flex with `--header-file=` and `--define=api.prefix={filter_}` generate filter-parser/lexer for libnslog. (7) **`elfs/gui/oxnetsurf.c` (~820 LOC)** — real HTML browser linked with `libdom + libhubbub + libparserutils + libnslog + libnsutils + libwapcaplet + BearSSL + liboxshim + musl` (8 `.a` files, ~10 MB grouped). Editable URL bar + blue caret + BeOS-style Go button; default URL `http://httpbin.org/html`. HTTP fetch via `socket+connect+read`; HTTPS via BearSSL with custom X.509 wrapper that accepts `BR_ERR_X509_{NOT_TRUSTED, TIME_UNKNOWN, EXPIRED}` (no-anchor trust). Pipeline `body -> dom_hubbub_parser_create -> parse_chunk -> completed -> recursive walk_dom`. Extracts text + headings (h1-h6 -> `# Title`) + list items + links (`<a href>` -> clickable spans). Link click resolves relative URL preserving https. Status bar `HTTP/HTTPS NNN bytes, M lines, K links`. Scrollable 900x600 render. Detailed logs to `/dev/ttyS0` via `nslog()`. (8) **`liboxshim`** (`res/netsurf-shim/errno_shim.c` + `lib/libc/{ox.c,ox_font.c,ox_icons.c,ox_text.c,ox_ui.c,ox_log.c}` recompiled with `MUSL_CFLAGS`): mini-libc declares `extern int errno` (global) while musl requires `(*__errno_location())`; the shim provides `int errno=0` strong-def + the archive `liboxshim.a` listed BEFORE `MUSL_LIB` in `--start-group` so the shim's definitions win the link resolve. (9) **Override `shm_open`/`shm_unlink`** in the shim: musl 1.2.5 implements `shm_open` as `open("/dev/shm/<name>")` (osnos doesn't mount `/dev/shm`); the shim emits direct `syscall(519/520)` (= `SYS_SHM_OPEN/SHM_UNLINK`). Without this, oxnetsurf created the window (IPC handshake OK) but `local_alloc` never tracked the backing -> all `ox_draw_*` were silent no-ops -> window with perfect titlebar and completely empty gray body. Confirmed via `llvm-objdump` that `shm_open` now emits `syscall #0x207`. (10) **Override `connect()`** in the shim with retry-on-EINPROGRESS+EAGAIN (10 ms x 500 attempts = 5 s cap): musl's `connect` doesn't retry (assumes blocking kernel), osnos returns `EINPROGRESS` non-blocking — mini-libc has its own retry loop (`lib/libc/inet.c:158`); the shim replicates that semantics for musl-linked code. Without this, the first attempt failed with `errno=11` and oxnetsurf aborted the fetch. (11) **BeOS yellow titlebar WM** (oxsrv.c) — yellow tab `#FCE06D` (classic BeOS R5) on the left side of the top, NOT full width (BeOS style where the tab only runs as far as it needs to). Three 12x12 black square buttons aligned to the right of the tab: zoom (corners), min (line below), close (X). Slim click on zoom triggers 2x scaled blit (no SHM grow). Extended `is_zoom/is_min` hit-test in `hit_title()`. Drag to move; double-click to focus. (12) **BeOS-style top-right deskbar WM** (oxsrv.c) — dark grey strip at `(scr_w-280, 0)`. App menu button `≡` on the left + window-tile-strip (3 letters of the title per window, darker if minimized) + HH:MM clock on the right with 60s refresh. (13) **Yaru icon `netsurf` symlink -> `org.gnome.Epiphany.png` -> `webbrowser-app.png`** via `tools/fetch_icons.sh` (follows git symlinks up to 5 levels); copies to `/home/.icons/netsurf.rgba`. (14) **Tasks completed**: #40-44 (Duktape + oxjs + bindings + samples + menu), #47-51 (oxterm backspace + arrows + audit + oxsqliteview + oxjs logs + deep bug audit + oxterm deep dive), #52-56 (NetSurf Stage 1/2/3 + oxnetsurf HTTP+DOM+render + menu+icon). 1 pending: #45 (ox.fs / ox.http / ox.sqlite bindings — future). | **~4500** |
| **FASE 12.3 — Ox WM BeOS/Haiku look + liboxui + 5 new apps + resize protocol + Ghostty scrollback** | (1) Haiku-style menu in oxsrv: cream background `#e8e8e8`, hard black 1px border, **no shadow**, **no rounded corners**, Haiku-blue selection `#6698cb` with white fg. (2) **TTF font loader** (`lib/libc/ox_text.c`, ~310 LOC): vendored `vendor/stb/stb_truetype.h` (public domain, single-header). Glyph cache alpha8 with 24x28 max dimensions, Porter-Duff "over" alpha-blend on BGRA. `ox_text_init("/home/.fonts/default.ttf", 12)` loaded at oxsrv boot. Transparent fallback to 8x8 bitmap if TTF missing. `make fetch-fonts` downloads DejaVu Sans 2.37 (757 KB, Bitstream Vera license) from the official repo via curl. (3) **Color icon system** (`lib/libc/ox_icons.c` + `tools/fetch_icons.sh`): 14 24x24 RGBA icons copied to `/home/.icons/<key>.rgba` from the **Yaru theme (Ubuntu, GPL-3)** via `fetch_icons.sh` (follows git symlinks + ImageMagick resize + RGBA8888 raw). On-demand loader with cache (max 16 slots). `ox_icon_draw_rgba` alpha-blend. Deskbar tiles now show real color icons (calc with green keypad, terminal black, etc.) — used to be 3 chars or mono drawings. Menu items too. (4) **Critical Makefile bug — libc built with kernel CFLAGS**: the generic rule `$(BUILD)/%.c.o: %.c` (kernel) won over the more specific libc pattern by declaration order in GNU make. Worked accidentally when previous-build `.o`s were on disk. Fresh build -> `-mcmodel=kernel`, no include path, `'dirent.h' file not found`. Fix: explicit static pattern rule for `LIBC_C_OBJS`. (5) **Critical Makefile bug — macOS ar/objcopy**: `/usr/bin/ar` invokes BSD ranlib which rejects ELF objects ("not a mach-o file") and leaves `libosnos_c.a` empty. `objcopy` doesn't exist on macOS. Fix: switch to `llvm-ar` + `llvm-objcopy` with autodetect via `command -v` (fallback `/opt/homebrew/opt/llvm/bin/`). (6) **`liboxui`** (`lib/libc/ox_ui.{c,h}`, ~600 LOC): mini widget toolkit — `ox_button_t` (hover/pressed), `ox_label_t` (left/center/right align), `ox_listview_t` (mouse + keyboard nav + wheel), `ox_scrollview_t` (clip rect + scrollbar drag + page-click + wheel + arrow keys). Plus dialogs: `ox_msgbox_t` (modal OK box) and `ox_filepicker_t` (opendir-based file picker with back/up navigation, dirs-first sort, double-click cd, BACKSPACE = up). (7) **5 new apps** (`elfs/gui/`): **oxlog** — generic log viewer with ScrollView + F5 reload, argv[1]=path; **oxmem** — dual-panel (`/sys/meminfo` + `/sys/tasks`) auto-refresh 1s; **oxipc** — dual-panel (`/sys/services` + `/sys/tasks`); **oxnet** — dual-panel (`/sys/net` + `/sys/arp`). Wired to the Deskbar menu. (8) **oxsqliteview file bar**: toolbar added editable DB path input + Open button (Enter to reload) + `...` button that opens a modal `ox_filepicker_t` with double-click to open a new DB without relaunching the app. (9) **oxterm Ghostty-style scrollback**: 256-row ring buffer; wheel + PageUp/PageDn navigate; ESC/End or any input returns to live. **Catppuccin Mocha palette** (`#1e1e2e` bg / `#cdd6f4` fg / `#f5c2e7` cursor pink) replaces Adwaita green-on-dark. Full-block cursor with glyph on bg (true Ghostty inverse). Padding CELL_H 12->14, MARGIN 4->8. Status strip when scrolled showing `-N/total`. (10) **Resize protocol scaffolded — and disarmed** (`osnos_ipc_abi.h` + `lib/libc/ox.c` + `oxsrv.c`): `IPC_OX_EVENT_RESIZE=0x70` defines the wire format (arg0=win_id, arg1=w<<32\|h, data=new SHM name); `OX_EV_RESIZE` event type + `new_w/new_h` + `ox_window_dims()`. Initially active — drag-resize and zoom_slot triggered real SHM swap. **Bug reported**: when the buffer grew, apps that didn't recompute layout drew with the old WIN_W in the new stride -> diagonal lines / app "disappeared"; back to original size looks fine. **Mitigation**: `resize_window` now clamps to `buf_w/buf_h` (new fields = physical backing dimensions), does NOT swap SHM, and zoom_slot reverts to Phase A's 2x scaled blit. The IPC protocol stays in the ABI ready to re-arm when all apps opt in. (11) **`ox_text_draw` replaces `buf_draw_text` in oxsrv** — all chrome paths (titlebar, menu, deskbar) now use anti-aliased proportional text with auto bitmap fallback. (12) DESKBAR_H 26->32 and MENU_ITEM_H 26->28 / MENU_W 200->220 to accommodate 24x24 icons with padding. | 2400 |
| **FASE 12.2 — Ox performance + premium fluidity (SHM-backed windows working end-to-end)** | (1) **Root cause of "lag on close + Settings without thumbs"**: the `IPC_OX_PRESENT` handler in oxsrv had a legacy `if (g_wins[slot].dirty)` check inherited from pre-SHM (when each `DRAW_RECT/TEXT/IMAGE` IPC set the flag). After the SHM refactor, draws are local writes -> the flag is NEVER set -> `mark_dirty` never fires -> composite is skipped. Settings loaded thumbs into SHM but the screen didn't refresh until an external event (open/close of another window) forced a full repaint that incidentally repainted settings. Same bug for "everything feels laggy after closing an app": any redraw of the remaining open apps was ignored, cursor over windows with stuck content. Fix: PRESENT always marks dirty (no flag check). (2) **PTE_SHM bit** (kernel `vmm.h` + `syscall.c` + `vmm.c`): new software AVL bit 9 in PTEs. `sys_mmap` shm path + `address_space_clone` fork shm fixup set the bit. `address_space_destroy` skips `pmm_free_page` for PTEs with PTE_SHM — those pages are owned by `shm_obj`, freed by the last `shm_unref`. Without this, client exit returned to the PMM pages that oxsrv still had mapped -> framebuffer corruption + latent double-free. (3) **`task_reap_dead` defensive IPC cleanup**: in addition to `ipc_drop_for_pid` in `proc_exit_current_user`, the reaper does a second pass when recycling the slot — closes the race window where an IPC arrives between the drop and the state-flip to ZOMBIE. (4) **Filtered `task_wake_pollers`**: only wakes tasks BLOCKED with `saved_rax == SYS_POLL` or `SYS_IPC_SEND` (vs all). Avoids thundering herd when a mouse push woke consrv that was BLOCKED on IPC recv. (5) **Menu dirty-rect in oxsrv**: 5 menu sites (right-click open, F1 toggle, hover, item pick, click outside) set `g_dirty=1` without calling `mark_dirty(...)` -> fell into the full path (~12 MB memcpy + blit). New `mark_menu_dirty()` helper marks only the menu bbox. (6) **vmm_unmap cleans intermediate PT pages bottom-up**: walks PT/PD/PDPT freeing empty levels. Without this, mmap/munmap cycles in long-running processes (oxsrv window backings) leaked ~4 KB per cycle. (7) **`framebuffer_blit_kernel` row-memcpy**: went from pixel-by-pixel volatile loop to `os_memcpy` per row (~10x faster in QEMU). (8) **SHM bumped 16/256 -> 32/1024**: 4 MiB max per object, 32 objects. Accommodates oxsettings 720x560 (1.6 MiB) + 10 thumbnails + concurrent windows. (9) **`fd_readable` for `/dev/mouse0` and `/dev/input0`**: checks the ring level via `devfs_mouse_has_data()`/`devfs_input_has_data()` — used to always return true, causing `sys_poll` to return immediately without data. (10) **Heartbeat 2s -> 5s**: the `write(ttyfd, hb, ~250)` to UART COM1 blocks ~22 ms per byte due to byte-by-byte busy-loop in `serial_putc`. At 5 sec the visible freeze is <0.5% of the time. (11) **Instrumented diagnostics** in oxsrv heartbeat: per-trigger counters of fulls (alloc/destroy/raise/reload/other), timing in ms (`t_full_ms` / `t_dirty_ms` / `t_destroy_ms` with max), iters/sec, ev/s(m/k/i), avg_px dirty rect, last full reason. This data confirmed composite is <1ms in QEMU and ruled out the compositor as the bottleneck — the real problem was the PRESENT flag bug. (12) **IPC_PROC_EXITED leak to shellsrv** (`proc/exec.c`): `proc_exit_current_user` always sent an `IPC_PROC_EXITED` to `SERVER_SHELL` when any task died. But shellsrv runs in background blocked on `read(stdin)` and never drains its IPC queue. Each close of an Ox app left 1 stuck message (parent=oxsrv, not shellsrv — shellsrv doesn't even need it). Diagnosis via the new metrics: `ipc` grew 1->2->3->4 with each close, and simultaneously `iters` dropped from 30Hz to 4Hz + `ev/s(m=)` collapsed from 30 to 4 while the cursor was still moving. Secondary cause: each `ipc_send` calls `task_unblock(target_pid)` -> shellsrv woke spuriously on each IPC, burning scheduler dispatches on useless wake-block cycles that are expensive in QEMU TCG. Fix: only emit IPC_PROC_EXITED if `service_get_pid(SERVER_SHELL) == t->parent_pid` (i.e., shellsrv IS the parent of the dying task). User verified: cursor maintains 30Hz post-close, ipc=1 stable. | 550 |
| **FASE 14.5 polish — Catchable Ctrl+C + TIOCSPGRP + SA_SIGINFO null-args** | (1) `kill_pending` honors user handler: previously `proc_exit_current_user(128+sig)` was always called if kill_pending=1. Now if the app installed a handler (sa_handler != DFL != IGN) and sig != SIGKILL, fall-through to signal delivery loop to invoke the handler. (2) `TIOCGPGRP`/`TIOCSPGRP` ioctls: busybox ash calls `tcsetpgrp(STDIN, pgid_of_fg_job)`; without these ioctls it failed with ENOTTY and `kernel_fg_pid` stayed 0, tty_signal silently dropped. Now `tcsetpgrp` updates `kernel_fg_pid` and Ctrl+C routes correctly. (3) SA_SIGINFO compat: 3-arg handlers `void h(int, siginfo_t *, void *)` read rsi/rdx with garbage from the syscall -> page fault at the first `movups (rsi+0x70)`. Fix: zero buf[8]/buf[9] (rdx/rsi = NULL) in signal delivery. Apps with a NULL-check (lighttpd cmovneq) use the fallback. (4) Verified: lighttpd Ctrl+C -> graceful shutdown (exit=0). | 60 |
| **FASE 14.5 — lighttpd 1.4.76 port (real HTTP server)** | (1) `vendor/lighttpd/` (124 .c, ~106K LOC) without autotools/cmake: hand-craft `build-osnos/config.h` (30 HAVE_* matching musl), `plugin-static.h` (10 builtin mods), host-compiled `lemon` generates `configparser.c`. (2) fdevent backend = poll (not epoll). (3) Output `/bin/lighttpd` 1.85 MB static ELF. (4) Kernel fix #1 — sys_read/write dispatch AF_INET: was an omission; the old httpd used sendto/recvfrom directly, lighttpd uses standard read/write. Fix: branch sock_recv/sock_send in sys_read/write too. (5) Kernel fix #2 — permissive sys_setsockopt: now accepts no-op success for all flags under SOL_SOCKET/IPPROTO_TCP/IPPROTO_IP. (6) Config seeded in `/etc/lighttpd/lighttpd.conf`, alias `lighttpd='lighttpd -f /etc/lighttpd/lighttpd.conf'` in `.ashrc`. (7) Verified: `curl http://localhost:8080/` -> HTTP 200 OK + body, multiple paths (`/index.html`, `/hello.c`, `/demo.sql`). | 350 |
| **FASE 14.4 — Dynamic linking via ld-musl.so** | (1) musl rebuild with shared: `./configure` without `--disable-shared`; `lib/libc.so` (882 KB DYN ELF) serves as both libc.so and dynamic linker (`ld-musl-x86_64.so.1`). Manual `ld.lld` link (clang choked on `-Wa,--noexecstack`). (2) compiler-rt stubs (`__mulxc3`/`__mulsc3`/`__muldc3`) linked to libc.so so ld.so doesn't report undefined symbols. (3) `elf_load_dyn(main, interp)` + `elf_get_interp`: detects PT_INTERP in main, loads interpreter at `INTERP_LOAD_BASE=0x40000000`, returns `elf_load_result_t` with e_entry, phdr_user_va, phnum, phentsize, interp_base. (4) Extended auxv (8 pairs): AT_PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY/RANDOM/NULL. (5) `proc_execve_replace_argv` detects PT_INTERP and routes to `elf_load_dyn` + `build_argv_block_argv_dyn`. (6) sd.img bump 32 -> 64 MiB to accommodate duplicated libc.so. (7) `elfs/tests/hello_dyn.c` verified: `/bin/hello_dyn` -> "hello from dynamic linker on osnos!". | 700 |
| **FASE 14.3 — POSIX SHM (`shm_open` + `mmap MAP_SHARED`)** | (1) `src/micro/shm.{c,h}` (~170 LOC): pool 16 objects x 256 pages = 1 MiB. State `refcount + unlinked` (POSIX: persists until unlink + last close). (2) Extended OFD with `is_shm + shm_ref`. (3) Syscalls `SYS_SHM_OPEN=519`/`SYS_SHM_UNLINK=520`; `sys_ftruncate` dispatches to `shm_truncate`. (4) `sys_mmap` with MAP_SHARED fd-backed: vmm_map the shm_obj's physical pages without pmm_alloc, `shm_backed=1` in `mmap_regions` so munmap only does vmm_unmap. (5) Critical `sys_fork` fix: `address_space_clone` deep-copied shm pages; fix re-maps the parent's original phys. Without this, child writes were invisible to the parent. (6) mini-libc gap-fill: `ftruncate` wrapper + `shm_open`/`shm_unlink`. (7) `elfs/tests/shmtest.c` verifies cross-fork shared round-trip. | 250 |
| **FASE 14.2 — AF_UNIX SOCK_STREAM** | (1) `src/include/osnos_unix_abi.h` + `lib/libc/include/sys/un.h`: Linux-compat sockaddr_un layout. (2) `src/micro/unix_sock.{c,h}` (~270 LOC): pool 32 sockets + 16 bound paths, 4 KiB ring buffers per dir, backlog 8. States UNUSED/UNBOUND/LISTENING/CONNECTED/DISCONNECTED. No abstract namespace or SOCK_DGRAM. (3) Extended OFD with `is_unix_socket + unix_idx` parallel to is_socket. (4) Dispatch in syscalls: sys_socket/bind/listen/connect/accept/read/write/sendto/recvfrom/fd_readable branch by family. (5) Extended errno: EISCONN=106, ENOTCONN=107. (6) `elfs/tests/unixtest.c` verifies PING/PONG roundtrip parent<->forked child. | 300 |
| **FASE 14.1 — POSIX make (pdpmake) — self-hosting build** | (1) `vendor/pdpmake/` 1.4.1 (~3.4K LOC) against mini-libc -> `/bin/make`. (2) mini-libc gap-fill (`posix_extras.c`): getopt, stpcpy, popen/pclose, utimensat stub. New headers `<strings.h>`, `<glob.h>` (GLOB_NOMATCH stub), `<ar.h>`. `<sys/stat.h>` redesigned with `st_atim/mtim/ctim` (struct timespec) + legacy macros `st_atime` -> `st_atim.tv_sec`. (3) `resolve_path` helper in sys_open/stat/access/mkdir/rmdir/unlink/rename/chdir — relative paths resolve against `task->cwd`. (4) exec preserves cwd if already set (fork+exec case); used to reset it always. (5) GNU getopt convention: `optind=0` = "reset + start at argv[1]" (without this pdpmake said `make: don't know how to make make`). (6) `/bin/sh` = copy of busybox (busybox dispatches by argv[0]). (7) **Critical bug — sys_execve flattened argv into a string + re-tokenized**, breaking args with spaces: new `proc_execve_replace_argv(path, argv[], envp)` + `build_argv_block_argv` that consume array directly. (8) **Real `SYS_CLONE`** with CLONE_VM + CLONE_VFORK for musl `posix_spawn`. PML4 sharing via lookup-refcount. (9) **execve resets `sa_handler[]` to SIG_DFL** (POSIX violation fix — child used to inherit handlers from ash living in busybox's text). (10) Verified: `cd /home && make hello && /home/hello` end-to-end. | 800 |
| **FASE 13.3 — SQLite 3.45.2 port (fourth self-host language: SQL) + deep bug fixes** | (1) `vendor/sqlite/` amalgamation (sqlite3.c ~250K LOC + shell.c + sqlite3.h). Linked against musl. Output `/bin/sqlite3` ~5 MB static ELF. (2) 4 new syscalls: `SYS_FSYNC=74`/`FDATASYNC=75` (stubs, FAT16 is already sync), `SYS_FTRUNCATE=77` (real, via vfs_read+pad+rewrite), `SYS_GETTIMEOFDAY=96` (alias of clock_gettime with conversion), `SYS_GETRANDOM=318` (xorshift PRNG seeded by timer). (3) Extended `sys_fcntl`: F_SETLK/F_GETLK/F_SETLKW/F_OFD_* return 0 (single-process, advisory locks don't apply); F_DUPFD_CLOEXEC mapped. (4) Bumps: `EXEC_VFS_BLOB_MAX` 2 -> 16 MiB; `KHEAP_MAX_BYTES` 4 -> 32 MiB (sqlite ELF 5 MB didn't fit). (5) SQLite CFLAGS: THREADSAFE=0, OMIT_LOAD_EXTENSION, OMIT_WAL, DEFAULT_LOCKING_MODE=1 (exclusive), DEFAULT_TEMP_STORE=2 (memory), NO_SYNC=1, DEFAULT_MMAP_SIZE=0. (6) `res/demo.sql` + `res/demo.db` shipped to `/home/demo.db` (15 books + 4 users + 6 checkouts + view + indices). (7) **Critical bug #9 — FS_BASE save/restore + reset on execve**: `arch_prctl(ARCH_SET_FS)` writes MSR_FS_BASE globally on the CPU; without per-task save/restore, ash inherited sqlite3's FS_BASE and page-faulted in `__errno_location` post-wait. Three patches: (a) `uint64_t fs_base` in task_t + rdmsr/wrmsr in task_run_next; (b) reset to 0 in proc_execve (both paths — task_create_user_elf + in-place exec); (c) copy parent's to child's in sys_fork. **Without these 3, NO musl-linked program spawned from ash survived its parent**. (8) **Bug #10 + #11 — echo and backspace in REPL**: `tty_echo_char` used `framebuffer_draw_string` directly (no serial mirror; different cursor from apps' path via consrv). Fix: use `framebuffer_write_bytes` (same cursor, same serial mirror). `tty_echo_erase` same: sequence `"\b \b"` via write_bytes (cursor back, overwrite with space, cursor back). **Without these fixes, REPLs of sqlite/lua had functional stdin but ZERO visual echo** — user was typing blind. (9) Improved page_fault log: added task name + pid + cr2 + rip (`*** task 'busybox' pid=6 killed: Page fault cr2=0x... rip=0x...`) — critical to diagnose #9 (discover that ash, not sqlite3, was faulting). (10) End-to-end verification: `sqlite3 :memory: < q.sql` with `SELECT 99` -> `99`; `sqlite3 /home/demo.db` interactive REPL with `.tables`, `SELECT title FROM books`, `.quit` all with visible echo + backspace; ash survives multiple sqlite3 runs without respawn. **Fourth self-host language**: C + Lua + jq + SQL. | 900 |
| **FASE 13.2 — BusyBox rebuild with history file + ~30 new applets** | (1) Critical wrapper bug `osnos-cc-wrapper.sh`: compile mode didn't pass `-target x86_64-unknown-none-elf` -> clang on macOS produced native Mach-O ARM64, not x86_64 ELF. ld.lld rejected with "unknown file type". Fix: added `-target` + `-U__APPLE__ -D__linux__` (avoids the BSD branch of `include/platform.h` which requires macOS-only `<machine/endian.h>`) + musl includes injected via `-isystem` (busybox doesn't pass them by default) + filtered clang-only flags from the link path (`-finline-limit`, `-falign-*`, `-Wp,*`) + separate branch for preprocess (`-E -xc -MM -dM`). (2) Updated `.config`: `FEATURE_EDITING=y` + `EDITING_HISTORY=500` + `EDITING_SAVEHISTORY=y` + `EDITING_SAVE_ON_EXIT=y` + `EDITING_FANCY_PROMPT=y` (PS1 `\w` expansion) + ~30 new applets. (3) STANDALONE_SHELL disabled (multi-arg dispatch bug). Instead `/home/.ashrc` defines `alias vi='busybox vi'`, etc — FAT16 doesn't support symlinks so the Linux-style "/bin/vi -> /bin/busybox" approach doesn't apply. (4) `history` builtin + cross-reboot persistence. (5) Verified: `sed s/x/y/`, `awk -F: ...`, `find -type f`, `stat /home/README.TXT`, `base64`, `md5sum`, `bc -e "5*5"` functional. Original roadmap's FASE 12 TUI superseded — BusyBox covers vi/less/sed/awk/find/etc. | 800 |
| **FASE 13.1 — BusyBox ash as init shell + login mode + .bashrc-style /home/.ashrc** | (1) `vendor/busybox/` — BusyBox 1.36.1 vendored, linked against musl via `osnos-cc-wrapper.sh`. (2) **Critical bug #1 — restart_syscall pattern**: `sys_read` + `sys_poll` looped with `sys_nanosleep()`; but nanosleep does `sched_resume_jump()` (longjmp to scheduler) and leaves the task with `saved_rax=0` pointing to user-space RIP POST-syscall. ash called read(0), kernel longjumped, ash got read=0 -> EOF -> exit(0) -> watchdog respawn -> infinite loop. Fix: `block_restart_syscall(wakeup_ms, syscall_nr)` stamps iret frame with `rip -= 2` + `saved_rax = syscall_nr`. CPU re-executes the syscall on wakeup — POSIX restart_syscall pattern. (3) **Critical bug #2 — syscall number collision**: osnos lived in 260-268; collided with Linux #262=newfstatat (which musl `stat()` invokes). Moved to 510-518. New mappings: `SYS_LSTAT=6`, `SYS_OPENAT=257`, `SYS_NEWFSTATAT=262`, `SYS_EXIT_GROUP=231`. (4) **Bug #3 — `sys_stat` faulted on short paths**: `copy_from_user(kpath, path, OSNOS_PATH_MAX)` asked for 128 bytes; fix: byte-by-byte copy until NUL. (5) **Bug #4 — `VFS_MAX_MOUNTS=8` insufficient**: with 9 mounts `/home` didn't fit. Bumped to 16. (6) Login shell + bash-style split: `proc_execve("/bin/busybox", "sh -l", envp)`. `/etc/profile` sourced ONCE -> exports + `ENV=/home/.ashrc`. `/home/.ashrc` sourced on each interactive shell (exact mirror of ~/.bashrc) -> green PS1 `osnos:\w# ` + aliases + banner. (7) Verified: ash survives as init shell, `echo $((100*7))=700`, `for i in a b c`, `ls /etc` via aliasfs, pipes, redir, glob, all POSIX. | 800 |
| **FASE 13.0 — musl libc port (opt-in second libc)** | (1) `vendor/musl/` — musl 1.2.5 (~140K LOC). `./configure --target=x86_64 --disable-shared` + `make -j4` compiles on the first try — zero patches to the upstream tree. Output `vendor/musl/build-osnos/lib/{libc.a, crt1.o, crti.o, crtn.o}`. (2) Kernel gaps closed: `SYS_WRITEV=20` (musl stdio via writev), `SYS_ARCH_PRCTL=158` (ARCH_SET_FS -> wrmsr MSR_FS_BASE), `SYS_SET_TID_ADDRESS=218`. (3) Extended `build_argv_block` with minimal auxv `[{AT_PAGESZ=6, 4096}, {AT_NULL=0, 0}]`. (4) `elfs/musl.lds` preserves init_array/fini_array + adds PT_TLS. (5) `elfs/tests/hello_musl.c` smoke test: crt1 boot + auxv parse + TLS wrmsr + argv pass-through + snprintf with `%f` + clean exit. (6) `GNUmakefile` `USER_ELF_MUSL_SRCS` + pattern rule. Milestone: two libcs coexist, clear path to porting real POSIX apps. | 200 |
| **FASE 12.1 — GUI UX polish + watchdog + full ANSI** | (1) `/bin/uxsh` mini-shell for oxterm. (2) oxnotepad accepts argv[1]. (3) Full ANSI parser in oxterm: state machine ESC->CSI->final; SGR truecolor, cursor pos, erase. Cell grid `{ch, fg, bg}`. (4) `/bin/oxfiles` file browser: opendir + click-to-cd / click-to-edit. (5) libc stdio EAGAIN retry (drain_write 200x1ms). (6) Watchdog auto-resume in consrv + kbdsrv (defense against kill -9 of oxsrv). (7) oxsrv mouse MOVE coalesce to 1/frame. | 600 |
| **FASE 12.0 — Ox mini-X window system** | (1) Linux-compat kernel framebuffer ioctls: `FBIOGET_VSCREENINFO`, `FBIO_BLIT`. (2) Ox ABI: `SERVER_OX=5`, IPC range `0x60-0x7F`, 14 opcodes. (3) libc client (`lib/libc/ox.{c,h}`): mini-Xlib-style API. (4) `/bin/oxsrv` (~700 LOC): registers SERVER_OX, opens /dev/fb0 + mouse0 + input0, BGRA full-screen backbuffer + parse PPM. Loop: drain -> recompose (wallpaper -> window stack -> menu -> cursor) -> single `FBIO_BLIT` per dirty frame. Events: title click=focus/drag/close; right-click wallpaper or F1=Openbox-style root menu; Alt+F4=close; Alt+Left=cycle focus. Settings via `/home/.oxrc`. (5) GUI apps (5 x ~250 LOC): oxnotepad, oxcalc, oxterm (PTY+minishell), oxsettings. (6) Wallpapers generated at build (PNG if present, otherwise procedural). (7) sd.img 16 -> 32 MiB + `mformat -c 8` (FAT16 cluster count <65525). (8) FAT case-sensitivity decision: case-insensitive + case-preserving via LFN. | 2200 |
| **FASE 11.4 — PS/2 mouse driver + `/dev/mouse0`** | PS/2 polling driver (3-byte packets, sign extension, sync recovery), `mouse_server` kernel task pushes to 32-event ring. `/bin/mousetest` shows events live. Enabled the graphics line. PIC IRQ 12 still masked. | 250 |
| **FASE 11.3 — jq 1.7.1 port (third self-host language)** | jq vendored (~24K LOC) built with `-DWITHOUT_ONIG=1`. libc gap-fill: `alloca.h`, single-thread `pthread.h` shim, `libgen.h`, `memmem`, `isnormal`, `realpath`, `rand/srand`. **Critical bug**: `malloc(0)` returned NULL — glibc/musl return non-NULL. Fix: `if (size==0) size=1`. Without this fix jq crashed on the first `calloc(0, 24)`. `/home/test.json` shipped. | 350 |
| **FASE 11.2 — Lua 5.4 port (second self-host language)** | Lua 5.4.7 vendored (~24K LOC) without LUA_USE_POSIX -> ISO C fallback path. libc gap-fill: `locale.h`, `sig_atomic_t`, math (`asin/acos/sinh/cosh/tanh/frexp/modf`), time (`clock/mktime/difftime/strftime`), stdlib `system` stub. `/bin/lua` REPL + scripts. | 200 |
| **FASE 11.1 polish — FAT true append + offset-native + caching** | `fat_extend_existing` real cluster-chain extend (O(len) vs O(N) RMW). FAT-sector cache. BUFSIZ 512 -> 4096 in libc. TCC compile time **instantaneous**. `/bin/readelf -S` added. | 300 |
| **FASE 11.0 — TinyCC port + offset-native VFS reads (self-hosting tier)** | **HISTORIC MILESTONE**: osnos compiles C from inside. TinyCC 0.9.27 (~30K LOC) with critical patch: PLT32 -> PC32 direct relocation when static_link (without it, every libc call jumped to *NULL). sysroot on sd.img. **Critical bug #1 — sys_read truncated files >1024 B**: hardcoded stack scratch. Fix: offset-native VFS reads (`vfs_read_at`). **Critical bug #2 — fat_append_path truncated writes >8192 B**: hardcoded scratch. Fix: `kmalloc(existing+len)` cap 4 MiB. libc gap-fill (`ldexp`, `strtod/f`, `struct tm`, `localtime/gmtime`, `gettimeofday`, `fdopen`, `mprotect` noop, `sscanf`). `tcc hello.c -o hello && ./hello` end-to-end. | 900 |
| **FASE 10 — Servers to ring 3** | consrv + kbdsrv + shellsrv ring-3 ELFs replace the kernel-mode equivalents. IPC via service registry. Watchdog auto-restart. Critical refactor: **the kernel no longer has a UI** — everything is ring 3. | 1500 |
| **FASE 9 — Real preemptive scheduler CPL=3** | Timer-driven preemption (50 ms quantum) for ring-3 tasks. Ring-0 still cooperative. longjmp resume pattern from sys_exit / fault handlers. | 800 |
| **Pre-FASE 9 — POSIX core ABI** | Real fork(2) + execve(2) + wait(2) + sigaction(2). Process groups + sessions. OFD shared offsets. FD_CLOEXEC. PTY pairs. Automatic SIGCHLD. EINTR. WUNTRACED/WCONTINUED. Anonymous mmap + brk. Multi-stage pipes + O_NONBLOCK. Per-task FXSAVE/FXRSTOR. Shell pipes `\|`, redirection `> >> <`. Self-tests: 23/23 PASS. | 4000 |
| **Pre-FASE 9 — Networking** | Full TCP/IP stack: ARP + IPv4 + ICMP + UDP + TCP. RTL8139 driver. POSIX sockets. DNS. `/bin/httpd`. Beej's selectserver verbatim. | 3500 |
| **Pre-FASE 9 — Real FAT16 disk** | block_ata PIO. FAT16 read/write. Persistent /home, /etc via aliasfs. sd.img pre-populated at build. | 2000 |
| **FASE 8 — Earlier base** | Robust kheap, TTY line discipline + termios, env passing + PATH, shell rc + history, job control (Ctrl+Z/fg/bg/jobs), `/bin/ovi` modal vim-style editor, getcwd/chdir, mmap. Total kernel + libc pre-FASE-11: ~25K LOC. | 8000 |

---

## Current inventory (post-FASE-12.4 snapshot)

- **Kernel ELF**: ~1.8 MB stripped (`build/kernel`)
- **sd.img**: **128 MiB** FAT16 (4 KiB clusters), **115 ELFs in
  `/bin/`** + full sysroot in `/lib/` (libc.a + crt + libtcc1.a +
  **libc.so + ld-musl-x86_64.so.1**) + `/usr/include/`. Bump
  64 -> 128 MiB accommodates NetSurf libs (~6 MB of `.a`s) +
  Duktape (`/bin/oxjs` ~600 KB) + lighttpd (1.85 MB) + sqlite3
  (5 MB) + busybox 1.45 MB.
- **Ox apps (15)**: `oxnotepad oxcalc oxterm oxfiles oxtop
  oxsettings oxhexedit oxbrowser oxsqliteview oxlog oxmem oxipc
  oxnet oxnetsurf oxjs` wired into the BeOS-style menu. **+14 JS
  apps**: `hello clock paint sysinfo weather notes db_demo
  fs_explorer calc colors bench lab snake quadratic` reachable
  from the menu as `JS: ...` (loaded by oxjs from
  `/home/apps/*.js`).
- **NetSurf libs (.a)**: libwapcaplet 21 KB + libparserutils 384
  KB + libnsutils 19 KB + libnslog 198 KB + libhubbub 1.2 MB +
  libdom 4.1 MB + BearSSL ~600 KB + liboxshim 260 KB.
  `--start-group` total ~7 MB.
- **Bootable ISO**: ~22 MB (`build/osnos-x86_64.iso`)
- **Expected total memory**: 2 GiB of RAM (`-m 2G` in QEMU)
- **Boot time**: ~3-4 seconds (kernel + spawn servers + ash banner
  + oxsrv).
- **Automated tests**: **21/21 PASS** via `/bin/alltest`
  (kerntest, fork/wait/sig/sigchld/pgroup/spawn/exec/ofd/pty/
  fdedge/job/term/serial/tcc/lua/jq/libc/**unix/shm/hello_dyn**);
  each test with a 60s timeout so a hang doesn't block the suite.
  Ox/JS/NetSurf tests are not automated (would need frame-grab +
  diff, out of scope).

---

## Future roadmap

### FASE 14 — Complete self-hosting (in progress plan)

Goal: be able to `cd /home && make hello && ./hello` from inside,
then build the rest incrementally.

#### FASE 14.1 — `make` port (pdpmake) — **CLOSED — self-hosting build works end-to-end**

`cd /home && make hello && /home/hello` compiles and runs a
tcc-generated ELF from inside osnos. Verified: output `hello from
tcc on osnos!`.

Work (~6 cascading changes, all critical):

- `vendor/pdpmake/` — pdpmake 1.4.1 (POSIX make, public domain,
  ~3.4K LOC) vendored and built against mini-libc -> `/bin/make`.
- Mini-libc gap-fill (`lib/libc/posix_extras.c` + headers):
  `getopt/optarg/optind/opterr/optopt`, `stpcpy`, `popen/pclose`,
  `utimensat` (stub), new headers `<strings.h>`, `<glob.h>`
  (stub `GLOB_NOMATCH`), `<ar.h>`, extensions to `<fcntl.h>`
  (AT_FDCWD, UTIME_NOW, UTIME_OMIT) + `<sys/stat.h>` redesigned
  to `st_atim/mtim/ctim` (struct timespec) with legacy macros
  `st_atime` -> `st_atim.tv_sec` (binary layout intact, Linux
  compat).
- `resolve_path` helper in `sys_open` + `sys_stat/access/mkdir/
  rmdir/unlink/rename/chdir` — relative paths resolve against
  `task->cwd`. The kernel used to reject paths without leading
  `/` with EINVAL, breaking `fopen("Makefile")` from pdpmake/tcc.
- exec preserves cwd fix (`src/proc/exec.c`): `proc_execve` used
  to reset `t->cwd = "/"` and read PWD from envp. But busybox ash
  doesn't export PWD consistently to the child's envp -> `cd
  /home && make hello` ended with cwd=`/`. Fix: if `t->cwd` is
  already set (normal fork+exec case), preserve it. Only seed
  when it comes empty (direct spawn from kernel).
- GNU libc getopt convention fix (`lib/libc/posix_extras.c`):
  pdpmake does `optind = 0` to reset getopt between calls
  (`GETOPT_RESET()`). GNU convention says "0 = reset + start at
  argv[1]". My getopt took 0 literally and consumed argv[0].
  Without this fix `make hello` said `make: don't know how to
  make make` (target = program name).
- `/bin/sh` = copy of `/bin/busybox` (FAT16 has no symlinks;
  busybox dispatches by argv[0]). pdpmake's `system()` invokes
  `/bin/sh -c "..."`.
- `/home/Makefile` + `/home/hello.c` seeded into sd.img as a demo
  of the `make hello` workflow.
- **Critical bug — sys_execve flattened argv into a string +
  re-tokenized**: `sys_execve` concatenated `argv[1..N]` into
  `args_kbuf` separated by spaces, then
  `proc_execve_replace -> build_argv_block` re-tokenized that
  string by whitespace. Result: `execve("/bin/sh", ["sh","-c",
  "echo HELLO"])` turned into argv=`["sh","-c","echo","HELLO"]`
  and `sh -c echo` ran echo with `HELLO` as `$0` (not as arg) ->
  empty output. This broke EVERY make recipe that passed commands
  with args via `system()`. Fix: new `proc_execve_replace_argv(
  path, argv[], envp)` + `build_argv_block_argv` that consume an
  argv ARRAY without tokenizing; `sys_execve` uses it directly
  preserving boundaries. (`build_argv_block` string-version
  remains for internal callers that pass already-tokenizable
  strings — `proc_execve` from kmain.)
- **Real `SYS_CLONE` with `CLONE_VM` + `CLONE_VFORK`**
  (`src/micro/syscall.c` + `src/proc/exec.c` + `src/micro/
  task.{c,h}`): for musl `posix_spawn`. When `flags & CLONE_VM`,
  the child shares `pml4` with the parent (refcount via lookup
  in `task_pml4_other_users`); `user_stack_top = child_stack`.
  When also `CLONE_VFORK`, parent is marked `TASK_BLOCKED` with
  snapshot of the syscall context, child runs first; at child's
  `proc_execve_replace_argv` or `proc_exit_current_user`, parent
  wakes (saved_rax = child pid). `address_space_destroy` on
  exit/exec is skipped if other tasks still use that pml4.
  Without `CLONE_VM` flags, trivial alias of `sys_fork`. **Without
  this, posix_spawn (which musl uses for `system()`) corrupted
  the parent's address space by sharing AS without refcounting**.

- **Critical bug — execve didn't reset sa_handler[] (POSIX
  violation)**: `proc_execve_replace[_argv]` didn't reset caught
  signal handlers to `SIG_DFL`. When ash forked for `make hello`,
  make inherited the sa_handler[] table — including busybox's
  `SIGCHLD` handler pointing to `signal_handler` in busybox's
  text segment (0x235787). When sh exec'd terminated, kernel sent
  SIGCHLD to make; iretq jumped to 0x235787 which is NOT mapped
  in make's address space -> page fault. Fix: in execve, iterate
  32 slots and reset any handler different from SIG_IGN to
  SIG_DFL (`t->sa_handler[i] = 0; t->sa_restorer[i] = 0`).
  Diagnosed via user stack dump in the page fault handler (saw
  rip=0x235787, restorer=0x25a179=`__restore_rt` in busybox via
  `llvm-objdump`).

End-to-end verification: `sh -c "echo a b c"` -> `a b c`; `cd
/home && make hello` -> tcc compiles WITHOUT SEGFAULT;
`/home/hello` -> "hello from tcc on osnos!"; `make clean` runs
the clean recipe. Only cosmetic pending item: mini-libc `/bin/rm`
doesn't support `-f` flag (independent, doesn't block FASE 14.1).

#### FASE 14.2 — AF_UNIX sockets — **CLOSED**

`socket(AF_UNIX, SOCK_STREAM)` + `bind(pathname)` + `listen` +
`connect` + `accept` + `read/write/send/recv` + `close` work
end-to-end. Smoke test `/bin/unixtest` does PING/PONG round-trip
between parent (server) and forked child (client) without real
networking involved. **Out of scope**: SOCK_DGRAM (datagrams),
`SCM_RIGHTS` (fd passing between processes via UNIX), abstract
namespace (`sun_path[0]==0` Linux extension), credentials
passing. Enough for xeyes/X11.

#### FASE 14.3 — POSIX SHM — **CLOSED**

`shm_open(name, O_CREAT|O_RDWR) + ftruncate(fd, size) +
mmap(NULL, size, PROT_*, MAP_SHARED, fd, 0)` works end-to-end,
including the critical case **shared memory across fork** —
child and parent see the SAME physical pages, not snapshot
copies. **Out of scope**: file-backed (non-shm) mmap of regular
disk files (future: needed for programs that mmap ELFs).
PROT_EXEC enforcement via NX bit (today every mmap is effectively
RWX). Resize after mmap (changing a shm that already has live
mappers breaks; requires client notification via signals).

#### FASE 14.4 — Dynamic linking (.so) — **CLOSED**

`/bin/hello_dyn` linked dynamic-musl boots via PT_INTERP, the
dynamic linker (`ld-musl.so` which in musl IS the libc.so)
resolves printf + libc symbols against `libc.so`, and main()
runs cleanly printing to stdout. **Out of scope**: PIE main
(ET_DYN executable loaded with random offset). RTLD_LAZY (lazy
bind via PLT trampolines — today everything relocates eagerly).
dlopen/dlsym (musl exposes them via libc.so but we haven't
tested loading additional dynamic libs). Multiple .so deps
(transitive DT_NEEDED).

**FASE 14.1-14.4 integration test**: `make hello && /home/hello`
OK; `shmtest: OK` OK; `unixtest: OK` OK; `hello_dyn` OK;
`sqlite3 SELECT 7*8 -> 56` OK; `lua print(1+2+3) -> 6` OK.

#### FASE 14.5 — lighttpd 1.4.76 port (real HTTP server) — **CLOSED**

`curl http://localhost:8080/index.html` -> `HTTP/1.1 200 OK`
with full headers and body served from `/home`. lighttpd builds
+ bind:80 + accept + read request + serve static + close — the
full HTTP path works.

**Limitations**: lighttpd in background with `&` fails due to
busybox `sh: can't open '/dev/null'` (weird busybox redirect
path — not lighttpd). Current workaround: run foreground or use
`osn_spawn` from another process. `server.upload-dirs` requires
a FAT16 directory; `/home` works, `/tmp` doesn't because there's
no tmpfs mount. PHP/CGI/FastCGI not built (would need
fork+execve+pipe roundtrips, would all work but scope creep).

#### FASE 14.6 — Ox extended or nano-X (pending)
Three possible paths, decision open:
- **A** — xeyes-via-Ox: native Ox client drawing two circles
  following the cursor. ~150 LOC. Demonstrates that the existing
  GUI infra is enough without bringing in X11.
- **B** — Vendor nano-X (~20K LOC) on top of FBDEV. Opens a real
  Xlib-like API. 1-2 sessions.
- **C** — Minimal X11 wire protocol bound to `/tmp/.X11-unix/X0`
  (AF_UNIX already there), translates to Ox. Allows unmodified
  Linux xeyes. Many sessions (X11 spec is enormous).

#### FASE 14.7 — `xeyes` (full-path test)
Depends on 14.6 (B or C).

#### FASE 14-misc — Minor quality of life — **CLOSED**

"One-shot" session: 8 items resolved without regressions. `alltest`
remains **21/21 PASS**. Verified end-to-end with FASE 14.1-14.5
integration tests. Includes:

- **Real per-PTY termios**: added `task_t.tty_termios_valid +
  tty_iflag/oflag/cflag/lflag/line/cc[19]`. `sys_ioctl TCGETS`
  for fd 0/1/2 snapshot of the global on first call + return of
  task's struct. `TCSETS/TCSETSW/TCSETSF` update task's struct +
  sync to global. On task switch (`task_run_next`), if incoming
  task has `tty_termios_valid=1`, restore via `tty_restore_from(
  struct)`. fork copies parent's struct. Each task now "sees"
  its own raw/canon/echo mode when dispatched.
- **sqlite3 argv passing**: resolved by the `sys_execve preserves
  argv boundaries` fix (FASE 14.1).
- **Page fault in musl atexit (clean sqlite3 exit)**: resolved
  by the accumulated fixes (FS_BASE rdmsr, catchable
  kill_pending, sa_handler reset).
- **Massive BusyBox application: 116 total applets (was 65)**:
  enabled `.config` + fixed the wrapper `osnos-cc-wrapper.sh`
  (filters `-Wl,-Map,*`, `--warn-common`, etc. that ld.lld
  rejects). Rebuild produces 1.45 MB binary. Aliases added to
  `/home/.ashrc`. **51 new applets**: networking (`wget`, `nc`,
  `ping`, `traceroute`, `ifconfig`, `netstat`, `route`, `arp`,
  `hostname`, `telnet`, `microcom`, `nslookup`, `ftpgetput`);
  archives (`tar`, `gzip`, `gunzip`, `zcat`, `bzip2`, `bunzip2`,
  `bzcat`, `xz`, `unxz`, `xzcat`, `ar`, `lzma`, `unlzma`);
  fs/perms (`chmod`, `chown`, `chgrp`, `ln`, `mkfifo`, `mknod`,
  `mktemp`, `mountpoint`, `sync`, `fsync`, `truncate`, `install`,
  `chroot`); process/user (`id`, `whoami`, `groups`, `who`,
  `users`, `tty`, `pidof`, `pgrep`, `pkill`, `watch`, `setsid`,
  `nice`, `nohup`, `nproc`, `time`, `last`); text/filter (`nl`,
  `od`, `split`, `comm`, `paste`, `join`, `fmt`, `expand`,
  `unexpand`, `shuf`, `yes`, `less`, `ed`, `uuencode`,
  `uudecode`, `ipcalc`). **Syscall stubs added** in syscall.c
  for `getpriority/setpriority`, `sched_setparam/get`,
  `sched_setscheduler/get`, `sched_yield`, `setrlimit/getrlimit`,
  `prctl`, `setresuid/gid`, `setuid/gid`, `sync` — all return 0
  (single-task no priority/perms). Without these stubs, `nice -n
  5 echo hi` gave ENOSYS.
- **Synthetic `/proc` filesystem** (`src/fs/procfs.{c,h}`, ~420
  LOC): mount on `/proc`. Top-level: `meminfo` (PMM stats),
  `uptime`, `loadavg`, `cpuinfo`, `stat`, `version`. Per-pid:
  `/proc/<pid>/{cmdline,comm,stat,status}` enumerating task
  table. `/proc/self` alias of the current task. `/proc/net/{
  dev,route,tcp,udp}` so `route -n`, `netstat -tan`, `ifconfig`
  (partial) can read net info. **Fixed bug**: trailing-slash
  form `/proc/<pid>/` also returns PROC_PID_DIR.
- **`/etc/resolv.conf` seeded** in sd.img with `nameserver
  10.0.2.3` (QEMU slirp DNS) + fallback 8.8.8.8. `/etc/hosts`
  extended with `10.0.2.2 host`.

#### FASE 14-misc-2 — SIOCGIF* network ioctls — **CLOSED**

`ifconfig` now shows full info of eth0 + lo. For Linux apps that
enumerate interfaces via ioctl. Includes `net_iface_ioctl()` in
`sys_ioctl` for the range `0x8910-0x8950` (Linux SIOCG*/SIOCS*),
plus `siginfo_t` real for SA_SIGINFO, `/dev/stderr` + stdin/
stdout/console, and tmpfs at `/tmp`.

#### FASE 14-misc-3 — Real networking (DNS resolves, ping responds) — **CLOSED**

Starting point: `nslookup google.com 10.0.2.3` said `can't
resolve` and `ping <ip>` didn't exist as a runnable ELF. After
this phase: `nslookup google.com -> 142.251.x.x` and `ping
8.8.8.8 -> 64 bytes from 8.8.8.8 ttl=255 time=30ms`. 21/21
alltest still PASS.

Three chained bugs broke musl's resolver, plus SOCK_RAW that
didn't exist:

- `sys_socket` accepts bundled `SOCK_CLOEXEC` + `SOCK_NONBLOCK`
  in `type`.
- `sys_recvfrom` UDP path preserves `src_ip`/`src_port`.
- `recvmsg(2)` + `sendmsg(2)` implemented (Linux syscalls 46/47).
- `SOCK_RAW` + ICMP echo for `ping`: new socket family.

**Known limitations**:
- `ping -c N` multi-packet sometimes stays at seq=0: pacing
  between pings (BusyBox uses `setitimer` + SIGALRM) probably
  doesn't respect our interval. Single-shot works perfect;
  multi-packet needs `setitimer` path review.
- `wget http://example.com/` resolves DNS but fails in TCP
  connect with "Operation in progress" — non-blocking
  `connect()` returning EINPROGRESS without completing.
  Out-of-scope for this phase.
- IPv6 unsupported (musl asks for AAAA, returns benign `Address
  2: (null)`).
- `sendmsg`/`recvmsg` only single-iovec.

### FASE 14-pendings — Quality of life remaining
- TODO Chip-8 emulator (last pending item from the original
  graphics roadmap)
- TODO Real `setitimer` (today's a stub) — blocks `ping -c N` and
  many timer-based loops.
- TODO Real non-blocking `connect()` with `EINPROGRESS` +
  completion via poll — blocks wget/curl HTTP outbound and
  unreliable oxnetsurf fetch.

### FASE 12.5 — NetSurf Stage 4: libcss + layout (pending)
- TODO Vendor libcss (~12K LOC) — CSS parser + selectors +
  computed styles.
- TODO Box-model layout (without bringing in full `netsurf-core`):
  block/inline flow, margin/padding/border, font metrics via
  stb_truetype (we already have it). Probably ~2K LOC new in
  `oxnetsurf.c`.
- TODO Image decode: libnsbmp + libnsgif + libnsbmp (PNG via
  lodepng?). Paint to SHM ox client.
- TODO Basic forms: `<input>` + `<form>` parse -> submit POST.
- TODO Back/forward history + bookmarks (same pattern as oxbrowser).

### FASE 12.6 — Massively expanded oxjs API surface — **CLOSED**
**Before**: 9 bindings (`window/clear/rect/text/present/onPaint/
onKey/onClick/onTick`). **Now**: ~70 bindings grouped in 12
sub-modules, all from a single mini-libc-linked ELF of ~2.9 MB.

- `ox.fs` (12 fn): `readFile/writeFile/appendFile/listDir/exists/
  stat/mkdir/unlink/rmdir/rename/chdir/cwd`.
- `ox.os` (10 fn): `exec` (popen->string), `system` (rc), `exit`,
  `getpid`, `getenv`, `setenv`, `sleep`, `usleep`, `hostname`,
  `argv`.
- `ox.http` (2 fn): `get(url)` / `post(url, body, contentType?)`
  -> `{status, body, headers}`. Pure HTTP/1.0 via
  `socket+connect+read`.
- `ox.net` (7 fn): `tcpConnect(host, port)`, `tcpListen(port)`,
  `accept(fd)`, `send(fd, data)`, `recv(fd, max?)` with EAGAIN
  backoff, `close(fd)`, `udpSend(host, port, data)`.
- `ox.draw` extras in ox.*: `line(x1,y1,x2,y2,col)` (Bresenham),
  `circle(cx,cy,r,col)` (filled mid-point), `pixel(x,y,col)`,
  `frame(x,y,w,h,col,thickness?)`.
- `ox.color`: `rgb(r,g,b) -> "#rrggbb"`, `hex(r,g,b) -> int
  0xRRGGBB`.
- `ox.sys` (4 fn): `sysread(path)`, `meminfo()`, `uptime()`,
  `tasks()` — to build JS monitors.
- `ox.time` (4 fn): `now()` (ms with frac), `epoch()`, `date()`
  (`"YYYY-MM-DD HH:MM:SS"`), `format(epoch, fmt)` (strftime).
- `ox.clipboard` (2 fn): `set(text)` / `get()` — goes through
  the Ox WM's global clipboard.
- `ox.log` (3 fn): `info/warn/error` — categorized prefix to
  `/dev/ttyS0`.
- `ox.syscall(num, ...args)` — invokes arbitrary syscall.
  Constants in `ox.syscall.{READ,WRITE,OPEN,...}` (30+ Linux
  x86_64 codes).
- `ox.sqlite` (2 fn): `exec(db, sql)` / `query(db, sql)` —
  popen-out to `/bin/sqlite3 -separator '\t'` and split by tab.
- `ox.ui` (2 fn): `msgbox(title, text)` and `prompt(title, text)`
  — synchronous modals that draw a BeOS-style panel over the
  current window and block in their own event loop until
  OK/Enter/Esc.
- `ox` core expanded: `title(str)`, `size() -> {w,h}`,
  `onMouse(ev)` receiving `{x, y, buttons, kind, wheel}` (richer
  than onClick).
- Sample apps in `res/apps/` -> `/home/apps/`: `hello`, `clock`
  (digital+analog), `paint` (drag, palette, save log), `sysinfo`
  (live `/sys/` monitor), `weather` (HTTP+JSON), `notes` (sticky
  notes with prompt + JSON storage), `db_demo` (SQL grid on
  /home/demo.db), `fs_explorer` (file browser with preview),
  `calc` (calculator with eval), `colors` (HSV grid with
  clipboard copy), `bench` (multi-test perf bench), `lab`
  (sampler of all modules), `snake`, `quadratic`. **14 apps**.
- Menu refactor in oxsrv: action=3 now takes `path` as bare JS
  name (`"snake"` -> `/home/apps/snake.js`). 14 `JS: ...`
  entries wired to the BeOS menu.
- `resolve_script` in oxjs: argv[1] accepts abs path, bare name,
  "name.js", or integer N (Nth `.js` in /home/apps
  alphabetical).

**No functional pending — current only limitation is
performance**: ox.http.get blocks the entire event loop during
the fetch, no async/promises. OK for simple apps ("press
reload"), but a browser-like app would need an integrated
mainloop.

### FASE 15.0 — Premium fluidity: IRQ input + idle HLT + regional compositor — **CLOSED**

Quality leap for interactive latency and compositor efficiency.
Built + verified on macOS host (see build notes below).

**Kernel — IRQ-driven input + idle:**
- OK **PS/2 keyboard + mouse via IRQ 1 / IRQ 12**
  (`src/drivers/ps2_irq.c`). The 8042 command byte enables both
  interrupt bits; the handlers reuse the existing
  `keyboard_poll`/`mouse_poll` state machines and push into the
  same `/dev/input0` + `/dev/mouse0` rings. The always-READY
  "keyboard"/"mouse" poll feeder tasks are gone; events arrive in
  microseconds instead of "next scheduler round".
- OK **Serial RX via IRQ 4 + 1 KiB software ring** (`serial.c`).
  The 16-byte HW FIFO overran whenever ring-0 stayed busy (with
  the GUI running, serial console input lost ~half its bytes —
  `oxterm &` arrived as `xem&`). IRQ4 drains the FIFO into the
  ring on arrival; `serial_try_getc` consumes the ring first.
  `serial-in` feeder now paces itself (wakeup_at_ms + 10 ms).
- OK **Idle HLT in the scheduler** (`scheduler_tick`): when no
  task is READY, `sti; hlt` until the next IRQ instead of
  busy-spinning (with a cli-guarded `task_any_ready()` recheck to
  close the lost-wakeup window). Host CPU while the GUI idles:
  ~1.4%.
- OK **`sys_poll` finite timeouts actually expire**. The
  `block_restart_syscall` mechanism re-executes the whole syscall
  on wake, so the deadline recomputed on every restart and
  `poll(50)` behaved like `poll(-1)` (froze the Deskbar clock and
  any timeout-paced loop). The deadline now persists in
  `task_t.poll_deadline_ms` across restarts (armed on first
  entry, disarmed on every return path).

**Ox compositor — regional repaints + frame pacing:**
- OK **Dirty-rect LIST (8 entries)** replaces the single global
  bounding box (which unioned far-apart damage — deskbar clock
  top-right + cursor bottom-left used to recompose ~the whole
  screen). Overlapping marks merge; on overflow the entry with
  least bbox growth absorbs the new rect.
- OK **Window create / destroy / raise / restore / minimize are
  regional** — they used to force a full-screen repaint (8-12 ms
  each). Verified live: a session of open + type + close shows
  `full=1` (only the boot paint) in the heartbeat.
- OK **Cursor motion fully decoupled** (`g_cursor_dirty`):
  restore-from-clean + redraw + blit union; never recomposes.
- OK **Body-blit clipping** (`g_clip_*`): replaying a window for
  a small dirty rect copies only the damaged rows/cols instead of
  the full body.
- OK **Frame pacing ~66 Hz** (`FRAME_MS 15`): events drain every
  iteration, composite+blit at most once per frame budget; poll
  timeout shrinks to the remaining budget when damage is pending.
- OK **Client damage rects in PRESENT** (ABI FASE 15.0):
  `IPC_OX_PRESENT arg1=1` + `data={x,y,w,h}` (window-relative)
  marks only that body region. New libc API `ox_present_rect()`
  (falls back to full present for degenerate rects / legacy
  scaled-zoom windows). `oxterm` adopted (body-only presents).
- OK Bug fixes: `g_wins[g_focus_slot]` bounds check on the
  pending-move path; Deskbar tiles now shrink (floor 14 px) so
  all 16 windows keep a clickable tile (used to silently drop
  past ~8).

**Build on macOS (gmake + llvm):**
- OK GNUmakefile guards against GNU make 3.81 (Apple's
  `/usr/bin/make`) — its first-match pattern-rule selection
  compiled vendor sources with kernel CFLAGS. Use Homebrew
  `gmake`.
- OK `AR` detection probes `/opt/homebrew/opt/llvm/bin/llvm-ar`
  BEFORE plain `ar` (BSD ar emits an empty symbol index for ELF
  members — every libc symbol came out undefined). All `ar rcs`
  recipes now use `$(AR)`.

**Tests:** `alltest` 18/21 after the phase (kerntest expectation
updated: the "keyboard" feeder task no longer exists by design).
Remaining 3 fails are sd.img staging issues unrelated to this
phase (tcc sysroot incomplete on macOS-built images; `hello_dyn`
linked against a host-path libc.so name; serialtest flaky under
alltest only) — tracked for FASE 15.1.

### FASE 15.1 — Interactive resize armed + tinyX (Xlib subset over Ox) — **CLOSED**

Premium-UI track (real window resize everywhere) + first slice of
the Linux X compatibility track. Verified live in QEMU headless
(screendump + serial heartbeat + PS/2 mouse driven via the QEMU
monitor).

**Ox server — drag-resize protocol armed (`elfs/gui/oxsrv.c`):**
- OK **`commit_resize()`** — the armed counterpart of the old
  clip-only `resize_window()`. For windows that opted in via
  `ox_window_create_resizable`, dragging the bottom-right handle
  now does the genuine thing: `swap_window_shm()` (fresh SHM at
  the new dims) + `IPC_OX_EVENT_RESIZE` so the client re-lays
  out. Live during the drag (100 ms throttle) + one final commit
  on release. Legacy windows keep the safe clip-only behaviour.
- OK **Content-preserving swaps**: `swap_window_shm` seeds the new
  buffer with the body colour and then copies the overlapping
  region of the old backing, so live resize shows the existing
  content instead of a blank flash until the client repaints.
  Safe vs. queued stale events: kernel SHM refcounts unlink
  (client mappings survive), and the libc side skips a remap whose
  SHM is already gone — the newest resize event always wins.
- OK Verified: drag oxnotepad/xdemo by the handle → window grows
  for real (more usable area, no pixel scaling), app reflows, no
  stride artifacts, `full=` stays at 1 (resize repaints are
  regional).

**Apps — resize opt-in + reflow:**
- OK Trivial opt-ins (already had `OX_EV_RESIZE` handlers):
  `oxlog`, `oxmem`, `oxipc`, `oxnet` — now created resizable.
- OK `oxfiles`: dynamic `g_w/g_h` layout (toolbar / sidebar /
  list / right-aligned sizes), RESIZE handler keeps the selection
  visible, and **mouse-wheel scrolling** (3 rows per notch) which
  was missing entirely.
- OK `oxnotepad`: layout macros (`BODY_W/BODY_H/VIS_LINES/...`)
  now derive from live `g_w/g_h`; RESIZE handler clamps scroll +
  keeps the cursor visible. Editor, find/replace strip and status
  bar all reflow.
- Resizable fleet now: oxterm, oxstudio, oxnotepad, oxfiles,
  oxlog, oxmem, oxipc, oxnet (8 of 16).

**tinyX — Xlib subset over Ox (X compat, slice 1):**
- OK New headers `lib/libc/include/X11/{X.h, Xlib.h, Xutil.h,
  keysym.h}` — constants and types match real X11 numerically
  (event codes, masks, Button*, XK_*; same ABI-fidelity rule as
  errno/keycodes). New `lib/libc/xlib.c` (~500 LOC) in
  libosnos_c.a.
- OK Implemented: XOpenDisplay/XCloseDisplay, XCreateSimpleWindow
  (→ resizable Ox window), XStoreName, XMapWindow (synthesizes
  the first Expose), XSelectInput (mask-filtered delivery),
  XCreateGC/XFreeGC/XSetForeground/XSetBackground,
  XFillRectangle/XDrawRectangle/XDrawLine (Bresenham)/XDrawPoint/
  XDrawString (baseline-anchored, 8x8 font)/XClearWindow/
  XClearArea, XFlush/XSync/XPending/XNextEvent (flushes before
  blocking, like real Xlib), XInternAtom/XSetWMProtocols
  (WM_DELETE_WINDOW handshake), XLookupString (keysyms from
  kernel keycodes; X keycode = kernel keycode + 8, evdev rule).
- OK Event mapping: KeyPress, ButtonPress/Release (wheel =
  Button4/5), MotionNotify, Expose, **ConfigureNotify** (from
  OX_EV_RESIZE — drag-resize reaches X clients), ClientMessage
  (WM_DELETE_WINDOW) / DestroyNotify on close.
- OK **`/bin/xdemo`** (`elfs/gui/xdemo.c`): a deliberately stock
  Xlib program (would build on Linux with `cc xdemo.c -lX11`)
  acting as the acceptance test. Verified live: window + BeOS
  decoration, Expose-driven redraw, click stamps (ButtonPress),
  r/g/b color keys (XLookupString), drag-resize → ConfigureNotify
  → re-render at the new size (hint line shows live WxH), `q`
  quits cleanly (wins drops to 0).
- WARN Not implemented (deliberate, see header comment): pixmaps,
  real fonts, colormaps, any wire protocol. Pixels are 0xRRGGBB
  TrueColor. Next slices: XPutImage-style blits, more keysyms,
  XSizeHints honoured by oxsrv, file-backed mmap → real tinyX.

### FASE 15.2 — Premium polish: 1080p + TTF everywhere + RTC + NetSurf HTTPS — **CLOSED**

Direct response to user feedback ("se ve antiguo e indie", "píxeles
gordos, falta suavidad"): kill the chunky-pixel look and the flat
1-px-border widgets. Verified live via QEMU screendumps.

**Resolution (the "fat pixels" root cause):**
- OK `limine.conf` now requests **1920x1080x32** (was the 1280x800
  default, which the host display nearest-neighbor-stretched). The
  compositor + regional damage tracking absorb the 2x pixel count
  without measurable lag (heartbeat unchanged).

**Typography:**
- OK New libc API **`ox_draw_text_pretty()`** (lib/libc/ox.c):
  proportional anti-aliased TTF text into the window backing,
  lazy-loading `/home/.fonts/default.ttf` on first call; transparent
  8x8 fallback. Apps keep `ox_draw_text` where layout math assumes
  the 8-px glyph grid (oxterm cells, oxnotepad body).
- OK Default UI font bumped 12 → **14 px** (oxsrv decorations, ox_ui,
  pretty-text path) to match the 1080p density.
- OK Adopted: **oxfiles** (toolbar path / sidebar / file names /
  sizes right-aligned with real text metrics), **oxlog** (header +
  status), all ox_ui widget labels, oxsrv menu/tab/clock already
  routed through ox_text.

**BeOS R5 bevel pass (ox_ui + oxsrv decorations):**
- OK ox_ui got a bevel/gradient vocabulary (OX_UI_COL_BEVEL_*,
  ui_vgrad/ui_bevel_raised/ui_bevel_sunken): buttons = raised bevel
  + 2-stop gradient face + pressed-sunken with label nudge; lists =
  sunken wells with gradient selection bar + faint hover row;
  scrollbars = sunken track + raised thumb with BeOS grip lines;
  dialogs = raised frame + gradient title bar.
- OK oxsrv: focused tab = yellow vertical gradient + inner top
  highlight; tab buttons = gradient + bevel inside the black
  outline; root-menu hover = gradient selection bar; Deskbar =
  dark gradient strip with top highlight + raised gradient tiles;
  "Ox" menu button = gradient face. All text vertically centered on
  the real TTF line height.

**Kernel — CMOS RTC (`src/drivers/rtc.c`, new):**
- OK One-shot MC146818 read at boot (update-in-progress dance, BCD +
  12h handling, century register with sanity clamps).
  `rtc_boot_epoch()` anchors the wall clock; `sys_time` and
  `sys_clock_gettime(CLOCK_REALTIME)` now return **real UNIX epoch**
  (MONOTONIC stays boot-relative). Boot log: `rtc: 2026-6-12
  18:27:14 UTC epoch=1781288834`. Deskbar clock now shows real UTC.

**NetSurf HTTPS (was: never worked):**
- OK Root cause (vs. working oxbrowser): `https_fetch` skipped
  `br_x509_minimal_set_time()` — BearSSL's X.509 engine aborted with
  TIME_UNKNOWN *before* extracting the server pkey, so the handshake
  died later on the missing key — and never injected DRBG entropy.
  Both ported from oxbrowser's do_tls_fetch; the time seed now uses
  the real RTC epoch, so expiry validation is genuine.
- OK Verified live: `oxnetsurf https://example.com` → TLS 1.2
  handshake + 828 bytes fetched + page rendered. (The remaining
  `parse_chunk err=65547` on example.com reproduces identically over
  plain HTTP — it's the known v1 parser limitation, tracked
  separately.)

**Port defaults:** QEMU hostfwd HTTP default moved 8080 → **8088**
(8080 is routinely taken by Docker/other projects); override with
`make run HTTP_PORT=…`.

### FASE 15.3 — xeyes port + global pointer + cursor-motion flicker fix + HTTPS robustness — **CLOSED**

**xeyes — first real X11 app PORTED (not rewritten):**
- OK `elfs/gui/xeyes.c`: the eye geometry constants, pupil-tracking
  math (`computePupil`) and eye rendering (eyeLiner / eyeBall with
  the abstract eye-space → pixel transform) are taken from the
  original X Consortium xeyes (Eyes.c / Eyes.h / transform.c, MIT
  license, Keith Packard). The Xt/Xaw widget shell is replaced by a
  plain-Xlib main loop (osnos has no Xt yet) polling the pointer at
  10 Hz like the original's timer. Builds unmodified on Linux with
  `cc xeyes.c -lX11 -lm`. In the root menu as "XEyes". Verified
  live: pupils track the cursor across the whole desktop.
- OK tinyX grew: **XFillArc / XDrawArc** (integer ellipse rasterizer
  with X 1/64-degree angle clipping, no libm) and **XQueryPointer**
  (global position + window-relative coords + button mask).
- OK New Ox protocol op **IPC_OX_QUERY_POINTER (0x71)** + libc
  `ox_query_pointer()`: screen cursor position, button mask, and the
  queried window's body origin in one round-trip.

**Cursor-motion flicker ("línea negra vibrante" user report):**
- OK Root cause: over-invalidation while the pointer moves. The menu
  rect was re-marked dirty on EVERY move while open, the Deskbar on
  every move inside it, and any titlebar the cursor was over on
  every move — each one a recompose + VRAM re-blit per frame, and
  every VRAM rewrite is a chance for the host display to catch a
  half-written frame (perceived as dark vibration along the motion
  path; rock solid when idle). Now hover marking fires only on
  STATE changes: menu row change, deskbar element change, titlebar
  enter/leave or button-hover change.
- OK Cursor blit: old∪new bounding union replaced by two small
  rects (merged only when they overlap) — a fast diagonal flick no
  longer rewrites a huge rect every frame.

- OK Host-side flicker root cause (the part the hover fix couldn't
  reach): `build_and_run.sh` ran QEMU with `-display
  cocoa,zoom-to-fit=on` on macOS — the zoomed cocoa view re-renders
  the scaled frame on every partial framebuffer update, which reads
  as black vibration while the guest cursor moves (idle = no
  updates = rock solid). Now: plain `cocoa` (no zoom) + framebuffer
  default 1440x900x32, which fits 1:1 in points on 13" MacBook
  displays → integer Retina scaling, crisp and update-flicker-free.
  Override: `QEMU_DISPLAY=cocoa,zoom-to-fit=on` env var, and bump
  limine.conf to 1920x1080x32 for big monitors.

**HTTPS robustness (user report "sigue fallando"):**
- OK Big-page hang fixed: when the response filled RESP_MAX (e.g.
  google.com uncompressed > 256 KiB), the BearSSL receive loop
  called `recvapp_ack(0)` forever — the engine stayed in RECVAPP
  with nothing consumed and the fetch never returned. Now a full
  buffer ends the read (truncating, logged). example.com-sized
  pages were unaffected, which is why the first verification passed.

### FASE 15 — Drivers to ring 3 (pending item from original FASE 11)
- TODO IRQ delegation via IPC from kernel-side handlers
- TODO MMIO mapping per-task with special permissions
- TODO Port-IO delegation (syscall whitelist or IOPB in TSS)
- TODO DMA bouncing via kernel-mediated buffer pool
- TODO Port PS/2, framebuffer, ATA, RTL8139, PIT to
  `elfs/osn-driver/`

### Far future
- TODO SMP (multi-core)
- TODO Copy-on-write for fork (today full page copy)
- TODO File-backed mmap (path to real port of tinyX/X11)
- TODO Real X11 wire protocol (oxlib is a shim until tinyX
  arrives)
- TODO ext2/ext4 read-only (alternative to FAT16 for more
  capacity)
- TODO More vendor ports: tiny perl, sqlite-net, lua-luarocks,
  etc.

---

## Project conventions

- **Language**: C99 (kernel + mini-libc), kernel code with
  `-Werror`
- **Toolchain**: clang + ld.lld (cross-compile from macOS or
  Linux)
- **Bootloader**: Limine 8.x (installed from system, not versioned
  in repo)
- **Test infra**: `./build_and_run.sh headless` + serial capture
  -> grep for CI
- **Spanish docs**: STATUS.es.md, ARCH.es.md, README.es.md,
  CLAUDE.es.md, CREATE_BUILTINS.es.md, CREATE_ELF.es.md
- **English docs**: README.md (root), STATUS.md (this),
  ARCH.md, CLAUDE.md

To re-enter the project after months away: read this STATUS.md ->
README.md -> ARCH.md -> CLAUDE.md -> CREATE_ELF.es.md (in that
order).

---

## How to extend

Three typical entry points:

1. **Add a shell command** — either a new BusyBox applet (rebuild
   with the option) or a new ELF in `elfs/tools/`. For ELFs: drop
   `elfs/tools/foo.c`, add to `USER_ELF_LIBC_SRCS` in
   GNUmakefile, `make` — the binary appears at `/bin/foo`.

2. **Add a program against musl** — drop `elfs/tests/foo.c`, add
   to `USER_ELF_MUSL_SRCS`, add a specific rule in GNUmakefile
   (template: copy the one for `hello_musl.elf`). Useful for
   programs needing full stdio, printf-%f, locale.

3. **Add a new syscall** — define the number in
   `src/micro/syscall.h` (range 500+ for osnos-specific; matching
   Linux x86_64 for POSIX), implement the handler in
   `src/micro/syscall.c` (`int64_t sys_foo(...)`), add the case
   in the dispatcher. If it has a libc wrapper: drop in the
   matching `lib/libc/`.

Step-by-step detail in [`CREATE_ELF.es.md`](CREATE_ELF.es.md) and
[`CREATE_BUILTINS.es.md`](CREATE_BUILTINS.es.md) (Spanish).
