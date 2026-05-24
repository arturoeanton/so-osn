# OSnOS

Sistema operativo hobby x86_64, estilo microkernel, escrito en C
desde cero. Bootea con Limine, corre en QEMU, y trae **BusyBox ash
1.36.1** como shell de sistema linkeado contra **musl 1.2.5**, FAT16
persistente, **cuatro lenguajes self-host** (C via TCC + Lua + jq +
**SQL via SQLite**), un **mini-X window system propio (Ox)** con
notepad/calculadora/terminal/file-browser/settings, ~70 syscalls
Linux x86_64-compatibles, y un microkernel de ~25K LOC con ELF
loader + paging propio + scheduler preemptivo + IPC + line discipline
POSIX completa.

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

   osnos:/# sqlite3 /home/demo.db                        # SQL REPL 🗃️
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

   osnos:/# oxsrv &                  # mini-X GUI 🪟 (Ox window system)
   # wallpaper aparece, right-click → menú Openbox-style:
   #   Files  Notepad  Calculator  Terminal  Settings  Reboot
```

> **TL;DR para reentrar al proyecto después de meses:** instalar
> Limine del sistema (no se versiona), correr `./build_and_run.sh`,
> listo. Todo el código vive en [`osnos/`](osnos/). Recorrido
> recomendado: este README → [`osnos/STATUS.md`](osnos/STATUS.md)
> (qué funciona hoy) → [`osnos/ARCH.md`](osnos/ARCH.md) (capas) →
> [`osnos/CLAUDE.md`](osnos/CLAUDE.md) (cheat-sheet operativa).

---

## Tabla de contenidos

- [Arrancar en un comando](#arrancar-en-un-comando)
- [Dependencias](#dependencias)
- [Estructura del repo](#estructura-del-repo)
- [Arquitectura](#arquitectura)
- [Estado actual](#estado-actual-qué-funciona-hoy)
- [Cómo extender](#cómo-extender-el-sistema)
- [Invariantes](#invariantes-que-no-se-deben-romper)
- [Documentación](#mapa-de-documentación)
- [Roadmap](#roadmap)
- [Licencia](#licencia)

---

## Arrancar en un comando

```sh
git clone <repo-url>
cd so-osn
./build_and_run.sh                  # bootea en QEMU con framebuffer
# o:
./build_and_run.sh headless         # bootea en serial, sin ventana (CI)
```

El script:
1. `make -C osnos` (kernel + libc + todos los ELFs + ISO + sd.img)
2. `qemu-system-x86_64 -M pc -cdrom build/osnos-x86_64.iso -boot d -drive file=sd.img -m 2G ...`

Primer arranque: ~3-4 segundos hasta el prompt `osnos:/#`. Rebuilds
incrementales: ~5 segundos típicos.

```sh
# Tests automáticos (verifica kernel + libc + ports)
cd osnos && make run        # boot + dentro del shell: `alltest`
```

---

## Dependencias

Host:
- `clang` + `ld.lld` (incluido en clang/LLVM)
- `xorriso` (para crear ISO)
- `mtools` (`mformat`, `mcopy`, `mmd` — para sd.img FAT16)
- `qemu-system-x86_64`
- **Limine** instalado del sistema:
  - macOS: `brew install limine`
  - Linux Ubuntu/Debian: `apt install limine` (o build from source)
  - Linux Arch: `pacman -S limine`
- Opcional para CI / headless: `nc` (netcat) para captura serial

Build secundario (no requerido para boot básico):
- `python3` (para algunos scripts host de generación)
- `sqlite3` host (para regenerar `res/demo.db` al build — fallback si no está, solo no se actualiza la DB)
- ImageMagick `convert` (para wallpapers reales — fallback procedural si no está)

Todo el resto vive vendoreado en [`osnos/vendor/`](osnos/vendor/):
- `tinycc/` (0.9.27) — compilador C que corre EN osnos
- `lua/` (5.4.7) — intérprete Lua
- `jq/` (1.7.1) — filtrador JSON
- `musl/` (1.2.5) — segunda libc opt-in
- `busybox/` (1.36.1) — ~60 applets POSIX
- `sqlite/` (3.45.2 amalgamation) — engine SQL

---

## Estructura del repo

```
so-osn/
├── README.md                  (este archivo)
├── LICENSE.md
├── build_and_run.sh           (wrapper: make + qemu)
└── osnos/                     ⇽ todo el código vive acá
    ├── GNUmakefile            (top-level: kernel + libc + ELFs + ISO + sd.img + QEMU)
    ├── limine.conf            (boot entry — carga /boot/kernel)
    ├── kernel-deps/           (Limine freestanding helpers vendoreados)
    │
    ├── src/                   ⇽ KERNEL (~25K LOC)
    │   ├── kernel/main.c      (kmain — init secuence)
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
    │   ├── libc/              (mini-libc local — para ELFs sin musl)
    │   │   ├── crt0.S         (_start → argc/argv/envp → main → _exit)
    │   │   ├── unistd.c stdio.c stdlib.c string.c signal.c termios.c pthread.c ...
    │   │   ├── ox.{c,h} ox_font.c   (cliente del Ox window system)
    │   │   └── include/             (headers: stdio, unistd, fcntl, ..., ox.h, linux/fb.h)
    │   └── sysroot/                 (crti.S / crtn.S para TCC sysroot)
    │
    ├── elfs/                  ⇽ USERLAND (~60 ELFs ring-3)
    │   ├── shell/             (osh — script interpreter legacy)
    │   ├── tools/             (~60 coreutils: ls cat cp mv rm mkdir touch echo ...
    │   │                       + tcc, lua, jq linkeados acá)
    │   ├── net/               (tcpclient, udptest, echotcp, httpd, ...)
    │   ├── tests/             (~28 smoke tests: forktest, sigtest, libctest, hello_musl, ...)
    │   ├── osn-server/        (consrv, kbdsrv, shellsrv — ring-3 servers)
    │   ├── gui/               (oxsrv + oxnotepad + oxcalc + oxterm + oxfiles + oxsettings)
    │   ├── libc.lds           (linker script para ELFs mini-libc-linked)
    │   └── musl.lds           (linker script para ELFs musl-linked)
    │
    ├── vendor/                ⇽ código de terceros vendoreado
    │   ├── tinycc/            (TinyCC 0.9.27, ~30K LOC)
    │   ├── lua/               (Lua 5.4.7, ~24K LOC)
    │   ├── jq/                (jq 1.7.1, ~24K LOC)
    │   ├── musl/              (musl 1.2.5, ~140K LOC) + build-osnos/lib/{libc.a, crt1.o, crti.o, crtn.o}
    │   ├── busybox/           (BusyBox 1.36.1) + osnos-cc-wrapper.sh + busybox_unstripped
    │   └── sqlite/            (SQLite 3.45.2 amalgamation: sqlite3.c + shell.c + sqlite3.h)
    │
    ├── res/                   ⇽ recursos para sd.img
    │   ├── demo.sql           (SQL para regenerar /home/demo.db al build)
    │   ├── demo.db            (DB pre-poblada: 15 books + 4 users + checkouts + view)
    │   └── wallpapers/source/ (opcional: PNG drop-in para Ox wallpapers)
    │
    ├── tools/                 ⇽ scripts host para build
    │   ├── gen_placeholder.c  (genera PPM procedurales si no hay PNG)
    │   └── gen_wallpapers.sh  (wrapper PNG→PPM o procedural fallback)
    │
    ├── ARCH.md                (arquitectura por capas + IPC + syscalls walkthroughs)
    ├── STATUS.md              (estado actual + bitácora de fases — SOURCE OF TRUTH)
    ├── CLAUDE.md              (cheat-sheet operativa para asistente IA)
    ├── CREATE_BUILTINS.es.md  (tutorial: agregar comando kernel-mode)
    ├── CREATE_ELF.es.md       (tutorial: agregar ELF ring-3 contra mini-libc o musl)
    ├── PLAN_FASE10.md         (plan original de la fase 10 — servers a ring 3)
    ├── ROADMAP_APENDICE.md    (roadmap multi-fase)
    └── build/                 (output: kernel ELF, ISO, .o files, ELFs construidos)
```

`sd.img` (32 MiB FAT16) se genera al build y vive al lado de
`osnos/GNUmakefile`. Se monta como IDE primary master en QEMU
(`-drive file=sd.img,if=ide,index=0,media=disk`).

---

## Arquitectura

### Diagrama de capas (post-FASE 13)

```
   ╔════════════════════════════════════════════════════════════════╗
   ║         user apps (ring 3) — busybox + tcc + lua + jq +        ║
   ║         sqlite + oxnotepad + oxcalc + ... + tests              ║
   ║              ↓ syscall (rax=#, rdi/rsi/rdx/r10/r8/r9)          ║
   ╠════════════════════════════════════════════════════════════════╣
   ║                  libc (ring 3 — opt por ELF):                  ║
   ║                                                                ║
   ║  lib/libc/  ←→ programas chicos (coreutils, osn-server, ox)    ║
   ║  musl/      ←→ programas serios (busybox, tcc, lua, jq, sqlite,║
   ║                hello_musl)                                     ║
   ╠════════════════════════════════════════════════════════════════╣
   ║              ring-3 servers (con IPC dirigido):                ║
   ║                                                                ║
   ║   consrv (SERVER_CONSOLE)   →  console output → /dev/fb0       ║
   ║   kbdsrv (SERVER_KEYBOARD)  ←  /dev/input0 → tty_input         ║
   ║   busybox sh (SERVER_SHELL) ←  TTY input, fork+exec apps       ║
   ║   oxsrv (SERVER_OX, opt-in) →  /dev/fb0 via FBIO_BLIT,         ║
   ║                                 mouse, kbd, IPC opcodes 0x60-7F║
   ╠════════════════════════════════════════════════════════════════╣
   ║                       KERNEL (ring 0)                          ║
   ║                                                                ║
   ║   syscall dispatcher (~70 syscalls)  ──→  task / scheduler     ║
   ║         │                                  │  ↓ resume jump    ║
   ║         ↓                                  scheduler_loop      ║
   ║   sys_read/write/open/...           ↓                          ║
   ║   sys_fork/execve/wait/sigaction   ipc queue (64 × 1024B)      ║
   ║   sys_socket/bind/listen/...                                   ║
   ║   sys_arch_prctl (TLS)                                         ║
   ║   sys_mmap/munmap/brk                                          ║
   ║   sys_fcntl(F_SETLK→0)                                         ║
   ║         ↓                                                      ║
   ║   VFS layer (longest-prefix dispatch) ── 16 mount slots:       ║
   ║      "/"     ramfs                                             ║
   ║      "/sys"  sysfs (synthetic)                                 ║
   ║      "/dev"  devfs (fb0, input0, mouse0, tty, ptmx, pts/N, ...)║
   ║      "/sd"   FAT16 (block_ata)                                 ║
   ║      "/bin"  → /sd/bin (aliasfs)                               ║
   ║      "/etc"  → /sd/etc (aliasfs)                               ║
   ║      "/home" → /sd/home (aliasfs)                              ║
   ║      "/lib"  → /sd/lib (aliasfs)                               ║
   ║      "/usr"  → /sd/usr (aliasfs)                               ║
   ║                                                                ║
   ║   TTY line discipline (canon/raw, ECHO, ISIG, EINTR)           ║
   ║         ↓ echo + erase                                         ║
   ║   framebuffer_write_bytes (cursor tracking + serial mirror)    ║
   ╠════════════════════════════════════════════════════════════════╣
   ║                     drivers (ring 0)                           ║
   ║                                                                ║
   ║   PS/2 kbd  PS/2 mouse  framebuffer  serial UART               ║
   ║   block_ata (FAT16)     RTL8139 (TCP/IP)   PIC + LAPIC + PIT   ║
   ╠════════════════════════════════════════════════════════════════╣
   ║   Limine boot → kmain → init drivers → spawn servers → sti     ║
   ╚════════════════════════════════════════════════════════════════╝
```

### Secuencia de boot (`kmain` en `src/kernel/main.c`)

1. `serial_init(COM1)` — UART primero, así panic handlers tienen sink
2. Validar Limine base revision + framebuffer; `framebuffer_init`
3. Memory layer: `pmm_init → vmm_init → kheap_init`
4. CPU + IRQs: `gdt_init → tss_init → idt_init → uaccess_init →
   syscall_msr_init → fpu_init → pic_init → lapic_init → timer_init`
5. Drivers: `block_ata_init` (IDENTIFY primary master), `rtl8139_init`
   (PCI scan; silent si no hay NIC), `net_init` (ARP + RX dispatch)
6. Microkernel state: `ipc_init → pipe_init → pty_init → task_init →
   reaper_init → scheduler_init → syscall_init → ramfs_init →
   bootstrap_fs`
7. Spawn kernel-side feeders (cooperative ring-0):
   - `keyboard` — drena PS/2 → `/dev/input0`
   - `mouse` — drena PS/2 AUX → `/dev/mouse0`
   - `serial-in` — COM1 RX → `tty_input`
8. Spawn ring-3 servers vía `proc_execve`:
   - `/bin/consrv` (registered as `SERVER_CONSOLE`)
   - `/bin/kbdsrv` (registered as `SERVER_KEYBOARD`)
   - `/bin/busybox sh -l` (registered as `SERVER_SHELL`)
     - Fallback: `/bin/shellsrv` (custom legacy shell) si busybox falta
9. `keyboard_server_init()` + `mouse_server_init()`
10. `init-respawn` watchdog task
11. `sti` (enable IRQs)
12. `scheduler_loop` — eternal home, `sched_resume_jump()` longjumpea acá

---

## Estado actual (qué funciona hoy)

Resumen alto nivel. Detalle exhaustivo por fase en
[`osnos/STATUS.md`](osnos/STATUS.md).

| Subsistema | Estado |
|---|---|
| Boot Limine + framebuffer + serial dual-console | ✅ |
| Teclado PS/2 + Mouse PS/2 + `/dev/input0` + `/dev/mouse0` | ✅ |
| GDT + IDT + TSS + paging propio + kheap (32 MiB) | ✅ |
| Microkernel cooperativo + preempt CPL=3 (50ms quantum) + IPC queue 64 | ✅ |
| Syscalls Linux x86_64 (~70) + osnos-specific (>= 500) | ✅ |
| **`restart_syscall` pattern** en sys_read/sys_poll (BusyBox needs) | ✅ |
| **`fs_base` save/restore en task switch + reset en execve** (musl TLS) | ✅ |
| `copy_from_user`/`copy_to_user` con fault recovery (extable) | ✅ |
| VFS + ramfs + sysfs + devfs + binfs + aliasfs + FAT16 (16 mount slots) | ✅ |
| Per-task fd table (16 fds) + OFD pool + pipe + dup/dup2 + fcntl | ✅ |
| `fork(2)` + `execve(2)` + `wait4(2)` + `sigaction(2)` POSIX completo | ✅ |
| Process groups + sessions + Ctrl+C/Z fan-out a pgid + WUNTRACED/WCONTINUED | ✅ |
| EINTR en blocking syscalls (read/wait/nanosleep/accept) | ✅ |
| PTY pairs (`/dev/ptmx` + `/dev/pts/N`, pool 8) | ✅ |
| `mmap`/`munmap` anónimo + brk/sbrk | ✅ |
| FXSAVE/FXRSTOR per-task (FP/SSE multi-task seguro) | ✅ |
| Job control: `&`, `jobs`, `fg`, `bg`, Ctrl+Z, `kill` | ✅ |
| `sys_spawn(2)` con fd inheritance | ✅ |
| **POSIX line discipline TTY** con echo + backspace consistentes (post-FASE-13.3 fix) | ✅ |
| **`/bin/busybox` (1.36.1, musl-linked) como init shell** | ✅ |
| **History persistente cross-reboot** (`/home/.ash_history`) | ✅ |
| **`/etc/profile` + `/home/.ashrc`** estilo .bashrc, banner + PS1 + aliases | ✅ |
| **~30 BusyBox applets**: vi awk sed find diff stat dd df du md5sum sha1sum sha256sum base64 hexdump bc dc more tac fold xargs find timeout ... | ✅ |
| **Coreutils nativos (~60 ELFs)**: ls cat cp mv rm mkdir touch echo wc head tail grep sort uniq cut tr ... | ✅ |
| Glob `*` en shell, pipes \|, redirection > >> <, `;` `&&` `\|\|` | ✅ |
| `/bin/less` con `/pattern` highlight, pipe-mode | ✅ |
| `/bin/ovi` editor modal vim-style (legacy; BusyBox vi también disponible) | ✅ |
| `/bin/term` + `/bin/minishell` sub-shell interactivo en PTY | ✅ |
| `/bin/readelf` ELF inspector | ✅ |
| `/bin/poweroff` + `/bin/reboot` (ACPI S5 / 8042 reset) | ✅ |
| **🎉 `/bin/tcc` — TinyCC 0.9.27 SELF-HOSTING C** | ✅ |
| **🎉 `/bin/lua` — Lua 5.4.7 REPL + scripts** | ✅ |
| **🎉 `/bin/jq` — jq 1.7.1 JSON filter** | ✅ |
| **🎉 `/bin/sqlite3` — SQLite 3.45.2 SQL engine + `/home/demo.db` preseeded** | ✅ |
| **🪟 Ox mini-X window system** (`/bin/oxsrv` + 5 GUI apps) | ✅ |
| init-respawn watchdog (consrv/kbdsrv/shellsrv auto-restart) | ✅ |
| Driver ATA PIO + FAT16 read/write/append + persistencia | ✅ |
| `/bin` disk-resident (sd.img poblado al build via mtools) | ✅ |
| Driver RTL8139 + ARP + IPv4 + ICMP + UDP + TCP completo | ✅ |
| Sockets POSIX + DNS + `/bin/httpd` sirviendo FAT16 | ✅ |
| TTY line discipline POSIX (termios canon/raw, ISIG) | ✅ |
| `/dev/fb0` + `/dev/input0` + `/dev/mouse0` + `/dev/tty` + `/dev/ttyS0` | ✅ |
| FB ioctls Linux-compat: `FBIOGET_VSCREENINFO`, `FBIO_BLIT` | ✅ |
| `/home` alias a `/sd/home` (idem `/etc`, `/bin`, `/lib`, `/usr`) | ✅ |
| `getcwd` / `chdir` syscalls + per-task cwd | ✅ |
| 18/18 tests automatizados via `/bin/alltest` | ✅ |
| SMP (multi-core) | ❌ |
| Copy-on-write para fork | ❌ |
| File-backed mmap | ❌ |
| Real X11 / tinyX (Ox es protocolo propio) | ❌ |

---

## Cómo extender el sistema

Detalle en [`osnos/CREATE_BUILTINS.es.md`](osnos/CREATE_BUILTINS.es.md)
y [`osnos/CREATE_ELF.es.md`](osnos/CREATE_ELF.es.md). Resumen:

### 1. Agregar un comando shell (la forma más común)

Drop `osnos/elfs/tools/foo.c`:

```c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    printf("foo: argc=%d\n", argc);
    return 0;
}
```

Agregar a `USER_ELF_LIBC_SRCS` en `osnos/GNUmakefile`, hacer `make`,
el binario aparece en `/bin/foo` en sd.img y se ejecuta desde el
shell: `foo a b c`.

### 2. Agregar un programa contra musl (para stdio/printf-%f real)

Drop `osnos/elfs/tests/foo_musl.c` (mismo formato). Agregar a
`USER_ELF_MUSL_SRCS` + agregar regla específica en GNUmakefile
copiando la de `hello_musl.elf`. Útil cuando necesitás `printf` con
`%f`, locale completo, pthread shim, math sin gap-fills.

### 3. Agregar un syscall

1. Definir el número en `src/micro/syscall.h`:
   - 0-499 reservado para Linux x86_64 compat (matching numbers)
   - 500+ para osnos-specific (no chocan con Linux)
2. Implementar el handler en `src/micro/syscall.c`:
   ```c
   int64_t sys_foo(int arg1, void *arg2) { ... }
   ```
3. Agregar el case en el dispatcher:
   ```c
   case SYS_FOO: return pack(sys_foo((int)frame->rdi, (void *)frame->rsi));
   ```
4. (Opcional) Wrapper de libc en `lib/libc/foo.c` o equivalente.

### 4. Agregar un server ring-3

Drop `osnos/elfs/osn-server/<name>.c` (libc-linked). Usar
`sys_service_register(SERVER_FOO)` para reclamar el slot, después
loop on `sys_ipc_recv`. Si el server es crítico, agregarlo al
watchdog en `src/kernel/main.c::server_respawn_tick`.

### 5. Agregar un applet a BusyBox

Editar `vendor/busybox/.config` (turn on el `CONFIG_<APPLET>=y`),
rebuild con `cd vendor/busybox && make CC=./osnos-cc-wrapper.sh
HOSTCC=clang -j4`. El binario unstripped resultante se copia
automáticamente a `/bin/busybox` al rebuilder sd.img. Agregar un
alias en `src/fs/bootstrap.c` dentro del seed de `/home/.ashrc` si
querés invocarlo como command nativo (FAT16 no soporta symlinks).

---

## Invariantes que NO se deben romper

1. **ABI Linux x86_64.** Todo número visible a userland (errno,
   syscall numbers, key codes, ELF constants, ioctls, signals,
   sockaddr layouts, getdents records) debe matchear Linux. Goal:
   correr ELFs Linux unmodified contra osnos libc o musl. Para osnos
   non-POSIX usar 500+ (`SYS_ISATTY=500`, `SYS_IPC_*=510+`, etc).

2. **Frontera ABI en `src/include/osnos_*_abi.h`.** Cualquier header
   con sufijo `*_abi.h` es contrato kernel↔userland. Cambiarlo
   requiere recompilar kernel + libc + todos los ELFs.

3. **SYSCALL ABI.** Tanto `int 0x80` como `syscall` van al mismo
   `syscall_dispatch` sobre `syscall_frame_t`. Registros: `rax = #`,
   `rdi/rsi/rdx/r10/r8/r9 = args` (R10 NO RCX, matching Linux).

4. **IPC contract** (`osnos_ipc_abi.h`). Opcode ranges: 0x00-0x0F
   sistema, 0x10-0x1F console, 0x20-0x3F fs/vfs, 0x40-0x5F process
   lifecycle, 0x60-0x7F Ox window system. Cada response setea
   `arg0=status, arg1=size, data=text`. `ipc_send` puede fallar
   con EAGAIN o ESRCH — nunca ignorar el return.

5. **Ramfs slot ownership.** Pointers borrowed de `ramfs_find`
   sobreviven a deletes de OTROS slots.

6. **Scheduling.** Ring-3 preempted por timer 50 ms. **Ring-0
   sigue cooperative** — kernel server que loopea sin retornar
   cuelga el kernel entero. Usar `wakeup_at_ms + state=BLOCKED`
   para tareas kernel periódicas.

7. **Single shared IPC queue** de 64 slots. Outputs de N líneas
   packear en single message; per-line sends desbordan la cola.

8. **No per-task FPU save extensivo aún.** Single-task FP fine;
   mixing FP across multiple ring-3 tasks puede corromper state.
   FXSAVE/FXRSTOR per-task implementado pero no estresado.

9. **QEMU machine type matters.** `block_ata` habla PIO a legacy
   0x1F0; `-M pc` attaches IDE ahí. `-M q35` attaches via AHCI y
   el driver no ve disco.

10. **`framebuffer_write_bytes` es el único path** que mantiene
    cursor consistente entre echo del kernel y output de apps via
    consrv. NO usar `framebuffer_draw_string` directo para echos
    interactivos (rompe REPLs).

---

## Mapa de documentación

| Archivo | Para qué leerlo |
|---|---|
| [`README.md`](README.md) (este) | Pitch + estructura + dependencies + arquitectura overview |
| [`osnos/STATUS.md`](osnos/STATUS.md) | **Source of truth** sobre qué funciona hoy + bitácora de fases + bugs notables + roadmap |
| [`osnos/ARCH.md`](osnos/ARCH.md) | Arquitectura por capas + walkthroughs de IPC y syscalls + flujo Ox + flujo musl |
| [`osnos/CLAUDE.md`](osnos/CLAUDE.md) | Cheat-sheet operativa orientada a asistente IA (también útil para humanos) |
| [`osnos/CREATE_BUILTINS.es.md`](osnos/CREATE_BUILTINS.es.md) | Tutorial: agregar comando kernel-mode (KERN flavor) |
| [`osnos/CREATE_ELF.es.md`](osnos/CREATE_ELF.es.md) | Tutorial: agregar ELF ring-3 (3 flavors: mini-libc, musl, bare) |
| [`osnos/PLAN_FASE10.md`](osnos/PLAN_FASE10.md) | Plan detallado de la fase 10 (servers a ring 3) |
| [`osnos/ROADMAP_APENDICE.md`](osnos/ROADMAP_APENDICE.md) | Apéndice multi-fase del roadmap |

---

## Roadmap

Detalle en [`osnos/STATUS.md`](osnos/STATUS.md). Resumen:

**Corto plazo (FASE 14)** — quality of life:
- Per-PTY termios real (cada shell/REPL su propio termios)
- Fix de argv passing en sqlite3
- `/proc` synthetic (al menos `/proc/<pid>/cmdline`, `/proc/meminfo`)
- Más BusyBox: `top`, `ps`, `free`, `uptime`
- Chip-8 emulator (último item del roadmap original gráfico)

**Mediano plazo (FASE 15)** — drivers a ring 3:
- IRQ delegation por IPC
- MMIO mapping per-task
- Port-IO delegation (IOPB en TSS)
- DMA bouncing
- Portar PS/2, framebuffer, ATA, RTL8139, PIT a `elfs/osn-driver/`

**Futuro lejano**:
- SMP (multi-core)
- Copy-on-write fork
- File-backed mmap
- Real X11/tinyX wire protocol (oxlib es shim hasta que llegue)
- ext2/ext4 read-only
- Más vendor ports (perl tiny, sqlite-net, lua-luarocks, ...)

---

## Inspiración y agradecimientos

- **Limine** por el bootloader limpio (hosted on
  [limine-bootloader/limine](https://github.com/limine-bootloader/limine))
- **musl** por ser una libc legible que se puede entender entera
  ([musl.libc.org](https://musl.libc.org/))
- **BusyBox** por convertir 1 MB de C en una distro funcional
  ([busybox.net](https://busybox.net/))
- **TinyCC** por demostrar que un compilador C puede caber en
  ~30K LOC ([repo.or.cz/tinycc.git](https://repo.or.cz/tinycc.git))
- **SQLite** por ser un milagro de ingeniería en un solo .c
- **Lua** por el design point de "lenguaje embebible" perfecto
- **Beej's Guide to Network Programming** por enseñar sockets sin
  llanto
- **OSDev wiki** por horas de paging + GDT + APIC
- **The C Programming Language (K&R)** por seguir siendo el manual
  de referencia

Y a quien quiera contribuir, abrir issues o usar este código para
aprender — el repo es para eso.

---

## Licencia

Código propio de OSnOS: MIT. Ver [`LICENSE.md`](LICENSE.md).

Código vendoreado en `osnos/vendor/`:
- TinyCC: LGPL 2.1
- Lua: MIT
- jq: MIT
- musl: MIT
- BusyBox: GPL v2
- SQLite: dominio público

Sus licencias respectivas aplican a esos componentes; ver el
header de cada source tree en `vendor/<proyecto>/COPYING` o
`vendor/<proyecto>/LICENSE`.
