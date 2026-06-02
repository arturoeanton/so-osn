# OSnOS

Hobby x86_64 operating system, microkernel-style, written in C from
scratch. Boots with Limine, runs in QEMU, and ships **BusyBox ash
1.36.1** as system shell linked against **musl 1.2.5**, persistent
FAT16, **four self-hosting languages** (C via TCC + Lua + jq +
**SQL via SQLite**), a homegrown **mini-X window system (Ox)** with
notepad/calculator/terminal/file-browser/settings, **self-hosting
POSIX make** (`cd /home && make hello && ./hello` builds with tcc
from inside), **AF_UNIX sockets**, **POSIX shared memory**
(`shm_open` + `mmap MAP_SHARED`), **dynamic linking via
ld-musl.so** (dynamically-linked apps actually run), ~70+ syscalls
that match Linux x86_64, and a ~25K LOC microkernel with ELF
loader + its own paging + preemptive scheduler + IPC + a full
POSIX line discipline.

> **Spanish version:** [`README.es.md`](README.es.md).

```
   osnos x86_64 — BusyBox ash 1.36.1 + musl (init shell, FASE 13.1)

     ___  ____         ___  ____
    / _ \/ ___| _ __  / _ \/ ___|
   | | | \___ \| '_ \| | | \___ \
   | |_| |___) | | | | |_| |___) |
    \___/|____/|_| |_|\___/|____/

     osnos — x86_64 microkernel hobby OS
   BusyBox ash on osnos — help for builtins, ls /bin for commands.

   osnos:/# ls /
   sys/  dev/  sd/  bin/  lib/  usr/  etc/  home/

   osnos:/# echo $((100*7))                              # POSIX arith
   700

   osnos:/# find /etc -type f                            # busybox find
   /etc/passwd  /etc/group  /etc/hosts  /etc/profile

   osnos:/# md5sum /etc/passwd
   3aae6b3999e00cd5ca29cc8e954bc63f  /etc/passwd

   osnos:/# sqlite3 /home/demo.db                        # SQL REPL
   SQLite version 3.45.2 2024-03-12 11:06:23
   sqlite> SELECT title, year FROM books ORDER BY year LIMIT 3;
   The Mythical Man-Month|1975
   The C Programming Language|1978
   SICP|1985
   sqlite> .quit

   osnos:/# lua                                          # Lua REPL
   Lua 5.4.7  Copyright (C) 1994-2024 Lua.org, PUC-Rio
   > print(math.sqrt(2))
   1.4142135623731
   > os.exit()

   osnos:/# tcc /home/hello.c -o /home/hello             # SELF-HOSTING C
   osnos:/# /home/hello
   hello from tcc on osnos!

   osnos:/# cat /home/test.json | jq '.tools[].name'    # JSON filtering
   tcc
   lua
   jq
   ovi

   osnos:/# oxsrv &                  # mini-X GUI (Ox window system)
   # wallpaper appears, right-click -> Openbox-style menu:
   #   Files  Notepad  Calculator  Terminal  Settings  Reboot
```

> **TL;DR for re-entering the project after months away:** install
> Limine from your system (it is not versioned in-tree), run
> `./build_and_run.sh`, done. All the code lives in
> [`osnos/`](osnos/). Recommended path: this README ->
> [`osnos/STATUS.md`](osnos/STATUS.md) (what works today) ->
> [`osnos/ARCH.md`](osnos/ARCH.md) (layered architecture) ->
> [`osnos/CLAUDE.md`](osnos/CLAUDE.md) (operational cheat-sheet).

---

## Table of contents

- [Quick start](#quick-start)
- [Dependencies](#dependencies)
- [Repository layout](#repository-layout)
- [Architecture](#architecture)
- [Current state](#current-state-what-works-today)
- [Extending the system](#extending-the-system)
- [Invariants](#invariants-that-must-not-be-broken)
- [Documentation](#documentation-map)
- [Roadmap](#roadmap)
- [License](#license)

---

## Quick start

```sh
git clone <repo-url>
cd so-osn
./build_and_run.sh                  # boot in QEMU with framebuffer
# or:
./build_and_run.sh headless         # boot over serial, no window (CI)
```

The script:
1. `make -C osnos` (kernel + libc + all ELFs + ISO + sd.img)
2. `qemu-system-x86_64 -M pc -cdrom build/osnos-x86_64.iso -boot d -drive file=sd.img -m 2G ...`

First boot: ~3-4 seconds to the `osnos:/#` prompt. Incremental
rebuilds: ~5 seconds typical.

```sh
# Automated tests (verifies kernel + libc + ports)
cd osnos && make run        # boot + from inside the shell: `alltest`
```

---

## Dependencies

Host:
- `clang` + `ld.lld` (bundled with clang/LLVM)
- `xorriso` (to create the ISO)
- `mtools` (`mformat`, `mcopy`, `mmd` — for the sd.img FAT16)
- `qemu-system-x86_64`
- **Limine** installed system-wide:
  - macOS: `brew install limine`
  - Linux Ubuntu/Debian: `apt install limine` (or build from source)
  - Linux Arch: `pacman -S limine`
- Optional for CI / headless: `nc` (netcat) for serial capture

Secondary build deps (not required for a basic boot):
- `python3` (some host-side generators)
- host `sqlite3` (to regenerate `res/demo.db` at build time — falls back to the shipped copy if absent)
- ImageMagick `convert` (for real wallpapers — procedural fallback if missing)

Everything else lives vendored in [`osnos/vendor/`](osnos/vendor/):
- `tinycc/` (0.9.27) — C compiler that runs INSIDE osnos
- `lua/` (5.4.7) — Lua interpreter
- `jq/` (1.7.1) — JSON filter
- `musl/` (1.2.5) — opt-in second libc
- `busybox/` (1.36.1) — ~60 POSIX applets
- `sqlite/` (3.45.2 amalgamation) — SQL engine

---

## Repository layout

```
so-osn/
├── README.md                  (this file)
├── README.es.md               (Spanish version)
├── LICENSE.md
├── build_and_run.sh           (wrapper: make + qemu)
└── osnos/                     <- all the code lives here
    ├── GNUmakefile            (top-level: kernel + libc + ELFs + ISO + sd.img + QEMU)
    ├── limine.conf            (boot entry — loads /boot/kernel)
    ├── kernel-deps/           (vendored Limine freestanding helpers)
    │
    ├── src/                   <- KERNEL (~25K LOC)
    │   ├── kernel/main.c      (kmain — init sequence)
    │   ├── micro/             (task, scheduler, syscall, ipc, fd, tty, mmap, fpu, ...)
    │   ├── drivers/           (framebuffer, keyboard, mouse, serial, ata, rtl8139, ...)
    │   ├── fs/                (ramfs, fat, devfs, sysfs, binfs, aliasfs, bootstrap)
    │   ├── proc/              (exec, elf loader, builtin registry)
    │   ├── net/               (eth, arp, ip, icmp, udp, tcp, socket)
    │   ├── lib/               (string, memory, printf — kernel-only helpers)
    │   ├── servers/           (kernel-side feeders: keyboard, mouse, serial input)
    │   └── include/           (osnos_*_abi.h — ABI frontier headers)
    │
    ├── lib/
    │   ├── libc/              (local mini-libc — for ELFs that don't use musl)
    │   │   ├── crt0.S         (_start -> argc/argv/envp -> main -> _exit)
    │   │   ├── unistd.c stdio.c stdlib.c string.c signal.c termios.c pthread.c ...
    │   │   ├── ox.{c,h} ox_font.c   (client for the Ox window system)
    │   │   └── include/             (headers: stdio, unistd, fcntl, ..., ox.h, linux/fb.h)
    │   └── sysroot/                 (crti.S / crtn.S for the TCC sysroot)
    │
    ├── elfs/                  <- USERLAND (~60 ring-3 ELFs)
    │   ├── shell/             (osh — legacy script interpreter)
    │   ├── tools/             (~60 coreutils: ls cat cp mv rm mkdir touch echo ...
    │   │                       + tcc, lua, jq linked here)
    │   ├── net/               (tcpclient, udptest, echotcp, httpd, ...)
    │   ├── tests/             (~28 smoke tests: forktest, sigtest, libctest, hello_musl, ...)
    │   ├── osn-server/        (consrv, kbdsrv, shellsrv — ring-3 servers)
    │   ├── gui/               (oxsrv + oxnotepad + oxcalc + oxterm + oxfiles + oxsettings)
    │   ├── libc.lds           (linker script for mini-libc-linked ELFs)
    │   └── musl.lds           (linker script for musl-linked ELFs)
    │
    ├── vendor/                <- vendored third-party code
    │   ├── tinycc/            (TinyCC 0.9.27, ~30K LOC)
    │   ├── lua/               (Lua 5.4.7, ~24K LOC)
    │   ├── jq/                (jq 1.7.1, ~24K LOC)
    │   ├── musl/              (musl 1.2.5, ~140K LOC) + build-osnos/lib/{libc.a, crt1.o, crti.o, crtn.o}
    │   ├── busybox/           (BusyBox 1.36.1) + osnos-cc-wrapper.sh + busybox_unstripped
    │   └── sqlite/            (SQLite 3.45.2 amalgamation: sqlite3.c + shell.c + sqlite3.h)
    │
    ├── res/                   <- resources for sd.img
    │   ├── demo.sql           (SQL used to regenerate /home/demo.db at build)
    │   ├── demo.db            (pre-populated DB: 15 books + 4 users + checkouts + view)
    │   └── wallpapers/source/ (optional: drop PNGs here for real Ox wallpapers)
    │
    ├── tools/                 <- host-side build scripts
    │   ├── gen_placeholder.c  (generates procedural PPMs when no PNGs are available)
    │   └── gen_wallpapers.sh  (wrapper PNG->PPM with procedural fallback)
    │
    ├── ARCH.md                (layered architecture + IPC + syscall walkthroughs)
    ├── STATUS.md              (current state + phase log — SOURCE OF TRUTH)
    ├── CLAUDE.md              (operational cheat-sheet for AI assistants)
    ├── CREATE_BUILTINS.es.md  (tutorial: add a kernel-mode command)
    ├── CREATE_ELF.es.md       (tutorial: add a ring-3 ELF against mini-libc or musl)
    ├── PLAN_FASE10.md         (original plan for phase 10 — servers to ring 3)
    ├── ROADMAP_APENDICE.md    (multi-phase roadmap)
    └── build/                 (output: kernel ELF, ISO, .o files, built ELFs)
```

`sd.img` (32 MiB FAT16) is generated at build time and lives next
to `osnos/GNUmakefile`. It is mounted as IDE primary master in QEMU
(`-drive file=sd.img,if=ide,index=0,media=disk`).

---

## Architecture

### Layer diagram (post-FASE 13)

```
   +================================================================+
   ||         user apps (ring 3) — busybox + tcc + lua + jq +       ||
   ||         sqlite + oxnotepad + oxcalc + ... + tests             ||
   ||              v syscall (rax=#, rdi/rsi/rdx/r10/r8/r9)         ||
   +================================================================+
   ||                  libc (ring 3 — opt-in per ELF):              ||
   ||                                                                ||
   ||  lib/libc/  <-> small programs (coreutils, osn-server, ox)    ||
   ||  musl/      <-> serious programs (busybox, tcc, lua, jq,      ||
   ||                  sqlite, hello_musl)                          ||
   +================================================================+
   ||              ring-3 servers (with directed IPC):              ||
   ||                                                                ||
   ||   consrv (SERVER_CONSOLE)   ->  console output -> /dev/fb0    ||
   ||   kbdsrv (SERVER_KEYBOARD)  <-  /dev/input0 -> tty_input      ||
   ||   busybox sh (SERVER_SHELL) <-  TTY input, fork+exec apps     ||
   ||   oxsrv (SERVER_OX, opt-in) ->  /dev/fb0 via FBIO_BLIT,       ||
   ||                                  mouse, kbd, IPC opcodes 0x60-7F ||
   +================================================================+
   ||                       KERNEL (ring 0)                         ||
   ||                                                                ||
   ||   syscall dispatcher (~70 syscalls)  -->  task / scheduler    ||
   ||         |                                  |  v resume jump   ||
   ||         v                                  scheduler_loop     ||
   ||   sys_read/write/open/...           v                          ||
   ||   sys_fork/execve/wait/sigaction   ipc queue (64 x 1024B)     ||
   ||   sys_socket/bind/listen/...                                  ||
   ||   sys_arch_prctl (TLS)                                        ||
   ||   sys_mmap/munmap/brk                                         ||
   ||   sys_fcntl(F_SETLK->0)                                       ||
   ||         v                                                     ||
   ||   VFS layer (longest-prefix dispatch) -- 16 mount slots:      ||
   ||      "/"     ramfs                                            ||
   ||      "/sys"  sysfs (synthetic)                                ||
   ||      "/dev"  devfs (fb0, input0, mouse0, tty, ptmx, pts/N, ...) ||
   ||      "/sd"   FAT16 (block_ata)                                ||
   ||      "/bin"  -> /sd/bin (aliasfs)                             ||
   ||      "/etc"  -> /sd/etc (aliasfs)                             ||
   ||      "/home" -> /sd/home (aliasfs)                            ||
   ||      "/lib"  -> /sd/lib (aliasfs)                             ||
   ||      "/usr"  -> /sd/usr (aliasfs)                             ||
   ||                                                                ||
   ||   TTY line discipline (canon/raw, ECHO, ISIG, EINTR)          ||
   ||         v echo + erase                                        ||
   ||   framebuffer_write_bytes (cursor tracking + serial mirror)   ||
   +================================================================+
   ||                     drivers (ring 0)                          ||
   ||                                                                ||
   ||   PS/2 kbd  PS/2 mouse  framebuffer  serial UART              ||
   ||   block_ata (FAT16)     RTL8139 (TCP/IP)   PIC + LAPIC + PIT  ||
   +================================================================+
   ||   Limine boot -> kmain -> init drivers -> spawn servers -> sti ||
   +================================================================+
```

### Boot sequence (`kmain` in `src/kernel/main.c`)

1. `serial_init(COM1)` — UART first, so panic handlers have a sink
2. Validate Limine base revision + framebuffer; `framebuffer_init`
3. Memory layer: `pmm_init -> vmm_init -> kheap_init`
4. CPU + IRQs: `gdt_init -> tss_init -> idt_init -> uaccess_init ->
   syscall_msr_init -> fpu_init -> pic_init -> lapic_init -> timer_init`
5. Drivers: `block_ata_init` (IDENTIFY primary master), `rtl8139_init`
   (PCI scan; silent if no NIC), `net_init` (ARP + RX dispatch)
6. Microkernel state: `ipc_init -> pipe_init -> pty_init -> task_init ->
   reaper_init -> scheduler_init -> syscall_init -> ramfs_init ->
   bootstrap_fs`
7. Spawn the kernel-side feeders (cooperative ring-0):
   - `keyboard` — drains PS/2 -> `/dev/input0`
   - `mouse` — drains PS/2 AUX -> `/dev/mouse0`
   - `serial-in` — COM1 RX -> `tty_input`
8. Spawn the ring-3 servers via `proc_execve`:
   - `/bin/consrv` (registered as `SERVER_CONSOLE`)
   - `/bin/kbdsrv` (registered as `SERVER_KEYBOARD`)
   - `/bin/busybox sh -l` (registered as `SERVER_SHELL`)
     - Fallback: `/bin/shellsrv` (custom legacy shell) if busybox is missing
9. `keyboard_server_init()` + `mouse_server_init()`
10. `init-respawn` watchdog task
11. `sti` (enable IRQs)
12. `scheduler_loop` — eternal home, `sched_resume_jump()` longjumps here

---

## Current state (what works today)

High-level summary. Exhaustive per-phase detail in
[`osnos/STATUS.md`](osnos/STATUS.md).

| Subsystem | Status |
|---|---|
| Limine boot + framebuffer + dual serial console | OK |
| PS/2 keyboard + PS/2 mouse + `/dev/input0` + `/dev/mouse0` | OK |
| GDT + IDT + TSS + own paging + kheap (32 MiB) | OK |
| Cooperative microkernel + CPL=3 preempt (50 ms quantum) + 64-slot IPC queue | OK |
| Linux x86_64 syscalls (~70) + osnos-specific (>= 500) | OK |
| **`restart_syscall` pattern** in sys_read/sys_poll (BusyBox needs this) | OK |
| **`fs_base` save/restore on task switch + reset on execve** (musl TLS) | OK |
| `copy_from_user`/`copy_to_user` with fault recovery (extable) | OK |
| VFS + ramfs + sysfs + devfs + binfs + aliasfs + FAT16 (16 mount slots) | OK |
| Per-task fd table (16 fds) + OFD pool + pipe + dup/dup2 + fcntl | OK |
| `fork(2)` + `execve(2)` + `wait4(2)` + `sigaction(2)` full POSIX | OK |
| Process groups + sessions + Ctrl+C/Z fan-out to pgid + WUNTRACED/WCONTINUED | OK |
| EINTR on blocking syscalls (read/wait/nanosleep/accept) | OK |
| PTY pairs (`/dev/ptmx` + `/dev/pts/N`, pool of 8) | OK |
| Anonymous `mmap`/`munmap` + brk/sbrk | OK |
| FXSAVE/FXRSTOR per-task (multi-task safe FP/SSE) | OK |
| Job control: `&`, `jobs`, `fg`, `bg`, Ctrl+Z, `kill` | OK |
| `sys_spawn(2)` with fd inheritance | OK |
| **POSIX TTY line discipline** with consistent echo + backspace (post-FASE-13.3 fix) | OK |
| **`/bin/busybox` (1.36.1, musl-linked) as init shell** | OK |
| **Cross-reboot persistent history** (`/home/.ash_history`) | OK |
| **`/etc/profile` + `/home/.ashrc`** in .bashrc style, banner + PS1 + aliases | OK |
| **~30 BusyBox applets**: vi awk sed find diff stat dd df du md5sum sha1sum sha256sum base64 hexdump bc dc more tac fold xargs find timeout ... | OK |
| **Native coreutils (~60 ELFs)**: ls cat cp mv rm mkdir touch echo wc head tail grep sort uniq cut tr ... | OK |
| Shell glob `*`, pipes \|, redirection > >> <, `;` `&&` `\|\|` | OK |
| `/bin/less` with `/pattern` highlight, pipe-mode | OK |
| `/bin/ovi` modal vim-style editor (legacy; BusyBox vi also available) | OK |
| `/bin/term` + `/bin/minishell` interactive PTY sub-shell | OK |
| `/bin/readelf` ELF inspector | OK |
| `/bin/poweroff` + `/bin/reboot` (ACPI S5 / 8042 reset) | OK |
| **`/bin/tcc` — TinyCC 0.9.27 SELF-HOSTING C** | OK |
| **`/bin/lua` — Lua 5.4.7 REPL + scripts** | OK |
| **`/bin/jq` — jq 1.7.1 JSON filter** | OK |
| **`/bin/sqlite3` — SQLite 3.45.2 SQL engine + `/home/demo.db` preseeded** | OK |
| **Ox mini-X window system** (`/bin/oxsrv` + 5 GUI apps) | OK |
| init-respawn watchdog (consrv/kbdsrv/shellsrv auto-restart) | OK |
| ATA PIO driver + FAT16 read/write/append + persistence | OK |
| `/bin` disk-resident (sd.img populated at build via mtools) | OK |
| RTL8139 driver + ARP + IPv4 + ICMP + UDP + complete TCP | OK |
| POSIX sockets + DNS + `/bin/httpd` serving from FAT16 | OK |
| POSIX TTY line discipline (termios canon/raw, ISIG) | OK |
| `/dev/fb0` + `/dev/input0` + `/dev/mouse0` + `/dev/tty` + `/dev/ttyS0` | OK |
| Linux-compatible FB ioctls: `FBIOGET_VSCREENINFO`, `FBIO_BLIT` | OK |
| `/home` aliased to `/sd/home` (same for `/etc`, `/bin`, `/lib`, `/usr`) | OK |
| `getcwd` / `chdir` syscalls + per-task cwd | OK |
| **POSIX `make` (`/bin/make` = pdpmake) — `cd /home && make hello` self-hosts with tcc** (FASE 14.1) | OK |
| **`AF_UNIX` SOCK_STREAM** (path namespace + socket/bind/listen/accept/connect/read/write) (FASE 14.2) | OK |
| **POSIX `shm_open` + `mmap(MAP_SHARED, fd)`** — cross-fork shared memory (FASE 14.3) | OK |
| **Dynamic linking** via `ld-musl-x86_64.so.1` + `/lib/libc.so` — PT_INTERP, full auxv, dyn-linked app runs (FASE 14.4) | OK |
| **Real `SYS_CLONE`** with `CLONE_VM` + `CLONE_VFORK` (musl's posix_spawn) | OK |
| **POSIX `execve` resets signal handlers to SIG_DFL** (without this, inherited SIGCHLD jumped into the old binary's interp text) | OK |
| **`sys_execve` preserves argv boundaries** (no flat-join + re-tokenize) | OK |
| **rdmsr FS_BASE on fork** (without this, musl `__post_Fork` NULL-derefs on %fs:0 at first fork pre-task-switch) | OK |
| 20/21 automated tests via `/bin/alltest` (includes unixtest + shmtest + hello_dyn) | OK |
| SMP (multi-core) | TODO |
| Copy-on-write fork | TODO |
| File-backed mmap of regular files | TODO |
| Real X11 / tinyX (Ox is its own protocol) | TODO |

---

## Extending the system

Details in [`osnos/CREATE_BUILTINS.es.md`](osnos/CREATE_BUILTINS.es.md)
and [`osnos/CREATE_ELF.es.md`](osnos/CREATE_ELF.es.md) (Spanish).
Summary:

### 1. Add a shell command (the most common case)

Drop `osnos/elfs/tools/foo.c`:

```c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    printf("foo: argc=%d\n", argc);
    return 0;
}
```

Add it to `USER_ELF_LIBC_SRCS` in `osnos/GNUmakefile`, run `make`,
the binary appears at `/bin/foo` in sd.img and runs from the shell:
`foo a b c`.

### 2. Add a program against musl (for real stdio / printf-%f)

Drop `osnos/elfs/tests/foo_musl.c` (same shape). Add it to
`USER_ELF_MUSL_SRCS` and add a specific rule in the GNUmakefile by
copying the one for `hello_musl.elf`. Useful when you need `printf`
with `%f`, full locale, the pthread shim, or math with no gap-fills.

### 3. Add a syscall

1. Define the number in `src/micro/syscall.h`:
   - 0-499 reserved for Linux x86_64 compat (matching numbers)
   - 500+ for osnos-specific (won't clash with Linux)
2. Implement the handler in `src/micro/syscall.c`:
   ```c
   int64_t sys_foo(int arg1, void *arg2) { ... }
   ```
3. Add the case to the dispatcher:
   ```c
   case SYS_FOO: return pack(sys_foo((int)frame->rdi, (void *)frame->rsi));
   ```
4. (Optional) libc wrapper in `lib/libc/foo.c` or equivalent.

### 4. Add a ring-3 server

Drop `osnos/elfs/osn-server/<name>.c` (libc-linked). Use
`sys_service_register(SERVER_FOO)` to claim the slot, then loop
on `sys_ipc_recv`. If the server is critical, add it to the
watchdog in `src/kernel/main.c::server_respawn_tick`.

### 5. Add a BusyBox applet

Edit `vendor/busybox/.config` (turn on `CONFIG_<APPLET>=y`), then
rebuild with `cd vendor/busybox && make CC=./osnos-cc-wrapper.sh
HOSTCC=clang -j4`. The resulting unstripped binary is copied to
`/bin/busybox` automatically when sd.img is rebuilt. Add an alias
in `src/fs/bootstrap.c` inside the `/home/.ashrc` seed if you want
to invoke it as a native command (FAT16 has no symlinks).

---

## Invariants that must NOT be broken

1. **Linux x86_64 ABI.** Every number visible to userland (errno,
   syscall numbers, key codes, ELF constants, ioctls, signals,
   sockaddr layouts, getdents records) must match Linux. Goal:
   run unmodified Linux ELFs against osnos libc or musl. For
   osnos-specific non-POSIX entries use 500+ (`SYS_ISATTY=500`,
   `SYS_IPC_*=510+`, etc.).

2. **ABI frontier in `src/include/osnos_*_abi.h`.** Any header
   suffixed `*_abi.h` is a kernel<->userland contract. Changing one
   requires rebuilding kernel + libc + every ELF.

3. **SYSCALL ABI.** Both `int 0x80` and `syscall` reach the same
   `syscall_dispatch` over a `syscall_frame_t`. Registers:
   `rax = #`, `rdi/rsi/rdx/r10/r8/r9 = args` (R10 NOT RCX, matching
   Linux).

4. **IPC contract** (`osnos_ipc_abi.h`). Opcode ranges: 0x00-0x0F
   system, 0x10-0x1F console, 0x20-0x3F fs/vfs, 0x40-0x5F process
   lifecycle, 0x60-0x7F Ox window system. Every response sets
   `arg0=status, arg1=size, data=text`. `ipc_send` may fail with
   EAGAIN or ESRCH — never ignore the return value.

5. **Ramfs slot ownership.** Pointers borrowed from `ramfs_find`
   survive deletes of OTHER slots.

6. **Scheduling.** Ring-3 preempted by the 50 ms timer. **Ring-0
   is still cooperative** — a kernel server that loops without
   returning hangs the entire kernel. Use `wakeup_at_ms +
   state=BLOCKED` for periodic kernel tasks.

7. **Single shared 64-slot IPC queue.** Outputs of N lines must be
   packed into a single message; per-line sends overflow the queue.

8. **No extensive per-task FPU save yet.** Single-task FP is fine;
   mixing FP across multiple ring-3 tasks may corrupt state.
   FXSAVE/FXRSTOR per-task is implemented but not stress-tested.

9. **QEMU machine type matters.** `block_ata` talks PIO to legacy
   0x1F0; `-M pc` attaches IDE there. `-M q35` attaches via AHCI
   and the driver doesn't see the disk.

10. **`framebuffer_write_bytes` is the only path** that keeps the
    cursor consistent between kernel echoes and app output via
    consrv. Do NOT use `framebuffer_draw_string` directly for
    interactive echoes (it breaks REPLs).

---

## Documentation map

| File | What to read it for |
|---|---|
| [`README.md`](README.md) (this) | Pitch + structure + dependencies + architecture overview (English) |
| [`README.es.md`](README.es.md) | Spanish version of this file |
| [`osnos/STATUS.md`](osnos/STATUS.md) | **Source of truth** on what works today + phase log + notable bugs + roadmap |
| [`osnos/STATUS.es.md`](osnos/STATUS.es.md) | Spanish version |
| [`osnos/ARCH.md`](osnos/ARCH.md) | Layered architecture + IPC and syscall walkthroughs + Ox flow + musl flow |
| [`osnos/ARCH.es.md`](osnos/ARCH.es.md) | Spanish version |
| [`osnos/CLAUDE.md`](osnos/CLAUDE.md) | Operational cheat-sheet aimed at AI assistants (also useful for humans) |
| [`osnos/CLAUDE.es.md`](osnos/CLAUDE.es.md) | Spanish version |
| [`osnos/CREATE_BUILTINS.es.md`](osnos/CREATE_BUILTINS.es.md) | Tutorial: add a kernel-mode command (KERN flavor) — Spanish |
| [`osnos/CREATE_ELF.es.md`](osnos/CREATE_ELF.es.md) | Tutorial: add a ring-3 ELF (3 flavors: mini-libc, musl, bare) — Spanish |
| [`osnos/PLAN_FASE10.md`](osnos/PLAN_FASE10.md) | Detailed plan for phase 10 (servers to ring 3) |
| [`osnos/ROADMAP_APENDICE.md`](osnos/ROADMAP_APENDICE.md) | Multi-phase roadmap appendix |

---

## Roadmap

Detail in [`osnos/STATUS.md`](osnos/STATUS.md). Summary:

**FASE 14 (CLOSED)** — modern POSIX infrastructure:
- 14.1 — POSIX `make` (pdpmake) + `make hello && ./hello` end-to-end self-host with tcc
- 14.2 — `AF_UNIX` SOCK_STREAM (socket pool + path namespace + ring buffer)
- 14.3 — POSIX SHM (`shm_open` + `mmap(MAP_SHARED, fd)`) + fork preserves shared pages
- 14.4 — Dynamic linking via `ld-musl-x86_64.so.1`, PT_INTERP + full auxv

**FASE 14.x pending (quality of life)**:
- Real per-PTY termios (each shell/REPL with its own termios)
- argv passing fix in sqlite3
- Synthetic `/proc` (at least `/proc/<pid>/cmdline`, `/proc/meminfo`)
- More BusyBox: `top`, `ps`, `free`, `uptime`
- Chip-8 emulator (last item of the original graphics roadmap)

**FASE 15 — extended GUI (under discussion)**:
- Option A: `xeyes-via-Ox` (native Ox client)
- Option B: vendor nano-X (~20K LOC) on top of FBDEV
- Option C: minimal X11 wire protocol bound to `/tmp/.X11-unix/X0` via AF_UNIX (now possible)

**Medium term (FASE 15)** — drivers to ring 3:
- IRQ delegation via IPC
- Per-task MMIO mapping
- Port-IO delegation (IOPB in TSS)
- DMA bouncing
- Port PS/2, framebuffer, ATA, RTL8139, PIT to `elfs/osn-driver/`

**Far future**:
- SMP (multi-core)
- Copy-on-write fork
- File-backed mmap
- Real X11/tinyX wire protocol (oxlib is a shim until then)
- ext2/ext4 read-only
- More vendor ports (tiny perl, sqlite-net, lua-luarocks, ...)

---

## Inspiration and credits

- **Limine** for the clean bootloader (hosted at
  [limine-bootloader/limine](https://github.com/limine-bootloader/limine))
- **musl** for being a libc readable enough to understand end-to-end
  ([musl.libc.org](https://musl.libc.org/))
- **BusyBox** for turning 1 MB of C into a functional distro
  ([busybox.net](https://busybox.net/))
- **TinyCC** for proving a C compiler can fit in ~30K LOC
  ([repo.or.cz/tinycc.git](https://repo.or.cz/tinycc.git))
- **SQLite** for being a miracle of engineering in a single .c
- **Lua** for the design point of the perfect embeddable language
- **Beej's Guide to Network Programming** for teaching sockets without
  tears
- **OSDev wiki** for hours of paging + GDT + APIC
- **The C Programming Language (K&R)** for still being the reference
  manual

And to anyone who wants to contribute, open issues, or use this code
to learn — that's exactly what the repo is for.

---

## License

OSnOS's own code: MIT. See [`LICENSE.md`](LICENSE.md).

Vendored code in `osnos/vendor/`:
- TinyCC: LGPL 2.1
- Lua: MIT
- jq: MIT
- musl: MIT
- BusyBox: GPL v2
- SQLite: public domain

Their respective licenses apply to those components; see the header
of each source tree at `vendor/<project>/COPYING` or
`vendor/<project>/LICENSE`.
