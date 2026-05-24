# OSnOS

Sistema operativo hobby x86_64, estilo microkernel, escrito en C desde
cero. Bootea con Limine, corre en QEMU, y trae **BusyBox ash 1.36.1
como shell de sistema (FASE 13.1)** linkeado contra **musl 1.2.5
(FASE 13.0)**, un filesystem virtual con FAT16 persistente, syscalls
compatibles con Linux x86_64 (con restart_syscall pattern para
bloqueo correcto de read/poll), una mini-libc propia para programas
chicos de ring 3, y un **mini-X window system propio llamado Ox
(FASE 12)** con server, file browser, notepad, calculadora, terminal
y settings.

```
   osnos x86_64 — BusyBox ash 1.36.1 + musl (FASE 13.1, init shell)
   
     ___  ____         ___  ____  
    / _ \/ ___| _ __  / _ \/ ___| 
   | | | \___ \| '_ \| | | \___ \ 
   | |_| |___) | | | | |_| |___) |
    \___/|____/|_| |_|\___/|____/ 
   
     osnos — x86_64 microkernel hobby OS
     Type 'help' for a list of shell builtins.
   BusyBox ash on osnos — type help for builtins, ls /bin for commands.
   osnos:/# ls /
   sys/  dev/  sd/  bin/  lib/  usr/  etc/  home/
   osnos:/# ls /etc                    # /etc → /sd/etc aliasfs → FAT16
   passwd  group  hosts  profile
   osnos:/# echo arith=$(( 100 * 7 ))  # ash POSIX arithmetic
   arith=700
   osnos:/# for f in /etc/*; do echo "$f"; done   # globs + loops
   /etc/group
   /etc/hosts
   /etc/passwd
   /etc/profile
   osnos:/# echo persistente > /home/note && cat /home/note
   persistente
   osnos:/# ls /bin | head -n 5
   user_hello
   osh
   hello
   echo
   true
   osnos:/# echo "abc" | tr a-z A-Z              # pipes + tr
   ABC
   osnos:/# uname -srm
   osnos 0.10 x86_64
   osnos:/# env
   PATH=/bin
   HOME=/home
   HISTFILE=/home/.ash_history
   HISTSIZE=500
   PS1=osnos:\w# 
   osnos:/# # ↑/↓ recall history; /etc/profile auto-sourced en login (-l)
   osnos:/# alltest              # 18/18 tests PASS (kernel + libc + ports)
   ALLTEST SUMMARY
     PASS  kerntest    forktest    waittest    sigtest
     PASS  sigchldtest pgrouptest  spawntest   exectest
     PASS  ofdtest     ptytest     fdedgetest  jobtest
     PASS  termtest    libctest    tcctest     luatest
     PASS  jqtest      hello_musl
   RESULT: 18/18 passed
   osnos:/# tcc /home/hello.c -o /home/hello     # ¡SELF-HOSTING C! 🎉
   osnos:/# /home/hello
   hello from tcc on osnos!
   osnos:/# lua                                  # ¡Lua 5.4 REPL!
   Lua 5.4.7  Copyright (C) 1994-2024 Lua.org, PUC-Rio
   > print("hello from lua on osnos")
   hello from lua on osnos
   > = math.sqrt(2)
   1.4142135623731
   > os.exit()
   osnos:/# cat /home/test.json | jq             # ¡jq filter JSON!
   { "os": "osnos", "self_hosting": ["tcc","lua","jq"], ... }
   osnos:/# jq -r '.tools[].name' /home/test.json
   tcc
   lua
   jq
   ovi
   osnos:/# oxsrv &                  # ¡FASE 12 — mini-X GUI! 🪟
   [1] pid=12 &
   # consola desaparece, sale wallpaper samurai, cursor osnos visible.
   # right-click sobre wallpaper → menú estilo Openbox:
   #   Files  Notepad  Calculator  Terminal  Settings  Reboot
   # Click "Notepad"   → ventana de notepad (Ctrl+S guarda)
   # Click "Terminal"  → oxterm con uxsh (PTY: ls, cat, ovi, todo Unix)
   # Click "Settings"  → cambiar wallpaper (samurai ↔ girl) en vivo
   # Click "Files"     → file browser; .ppm en /home/wallpapers/ →
   #                     setea wallpaper; click .txt → abre Notepad
   #                     con ese archivo
   # kill <pid-oxsrv>  → consrv + kbdsrv auto-resumen, shell vuelve
   osnos:/# hello_musl                # ¡FASE 13.0 — musl libc opcional!
   ============================================
     hello from musl libc on osnos
   ============================================
   argv[0] = hello_musl
   pi (musl %f) = 3.1415926536          # printf %f real (la mini-libc no)
   hex: deadbeef  decimal:        -42
   end of musl smoke test — all good
```

> **TL;DR para reentrar al proyecto después de meses:** instalar Limine
> del sistema (no se versiona), correr `./build_and_run.sh`, listo.
> Todo el código vive en [`osnos/`](osnos/). El recorrido recomendado
> para entender la arquitectura es leer este README → `osnos/ARCH.md` →
> `osnos/CLAUDE.md` → `osnos/STATUS.md`.

---

## Tabla de contenidos

- [Arrancar en un comando](#arrancar-en-un-comando)
- [Dependencias](#dependencias)
- [Estructura del repo](#estructura-del-repo)
- [Arquitectura](#arquitectura)
- [Estado actual (qué funciona hoy)](#estado-actual-qué-funciona-hoy)
- [Cómo extender el sistema](#cómo-extender-el-sistema)
- [Invariantes que NO se deben romper](#invariantes-que-no-se-deben-romper)
- [Convenciones de código](#convenciones-de-código)
- [Mapa de documentación](#mapa-de-documentación)
- [Roadmap](#roadmap)
- [Inspiración y agradecimientos](#inspiración-y-agradecimientos)
- [Licencia](#licencia)

---

## Arrancar en un comando

```sh
./build_and_run.sh
```

Esto: limpia, compila el kernel, arma un ISO booteable y lo arranca en
QEMU (SeaBIOS, no UEFI, porque arranca más rápido). En modo gráfico
default la consola serial se persiste a `./serial.log` — útil para
debug + grep sin tocar la ventana de QEMU.

Subcomandos:

```sh
./build_and_run.sh build      # solo compilar y armar el ISO
./build_and_run.sh run        # bootear un ISO ya existente
./build_and_run.sh headless   # boot WITHOUT QEMU window — stdio host
                              # = consola osnos. Ideal para CI / tee.
./build_and_run.sh clean      # borrar artefactos de build
```

**Headless / CI**: `./build_and_run.sh headless` lanza QEMU con
`-nographic -serial mon:stdio`. El stdio del host pasa a ser la
consola serial de la VM — tipear `alltest` + Enter desde la
terminal del host produce el log completo de los 15 tests por
stdout, ideal para capturar con `tee log.txt` o pipe a grep.

**Auto-poweroff para CI**:

```sh
./build_and_run.sh headless <<<'alltest; poweroff' | tee run.log
echo "exit code: $?"
```

`poweroff` dispara ACPI S5 (port 0xB004 + 0x2000) → QEMU cierra y
el script vuelve a la shell del host con su exit code real.

El script detecta macOS vs Linux automáticamente, busca Limine en
`/opt/homebrew/share/limine`, `/usr/local/share/limine` o
`/usr/share/limine`, y elige el backend de display de QEMU correcto
(`cocoa` en mac, `gtk` en Linux). Si Limine está en otro prefix,
exportá `LIMINE_DIR=/ruta/a/limine/share` antes de correr.

Salir de QEMU: cerrar la ventana, o `Ctrl+a` luego `x` en modo serial.

---

## Dependencias

Limine **no** se versiona en este repo. Se instala una sola vez en la
máquina host y el script lo levanta de ahí.

| Herramienta  | macOS (Homebrew)        | Fedora                              | Debian/Ubuntu                      | Arch                       |
|--------------|-------------------------|-------------------------------------|------------------------------------|----------------------------|
| Limine       | `brew install limine`   | `sudo dnf install limine`           | `sudo apt install limine`          | `sudo pacman -S limine`    |
| QEMU         | `brew install qemu`     | `sudo dnf install qemu-system-x86`  | `sudo apt install qemu-system-x86` | `sudo pacman -S qemu-full` |
| xorriso      | `brew install xorriso`  | `sudo dnf install xorriso`          | `sudo apt install xorriso`         | `sudo pacman -S libisoburn`|
| Clang + lld  | `brew install llvm`     | `sudo dnf install clang lld`        | `sudo apt install clang lld`       | `sudo pacman -S clang lld` |
| mtools       | `brew install mtools`   | `sudo dnf install mtools`           | `sudo apt install mtools`          | `sudo pacman -S mtools`    |

`mtools` (mformat + mcopy + mmd) se usa para crear `sd.img` — la
imagen FAT16 de 32 MiB que el kernel monta en `/sd` (con `mformat
-c 8` para que el cluster count se quede dentro del límite FAT16).
Sin sudo ni loopback mount. Pobladx por el build con `/bin/<ELFs>`,
`/home/`, `/home/wallpapers/{samurai,girl}.ppm`, `/home/.oxrc`,
`/lib/` (TCC sysroot: crt + libc.a + libtcc1.a) y `/usr/include/`
(headers libc + freestanding). Si no necesitás disco real podés
ignorar la dependencia y borrar el target `sd.img` del `GNUmakefile`,
pero entonces `/sd`, `/home`, `/bin`, `/lib`, `/usr` desaparecen
salvo el `binfs` fallback con el ROM recovery set (4 ELFs).

Los headers de build (`cc-runtime`, `freestnd-c-hdrs`,
`limine-protocol`, `linker-scripts`) **sí** están vendoreados bajo
[`osnos/kernel-deps/`](osnos/kernel-deps/). Para actualizarlos a
versiones nuevas: `osnos/kernel-deps/get-deps` (clona los commits
fijos definidos en el script).

---

## Estructura del repo

```
.
├── build_and_run.sh        # un comando, cross-platform (mac + Linux)
├── README.md               # este archivo
├── .gitignore
└── osnos/
    ├── GNUmakefile         # compila kernel + arma ISO + corre QEMU
    ├── limine.conf         # entry del bootloader
    ├── kernel-deps/        # deps vendoreadas (cc-runtime, limine-protocol, ...)
    │
    ├── src/                # === el kernel ===
    │   ├── kernel/         #   kmain, panic
    │   ├── micro/          #   core: task, scheduler, ipc, gdt/idt/tss,
    │   │                   #         pmm, vmm, kmalloc, syscall, uaccess
    │   ├── drivers/        #   framebuffer, teclado PS/2, PIC, LAPIC, timer,
    │   │                   #   block_ata, rtl8139
    │   ├── servers/        #   keyboard_server (poll PS/2 → /dev/input0).
    │   │                   #   console/fs/shell migrados a ring 3 (FASE 10).
    │   ├── fs/             #   VFS + backends (ramfs, sysfs, devfs, binfs,
    │   │                   #         fat, aliasfs)
    │   ├── proc/           #   exec, ELF loader, builtins
    │   ├── lib/            #   helpers freestanding (memcpy, strlcpy, printf)
    │   └── include/        #   headers públicos (osnos_status, osnos_keys, ...)
    │
    ├── lib/libc/           # === mini-libc osnos (ring 3, default) ===
    │   ├── crt0.S          #   _start → main → _exit
    │   ├── include/        #   stdio.h, stdlib.h, string.h, unistd.h,
    │   │                   #     ox.h (Ox client API), linux/fb.h, ...
    │   ├── ox.c            #   Ox client wire protocol (IPC 0x60-0x7F)
    │   ├── ox_font.c       #   8x8 bitmap font copy para clients
    │   └── *.c             #   wrappers de syscalls + stdio + malloc
    │
    ├── vendor/             # === third-party vendoreado ===
    │   ├── tinycc/         #   TinyCC 0.9.27 (self-hosting C)
    │   ├── lua/            #   Lua 5.4.7 (self-host interpreter)
    │   ├── jq/             #   jq 1.7.1 (JSON filter)
    │   └── musl/           #   musl 1.2.5 (segunda libc OPT-IN, FASE 13)
    │       └── build-osnos/lib/{libc.a,crt1.o,crti.o,crtn.o}
    │
    ├── tools/              # === host-side helpers ===
    │   ├── gen_placeholder.c  # genera PPM 1280x800 procedural
    │   └── gen_wallpapers.sh  # PNG via convert OR placeholder
    │
    ├── res/wallpapers/source/ # opcional: drop samurai.png / girl.png
    │
    ├── elfs/               # programas user-mode (95+ ELFs ring 3)
    │   ├── shell/          #   osh (mini script interpreter)
    │   ├── tools/          #   coreutils completos: ls cat cp mv rm
    │   │                   #     mkdir rmdir touch echo head tail wc
    │   │                   #     grep sort uniq cut tr seq yes tee
    │   │                   #     env pwd which printf date uname
    │   │                   #     basename dirname clear tree banner
    │   │                   #     calc top kill sleep ovi tcc lua jq
    │   │                   #     less reboot poweroff readelf
    │   │                   #     mousetest minishell term uxsh
    │   ├── net/            #   tcpclient httpd selectserver udptest
    │   │                   #     echotcp selecttest
    │   ├── tests/          #   libctest ttytest fptest mmaptest pipetest
    │   │                   #     forktest waittest sigtest sigchldtest
    │   │                   #     pgrouptest ptytest jobtest tcctest
    │   │                   #     luatest jqtest alltest user_hello (bare)
    │   │                   #     hello_musl (linkeado contra musl libc)
    │   ├── osn-server/     #   FASE 10 servers ring 3:
    │   │                   #     consrv (console), kbdsrv (keyboard),
    │   │                   #     shellsrv (shell de verdad)
    │   ├── gui/            #   FASE 12 — Ox window system:
    │   │                   #     oxsrv (server), oxnotepad, oxcalc,
    │   │                   #     oxterm, oxfiles, oxsettings
    │   ├── libc.lds        #   linker script compartido (libc-linked)
    │   └── musl.lds        #   linker script para ELFs linkeados a musl
    │                       # objcopy strippea el dir; ELFs accesibles
    │                       # como /bin/<basename> sin importar carpeta
    │
    ├── kernel-deps/        # cc-runtime, freestnd-c-hdrs, limine-protocol
    ├── build/              # outputs (gitignored)
    ├── sd.img              # 32 MiB FAT16 — /bin /home /lib /usr /home/wallpapers
    │
    ├── ARCH.md             # diagrama de capas + flujos IPC
    ├── STATUS.md           # qué funciona, qué no, por fase
    ├── PLAN.md             # plan de las próximas fases
    ├── CLAUDE.md           # nota técnica condensada (también útil para humanos)
    ├── CREATE_BUILTINS.es.md # tutorial: agregar builtin kernel-mode
    └── CREATE_ELF.es.md      # tutorial: agregar ELF ring-3
```

---

## Arquitectura

OSnOS es un microkernel con scheduler **preemptivo timer-driven**
(quantum de 50 ms, sólo CPL=3) sobre un core cooperativo en ring 0.
Los subsistemas están separados como si fueran servidores userspace y
se comunican exclusivamente por IPC (excepto los drivers de bajo
nivel). Cuando llegue la migración de servers a ring 3 (fase 10 del
roadmap), mover los servidores debería ser mecánico.

### Diagrama de capas (post-FASE 10)

```
┌─────────────────────────────────────────────────────────────┐
│ ring-3 GUI apps (FASE 12 — Ox mini-X):                      │
│   oxnotepad oxcalc oxterm oxfiles oxsettings — clients      │
│   talk to oxsrv via IPC opcodes 0x60-0x7F                   │
├─────────────────────────────────────────────────────────────┤
│ ring-3 Ox window server (FASE 12):                          │
│   oxsrv  — owns /dev/fb0 (FBIO_BLIT ioctl), cursor, z-order │
│            menu (Openbox-style), wallpaper loader (PPM),    │
│            settings via /home/.oxrc;                        │
│            SUSPENDs consrv + kbdsrv (auto-RESUME watchdog)  │
├─────────────────────────────────────────────────────────────┤
│ ring-3 servers (FASE 10):                                   │
│   consrv  — IPC_CONSOLE_WRITE → /dev/fb0 (SUSPEND/RESUME)   │
│   kbdsrv  — /dev/input0 → sys_tty_input (SUSPEND/RESUME)    │
│   shellsrv — line editor + dispatch + pipes/redirs/jobs     │
│              (registra SERVER_SHELL, ES EL shell del OS)    │
├─────────────────────────────────────────────────────────────┤
│ ring-3 user tasks: ~95 ELFs en /bin (coreutils + net +     │
│   tests + GUI apps + uxsh shell para oxterm)               │
├─────────────────────────────────────────────────────────────┤
│  lib/libc — mini-libc osnos                                 │
│       printf, malloc, fopen, ox.h GUI client, ... → syscall │
│  vendor/musl — musl 1.2.5 OPT-IN (USER_ELF_MUSL_SRCS):      │
│       %f, locale, pthread shim, full snprintf, TLS          │
├─────────────────────────────────────────────────────────────┤
│  Syscall ABI Linux x86_64 (read/write/open/pipe/dup/...):   │
│   SYS_FORK (57) + SYS_EXECVE (59) + SYS_WAIT4 (61) +        │
│   SYS_RT_SIGACTION (13) / SIGRETURN (15) → POSIX core ✅     │
│   SETPGID (109) GETPPID (110) GETPGRP (111) SETSID (112)    │
│   GETPGID (121) GETSID (124) → job control ✅                │
│   SYS_WRITEV (20) SYS_ARCH_PRCTL (158) SYS_SET_TID_ADDRESS  │
│   (218) → musl bootstrap ✅                                  │
│   osnos-specific (≥ 250):                                    │
│   SYS_IPC_SEND/RECV (260/261), SERVICE_* (262/263),         │
│   SYS_TTY_INPUT (264), SYS_TASKINFO (265),                  │
│   SYS_SPAWN (266), SYS_SET_FG (267), SYS_RESUME (268)       │
│   int 0x80 + syscall (LSTAR) → syscall_dispatch(frame)      │
├─────────────────────────────────────────────────────────────┤
│ Framebuffer ioctls (FASE 12): FBIOGET_VSCREENINFO (0x4600)  │
│   FBIO_BLIT (0x4680) — rect blit user buffer → FB.          │
│ IPC opcode ranges:                                          │
│   0x00-0x0F system (incl. CONSOLE/KEYBOARD SUSPEND/RESUME)  │
│   0x10-0x1F console, 0x20-0x3F fs/vfs,                      │
│   0x40-0x5F process lifecycle, 0x60-0x7F Ox window-system   │
├─────────────────────────────────────────────────────────────┤
│ VFS: ramfs (/) │ sysfs (/sys) │ devfs (/dev: null/zero/fb0/ │
│                │                  input0/mouse0/ttyS0/tty/  │
│                │                  ptmx + pts/N)             │
│      aliasfs (/home → /sd/home, /bin → /sd/bin,             │
│               /lib → /sd/lib, /usr → /sd/usr)               │
│      binfs (/bin fallback diskless) │ fat16 (/sd, sd.img)   │
├─────────────────────────────────────────────────────────────┤
│  IPC: 1 cola, 64 slots, payload 1024B. ipc_send rewrite     │
│       SID → pid; ring-3 receivers filtran por t->pid        │
├─────────────────────────────────────────────────────────────┤
│  micro/ core: task (per-task fds[16] slots {used, ofd_idx,  │
│    fd_flags=CLOEXEC} + fpu_state + saved iret/GPRs for      │
│    nanosleep/fork/wait resume + parent_pid + pgid + sid +   │
│    sa_handler[32] + sig_pending + TASK_ZOMBIE state),       │
│    ofd_pool[128] (shared OFDs, dup/fork share offset),      │
│    pipe (+ refcount), pty (pool 8 pairs — /dev/ptmx +       │
│    /dev/pts/N con canon/raw + ECHO), scheduler (preempt @   │
│    CPL=3 + coop), ipc, gdt/idt/tss, pmm/vmm                 │
│    (address_space_clone para fork), kmalloc, syscall,        │
│    uaccess, service                                          │
├─────────────────────────────────────────────────────────────┤
│  drivers/: PS/2 keyboard (AUX-aware: skip mouse bytes),    │
│            PS/2 mouse (AUX poll, 3-byte packets → events), │
│            framebuffer + font 8x8 + VT100 CSI parser +     │
│            FBIOGET_VSCREENINFO + FBIO_BLIT ioctls (FASE 12),│
│            UART 16550 (COM1, serial console),               │
│            PIC, LAPIC, PIT, ATA PIO, RTL8139                │
│  kernel-side cooperative tasks:                             │
│    keyboard feeder (PS/2 → /dev/input0 ring)               │
│    mouse feeder    (PS/2 AUX → /dev/mouse0 ring)            │
│    serial-in feeder (COM1 → tty_input)                      │
│    init-respawn watchdog (consrv/kbdsrv/shellsrv)           │
├─────────────────────────────────────────────────────────────┤
│                    Limine bootloader                         │
└─────────────────────────────────────────────────────────────┘
```

Diagrama completo + walkthroughs paso a paso ("una tecla viaja por
todo el sistema", "un comando `ls /home` viaja por todo el sistema")
están en [`osnos/ARCH.md`](osnos/ARCH.md).

### Secuencia de boot (`kmain` en `src/kernel/main.c`, post-FASE 10 + Fase 2)

1. Validar Limine + framebuffer → `framebuffer_init`.
2. Memoria: `pmm_init → vmm_init → kheap_init`.
3. CPU: `gdt_init → tss_init → idt_init → uaccess_init → syscall_msr_init`.
4. Interrupciones: `pic_init → lapic_init → timer_init` (PIT @ 100 Hz).
   `block_ata_init` corre IDENTIFY contra el primary IDE master.
5. Microkernel: `ipc_init → pipe_init → task_init → reaper_init →
   scheduler_init → syscall_init → ramfs_init → bootstrap_fs`.
6. `bootstrap_fs` monta `/`, `/sys`, `/dev`, `/sd` (FAT16 si hay
   disco). Con disco: aliasfs `/bin → /sd/bin` y `/home → /sd/home`
   (la imagen `sd.img` ya viene poblada por el build script con
   los 64 ELFs en `/sd/bin`). Sin disco: binfs sintético sobre el
   ROM recovery set (consrv, kbdsrv, shellsrv, banner).
7. Crear el keyboard feeder kernel-side (`task_create("keyboard",
   keyboard_server_tick)`) — único server kernel-side.
8. Spawn de los 3 servers ring 3 vía `proc_execve("/bin/consrv")`,
   `/bin/kbdsrv`, `/bin/shellsrv`. Cada uno se auto-registra contra
   su SERVER_*; kmain también lo pre-registra para evitar la race.
9. Spawn del **init-respawn watchdog**: cada ~100ms checkea si los
   3 servers ring-3 siguen vivos y los respawnea si murieron
   (ej. `exec /bin/top` reemplaza shellsrv → echo exits → watchdog
   trae shellsrv de vuelta).
10. `keyboard_server_init()` (hardware), `sti`.
11. `scheduler_loop()` — guarda un punto de longjmp y nunca vuelve.

---

## Estado actual (qué funciona hoy)

Resumen alto nivel. Detalle exhaustivo por fase en
[`osnos/STATUS.md`](osnos/STATUS.md).

| Subsistema | Estado |
|---|---|
| Boot Limine + framebuffer | ✅ |
| Teclado PS/2 (Shift, Ctrl, flechas, Ctrl+C/Z) | ✅ |
| Microkernel cooperativo + preempt CPL=3, IPC queue de 64 | ✅ |
| GDT + IDT + TSS, ring 0/3 selectors | ✅ |
| PMM (bitmap) + VMM (paging 4-niveles propio) + kheap + slab | ✅ |
| Syscalls Linux x86_64 + osnos-specific (>= 500) — `restart_syscall` pattern en `sys_read/poll` para bloqueo correcto | ✅ |
| `copy_from_user` / `copy_to_user` con fault recovery | ✅ |
| VFS + ramfs + sysfs + devfs + binfs + aliasfs + fat16 | ✅ |
| **FASE 10 — Servers en ring 3** (consrv, kbdsrv, shellsrv) | ✅ |
| **Shell ring-3** con line editor + history + flechas + pipes + redir + jobs + `cd ..` + `$VAR` + `$?` + `;` `&&` `\|\|` + `exec` + .oshrc | ✅ |
| **Glob `*`** en shellsrv (matcher recursivo, walk dir, push a argv) | ✅ |
| **Coreutils** (60+ ELFs: ls cat cp mv rm mkdir touch echo wc head tail grep sort uniq cut tr seq yes tee env pwd which printf date uname basename dirname clear tree banner …) | ✅ |
| ELF loader (Elf64 ET_EXEC, PT_LOAD, ring 3) | ✅ |
| mini-libc (stdio, stdlib, string, unistd, malloc, env) | ✅ |
| Per-task fd tables (16 fds) + pipe(2) + dup/dup2/fcntl | ✅ |
| Scheduler preemptivo timer-driven (CPL=3, 50 ms quantum) | ✅ |
| FXSAVE/FXRSTOR per-task (FP/SSE multi-task seguro) | ✅ |
| Job control: `&`, `jobs`, `fg`, `bg`, Ctrl+Z, `kill` | ✅ |
| `sys_spawn(2)` con fd inheritance + `osn_spawn` libc | ✅ |
| **`execve(2)` real** (SYS_EXECVE=59) — in-place ELF replacement, same pid+fds+cwd | ✅ |
| **`fork(2)` real** (SYS_FORK=57) — deep-copy pml4, fd table con pipe refcount bumps, child resumes at saved iret with rax=0 | ✅ |
| **`wait(2)` / `waitpid(2)` real** (SYS_WAIT4=61) + TASK_ZOMBIE state + WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG macros | ✅ |
| **`sigaction(2)` real** (SYS_RT_SIGACTION=13, SYS_RT_SIGRETURN=15) — sa_handler-only, sigframe en user stack, libc __sigtramp epilogue | ✅ |
| **EINTR** en blocking syscalls (`read`/`wait`/`nanosleep`/`recvfrom`/`accept`) cuando llega signal | ✅ |
| **SIGCHLD automático** al child exit + `signal()`/`sigaction()` reales sobre el mismo handler model | ✅ |
| **Process groups + sessions** (`setpgid`/`getpgid`/`setsid`/`getsid`/`getpgrp`/`getppid`) + `kill(-pgid, sig)` broadcast | ✅ |
| **Open File Description (OFD) refactor** — `ofd_pool[128]`, dup/dup2/fork share offset POSIX-strict | ✅ |
| **FD_CLOEXEC** (per-fd, no shared via dup; execve cierra solo CLOEXEC) | ✅ |
| **PTY pairs** (`/dev/ptmx` + `/dev/pts/N`, pool de 8, canon/raw, ECHO, EOF/EPIPE, ioctls TIOCGPTN/TCGETS/TCSETS) + libc `posix_openpt`/`ptsname`/`grantpt`/`unlockpt` | ✅ |
| **WUNTRACED / WCONTINUED** en `wait4(2)` + SIGSTOP/SIGCONT delivery + fan-out de Ctrl+C/Z a TODA la foreground process group + shellsrv migrado a `waitpid()` real (sin polling) | ✅ |
| **Mini terminal emulator** (`/bin/term` spawn `/bin/minishell` en PTY) — sub-shell interactivo, showcase del stack POSIX completo | ✅ |
| **Serial console + `/dev/tty`** — UART 16550 COM1 dual-console (fb + serial siempre on); `./build_and_run.sh headless` bootea sin ventana con stdio del host = consola; panic backtrace persiste en `serial.log` aunque el FB se rompa; `/dev/tty` habilita pipe-mode pagers (`cat foo \| less`) | ✅ |
| **`/bin/less`** pager con `/pattern` + `n`/`N` highlight (pipe-mode: `cat foo \| less` drena stdin + `dup2(/dev/tty, 0)` para keyboard) | ✅ |
| **`reboot(2)`** (#169 Linux ABI) + `/bin/poweroff` + `/bin/reboot` — ACPI S5 (port 0xB004) + 8042 keyboard reset; QEMU cierra limpio, propagable a CI | ✅ |
| **`tail -f`** en `/bin/tail` — poll loop 200ms con EAGAIN/EINTR safe, Ctrl+C exit | ✅ |
| **`/bin/readelf -a`** — ELF header + program headers inspector (LOAD/INTERP/DYNAMIC/...), debug ELFs desde dentro del guest | ✅ |
| **🎉 FASE 11.0 — TinyCC self-hosting** (`/bin/tcc` 0.9.27 portado): compila programas C nativos desde dentro de osnos contra `/lib/libc.a` + `/usr/include/`. Produce ELFs estáticos runnable. `tcc hello.c -o hello && ./hello` funciona end-to-end. Patch crítico en TCC para que static-link convierta PLT32 → PC32 direct (sin GOT). | ✅ |
| **FASE 11.1 polish — FAT true append + offset-native VFS + caching**: O(N) RMW reemplazado con cluster-chain extend O(len); FAT-sector cache en fat_get_entry; BUFSIZ 512→4096. TCC compile time pasó de "tarda mucho" a **instantáneo**. `/bin/readelf -S` para sección headers. | ✅ |
| **🎉 FASE 11.2 — Lua 5.4 self-host** (`/bin/lua` Lua 5.4.7 portado): segundo lenguaje en osnos. REPL interactivo + ejecución de scripts. `ovi script.lua && lua script.lua` end-to-end. Libc gap-fill: `locale.h`, `frexp`/`modf`/`asin`/`acos`/`sinh`/`cosh`/`tanh`, `clock`/`mktime`/`strftime`/`difftime`, `system` stub. | ✅ |
| **🎉 FASE 11.3 — jq 1.7.1 self-host** (`/bin/jq` portado, WITHOUT_ONIG): tercer lenguaje en osnos. Filter + transformer de JSON. `cat data.json \| jq '.field'`, `jq '.list \| length'`, pipes funcionales, builtins. Libc gap-fill: `alloca.h`, `pthread.h` shim single-thread, `libgen.h`, `memmem`, `isnormal`, `realpath`, `rand/srand`. **Bug crítico fixed**: `malloc(0)` ahora retorna non-NULL (glibc-compat). `/home/test.json` seed para jugar. | ✅ |
| **🖱️ FASE 11.4 — PS/2 mouse driver + `/dev/mouse0`**: driver polling PS/2 AUX en `src/drivers/mouse.{c,h}` (3-byte packets, sign extension, sync recovery via bit 3, dy invertido para screen coords), `mouse_server` cooperative kernel task (mirror del keyboard feeder) que pushea a un ring de 32 `mouse_event_t {int16 dx, dy; uint8 buttons}` en devfs. `/bin/mousetest` muestra eventos en vivo. Habilitó la línea gráfica (cursor overlay, file managers con click, eventual TinyX). PIC IRQ 12 sigue masked — polling consume ~1 inb/tick. | ✅ |
| **🪟 FASE 12.0 — Ox mini-X window system**: ring-3 server `/bin/oxsrv` (~700 LOC) owns el framebuffer vía ioctls nuevos `FBIOGET_VSCREENINFO`/`FBIO_BLIT`. Cliente API en `lib/libc/ox.{c,h}` con `ox_init/window_create/draw_rect/draw_text/draw_image/present/poll_event` estilo mini-Xlib. Wire protocol IPC opcodes `0x60-0x7F` (`IPC_OX_CONNECT/WINDOW_CREATE/DRAW_*/EVENT_KEY/MOUSE/EXPOSE/CLOSE/RELOAD_SETTINGS`). 5 apps GUI iniciales: **oxfiles** (file browser con click-to-open), **oxnotepad** (text editor que abre arbitrary path via argv), **oxcalc** (calculadora 4-función), **oxterm** (PTY + uxsh sub-shell, parser ANSI completo: SGR truecolor, cursor positioning, erase), **oxsettings** (wallpaper picker). Wallpapers PPM P6 generados al build (host C + sh script con fallback procedural para samurai/girl si no hay PNGs en `res/wallpapers/source/`). Root menu estilo Openbox via right-click. Coexistencia con consrv/kbdsrv via opcodes nuevos `IPC_CONSOLE_SUSPEND/RESUME` + `IPC_KEYBOARD_SUSPEND/RESUME`: oxsrv los suspende al arrancar, signal handler SIGTERM/SIGINT los resume al exit, watchdog en consrv/kbdsrv auto-resume si `SERVER_OX` desaparece (defensa contra `kill -9`). **Bug crítico fixed** en el camino: `keyboard.c` no chequeaba `STAT_AUX_DATA` del 8042 → bytes del mouse se interpretaban como scancodes (= números random aparecían en apps cuando se movía el mouse). Fix: skip AUX bytes en keyboard_poll. `kbdsrv` opens `/dev/input0` con `O_NONBLOCK`. `ipc_send` ahora rutea SID OR pid directo (server→client events). `sd.img` bumpado 16→32 MiB (wallpapers + GUI ELFs). | ✅ |
| **📝 FASE 12.1 — Polish UX GUI**: (1) `/bin/uxsh` (~140 LOC) mini-shell para oxterm — builtins `cd pwd clear help exit` + fork/execve para todo lo demás, PATH=/bin auto. (2) oxnotepad acepta path via `argv[1]` — file browser ya pasa el path al click. (3) Parser ANSI completo en oxterm: state machine ESC→CSI→final; soporta `ESC[H/f`, `ESC[A/B/C/D` cursor, `ESC[J/K` erase, `ESC[m` SGR (reset, reverse, 30-37/40-47/90-97/100-107, truecolor 38;2;R;G;B). (4) libc stdio `drain_write` retry on EAGAIN (~200 ms backoff) → output largo (TCC compilando, `cat big.txt`) no se trunca silente. (5) Watchdog auto-resume en consrv/kbdsrv. (6) `oxsrv` coalesce mouse MOVE events a 1/frame para no inundar IPC bajo storm. | ✅ |
| **🧬 FASE 13.0 — musl libc opt-in (segunda libc)**: musl 1.2.5 vendoreado en `vendor/musl/` (~140K LOC), compila clean con nuestra toolchain (zero patches al árbol upstream). Outputs en `vendor/musl/build-osnos/lib/{libc.a, crt1.o, crti.o, crtn.o}`. **Kernel gaps cerrados**: `SYS_WRITEV=20` (musl stdio escribe exclusivamente vía writev), `SYS_ARCH_PRCTL=158` (code `0x1002` ARCH_SET_FS → `wrmsr MSR_FS_BASE` = TLS pointer, sin esto cualquier acceso a errno crashea), `SYS_SET_TID_ADDRESS=218` (stub). `build_argv_block` extendido con auxv mínimo `[{AT_PAGESZ=6, 4096}, {AT_NULL=0, 0}]` (sin auxv musl lee bytes random como aux keys). Nuevo `elfs/musl.lds` que preserva `.init_array/.fini_array` + agrega PT_TLS. `elfs/tests/hello_musl.c` smoke test: 6/6 lines correctas end-to-end (auxv parse, TLS wrmsr, argv, snprintf con `%f` que la mini-libc no soporta, `%x` width/padding, exit limpio). Apps opt-in vía `USER_ELF_MUSL_SRCS` en GNUmakefile. | ✅ |
| **🐚 FASE 13.1 — BusyBox ash + login mode + .bashrc-style /home/.ashrc** (default shell de sistema): `/bin/busybox` 1.36.1 linkeado contra musl reemplaza a `shellsrv` como init shell. `proc_execve("/bin/busybox", "sh -l", envp)` con envp pre-poblado `PATH=/bin HOME=/home HISTFILE=/home/.ash_history HISTSIZE=500 TERM=linux` (HISTFILE en envp, NO en /etc/profile, porque ash lo lee antes de cmdloop). Split estilo bash: `/etc/profile` (sourced ONCE en login) sólo trae exports + `ENV=/home/.ashrc`; `/home/.ashrc` (sourced cada shell interactiva via $ENV, mirror exacto de `~/.bashrc`) trae banner ASCII "OSnOS", PS1 verde `osnos:\w# ` y aliases (`ll la l .. h cls`). Usuario edita `.ashrc` con `ovi /home/.ashrc` sin recompilar. In-memory history (up/down arrow) ✓ funciona via FEATURE_EDITING; persistencia a HISTFILE pendiente (binario busybox actual no tiene SAVEHISTORY=y compilado; rebuild bloqueado por cross-compile musl/macOS issues). **4 bugs kernel fixed** para que ash sobreviva: **(1) restart_syscall pattern**: `sys_read`/`sys_poll` rebobinan iret RIP 2 bytes + saved_rax = syscall_nr para que el CPU re-ejecute el syscall al despertar, en vez de longjump-with-rax=0 que ash interpretaba como EOF → exit(0) → infinite respawn loop. Patrón POSIX correcto. **(2) Syscall numbers** osnos-specific 260-268 → 510-518 (chocaban con Linux #262=newfstatat que musl `stat()` invoca). Nuevos: `SYS_LSTAT=6`, `SYS_OPENAT=257`, `SYS_NEWFSTATAT=262`, `SYS_EXIT_GROUP=231`. **(3) `sys_stat` copy_from_user** byte-a-byte hasta NUL (antes pedía 128 B y faulteaba con paths cortos al borde de página). **(4) `VFS_MAX_MOUNTS`** 8→16 (con `/home` aliasfs entraban 9 mounts, el extra perdía). Banner ASCII + welcome aparecen en cada boot/respawn. ↑/↓ recall in-memory history out-of-the-box (FEATURE_EDITING ya estaba on); persistencia cross-reboot a `/home/.ash_history` es best-effort según binario actual de busybox. `shellsrv` queda como fallback si `/bin/busybox` falta (diskless boot). | ✅ |
| **18/18 tests automatizados** via `/bin/alltest` (kerntest, forktest, waittest, sigtest, sigchldtest, pgrouptest, spawntest, exectest, ofdtest, ptytest, fdedgetest, jobtest, termtest, serialtest, tcctest, luatest, jqtest, libctest) | ✅ |
| **init-respawn watchdog** — consrv/kbdsrv/shellsrv auto-restart on death | ✅ |
| Driver ATA PIO + FAT16 read/write + dir-chain extension + NT case-bits + persistencia | ✅ |
| **/bin disk-resident** — sd.img poblado al build via mtools, kernel binary 1.1 MB (era 7.6 MB) | ✅ |
| Driver RTL8139 + ARP + IPv4 + ICMP + UDP + TCP completo | ✅ |
| Sockets POSIX (socket/bind/listen/accept/connect/send/recv/select) | ✅ |
| DNS resolver + getaddrinfo (vía slirp 10.0.2.3) | ✅ |
| `/bin/httpd` sirviendo FAT16 sobre HTTP; `selectserver.c` de Beej verbatim | ✅ |
| TTY line discipline POSIX (termios canonical/raw, ISIG, ioctl) | ✅ |
| `/dev/fb0` + `/dev/input0` character devices | ✅ |
| `/home` alias a `/sd/home` vía aliasfs (bind-mount VFS) | ✅ |
| `/bin/ovi` editor modal vim-style (hjkl + flechas, i/a/o, :w/:q) | ✅ |
| `getcwd` / `chdir` syscalls + per-task cwd (POSIX) | ✅ |
| mmap/munmap anónimo + brk/sbrk | ✅ |
| Pre-populate sd.img al build (Fase 2 final) | ✅ |
| `fork` + `execve` + `wait` + `sigaction` POSIX (ABI core 100% **de verdad** completo) | ✅ |
| Multi-core (SMP) | ❌ |

---

## Cómo extender el sistema

Cuatro patrones cubren el ~95% de las extensiones útiles.

### 1. Agregar un comando al shell

El shell vive en `elfs/osn-server/shellsrv.c` (ring 3). Editás el
array `COMMANDS[]` y agregás tu builtin:

```c
static int do_hola(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("hola humano\n");
    return 0;
}

static const cmd_t COMMANDS[] = {
    // ...
    { "hola", do_hola, "saluda" },
};
```

Como `shellsrv` es un ELF ring-3, tu builtin usa la libc normal
(`printf`, `getcwd`, `open`, etc) — sin acceso directo a estructuras
del kernel. La línea de `help` se autogenera.

Si querés algo más complejo o reutilizable como herramienta
standalone (que también funcione vía pipes/redirects), considerá
hacerlo un ELF separado en `elfs/tools/` — ver opción 2.

### 2. Agregar un programa de usuario en ring 3 (ELF + libc)

Crear `elfs/<categoria>/miprog.c` (elegí la categoría: `tools`, `net`,
`shell`, `tests`):

```c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("hola desde ring 3\n");
    return 0;
}
```

Agregarlo a `USER_ELF_LIBC_SRCS` en `osnos/GNUmakefile`, declarar el
símbolo `_binary_miprog_elf_start/_end` en `src/proc/builtin.c`, y
registrarlo:

```c
extern const char _binary_miprog_elf_start[], _binary_miprog_elf_end[];
USERELF("miprog", _binary_miprog_elf_start, _binary_miprog_elf_end,
        "mi programa"),
```

Tutorial largo en
[`osnos/CREATE_ELF.es.md`](osnos/CREATE_ELF.es.md).

### 3. Agregar un builtin "kernel-mode" (atajo en ring 0)

Para utilidades simples que no justifican un ELF entero. Tutorial en
[`osnos/CREATE_BUILTINS.es.md`](osnos/CREATE_BUILTINS.es.md). La idea:
una función `int fn(const char *args)` que se llama desde el shell y
puede usar la syscall API directamente.

### 4. Agregar un server ring-3 nuevo

Post-FASE-10, los servers son ELFs en `elfs/osn-server/`. Pasos:

1. Crear `elfs/osn-server/foosrv.c`:
   ```c
   #include "osnos_ipc.h"
   #include "osnos_ipc_abi.h"

   int main(int argc, char **argv) {
       (void)argc; (void)argv;
       ipc_service_register(SERVER_FOO);
       for (;;) {
           ipc_msg_t msg;
           if (ipc_recv_block(&msg) != 0) continue;
           switch (msg.type) {
               case IPC_FOO_DO_THING: /* handle */ break;
           }
       }
   }
   ```
2. Constante `SERVER_FOO` en `src/include/osnos_ipc_abi.h` (ABI
   compartido kernel ↔ ring-3).
3. Opcodes nuevos `IPC_FOO_*` en el mismo header (rangos: `0x00–0x0F`
   sistema, `0x10–0x1F` console, `0x20–0x3F` fs/vfs, `0x40–0x5F`
   process lifecycle, `0x60+` reservado para nuevos).
4. Registrar el ELF en `GNUmakefile` (`USER_ELF_LIBC_SRCS`) y
   `src/proc/builtin.c` (`DECLARE_ELF(foosrv); ELF(foosrv, "...")`).
5. En `kmain`:
   ```c
   int64_t foosrv_pid = proc_execve("/bin/foosrv", "", 0);
   if (foosrv_pid > 0) service_register(SERVER_FOO, (uint64_t)foosrv_pid);
   ```

---

## Invariantes que NO se deben romper

Si rompés alguna de éstas, probablemente algo se rompe en un lugar muy
lejano y muy difícil de debuggear. Están repetidas en
`osnos/CLAUDE.md` y como `_Static_assert` cuando es posible.

- **Compatibilidad ABI con Linux.** Cualquier valor numérico que vaya
  a llegar a userland eventualmente (errno, números de syscall,
  keycodes, constantes ELF, layout de `struct stat`, `linux_dirent64`)
  **debe** matchear los valores Linux x86_64 exactos. El objetivo a
  largo plazo es correr binarios Linux sin modificar. Si necesitás un
  código nuevo no-Linux, reservalo arriba de 200.
- **Syscall ABI dual.** `int 0x80` y `syscall` llegan al mismo
  `syscall_dispatch(frame)`. Registros: `rax=número`,
  `rdi/rsi/rdx/r10/r8/r9=args` (`R10`, no `RCX`, como Linux). El stub
  de `syscall` preserva `RCX/R11` para que `SYSRET` los restaure —
  no los pises en kernel.
- **Contrato IPC.** Toda respuesta lleva `arg0=status`, `arg1=size`,
  `data=payload`. **Siempre** chequear el return de `ipc_send` (puede
  ser `OSNOS_EAGAIN` si la cola está llena o `OSNOS_ESRCH` si el
  destinatario no existe). El shell propaga estos errores con
  `report_fs_failure`.
- **Slot ownership de ramfs.** Borrar un slot **no compacta** el
  array; un `const ramfs_file_t *` devuelto por `ramfs_find` sigue
  válido hasta que **ese** slot específico se borre. Crítico para el
  futuro layer de file descriptors.
- **Scheduler con preempción CPL=3.** Tasks ring-3 son preempt cada
  50ms; tasks kernel-side (keyboard feeder) son cooperativas y NO
  deben loopear sin yield. Ctrl+C/Ctrl+Z funcionan en fg tasks
  ring-3 via signal routing del TTY → kernel_fg_pid → task->kill/
  stop_pending.
- **Cola IPC compartida única (64 slots, 1KB/msg).** Outputs largos
  conviene batchearlos. `ipc_send` rewrite msg.to de SID → pid en el
  queue: los ring-3 receivers filtran por su propio pid (no por SID).
  Si la cola se llena, `ipc_send` devuelve `EAGAIN` — los emisores
  hot deben re-encolar o yieldear.
- **ABI Linux + osnos extensions (≥ 250).** Los números osnos-specific
  no chocan con Linux: SYS_IPC_SEND=260, RECV=261, SERVICE_REGISTER=
  262, LOOKUP=263, TTY_INPUT=264, TASKINFO=265, SPAWN=266, SET_FG=
  267, RESUME=268. Cualquier syscall futuro osnos también arriba de
  250.

---

## Convenciones de código

- **Toolchain:** clang + ld.lld. `-Werror` en `src/` (no en
  `cc-runtime`).
- **C estándar:** GNU C11, freestanding, sin libc en el kernel.
- **Strings/memoria:** usar los helpers de `src/lib/string.c`
  (`os_strlcpy`, `os_strlcat`, `os_strncmp`, `os_memcpy`, …). No
  reimplementar copy loops.
- **Errores:** todo lo que pueda fallar devuelve `osnos_status_t`. El
  caller chequea. Sin excepciones, sin globals de errno (excepto la
  libc de usuario, que sí los expone porque Linux ABI).
- **Naming:** `snake_case` para funciones y vars, `CamelCase` evitado
  excepto para tipos que ya tienen ese estilo (`Elf64_Ehdr` etc por
  compat ABI).
- **`os_*` prefix:** para helpers freestanding del kernel que
  sombrearían a la libc (`os_strlcpy` vs `strlcpy`). En `lib/libc/` no
  se usa el prefijo porque ahí sí es libc real.

---

## Mapa de documentación

| Documento | Para qué sirve |
|---|---|
| **`README.md`** (este) | Entrypoint general, build, layout, dónde mirar |
| **`osnos/ARCH.md`** | Diagramas + flujos IPC paso a paso |
| **`osnos/CLAUDE.md`** | Resumen técnico denso (también útil para humanos) |
| **`osnos/STATUS.md`** | Qué funciona hoy, por subsistema, por fase |
| **`osnos/PLAN.md`** | Plan inmediato (próximas tareas concretas) |
| **`osnos/ROADMAP_APENDICE.md`** | Notas largas de roadmap |
| **`osnos/CREATE_BUILTINS.es.md`** | Tutorial builtin kernel-mode |
| **`osnos/CREATE_ELF.es.md`** | Tutorial programa ring-3 |

---

## Roadmap

Cerrado recientemente:

- **FASE 13.0 — musl libc opt-in** (cerrada): musl 1.2.5 vendoreado
  en `vendor/musl/` y compilado clean con nuestra toolchain. Kernel
  agrega 3 syscalls bootstrap (`SYS_WRITEV=20`, `SYS_ARCH_PRCTL=158`
  con ARCH_SET_FS para TLS via wrmsr MSR_FS_BASE, `SYS_SET_TID_ADDRESS=
  218`). `build_argv_block` extendido con auxv mínimo {AT_PAGESZ,
  AT_NULL} — sin esto musl `__init_libc` lee bytes random como
  aux keys. Nuevo linker script `elfs/musl.lds` que preserva
  `.init_array/.fini_array` + PT_TLS. `elfs/tests/hello_musl.c`
  smoke verifica end-to-end: auxv parse, TLS via wrmsr, argv pass-
  through, `snprintf("%f")` (que la mini-libc no soporta), `%x`
  width/padding, exit limpio. Apps opt-in vía `USER_ELF_MUSL_SRCS`
  en `GNUmakefile`. Coexiste con la mini-libc: programs chicos
  siguen usándola (footprint reducido), apps que necesitan
  stdio/printf-%f/locale/pthread reales usan musl. **Limitación
  pendiente**: `printf`/`puts` via FILE* retorna -1 (la cadena
  `__ofl_lock` o init lazy de stdout falla; `snprintf` + raw
  `write(2)` funcionan).

- **FASE 12.1 — Polish UX GUI** (cerrada): (1) `/bin/uxsh` mini-shell
  (~140 LOC) para oxterm: builtins cd/pwd/clear/exit/help + fork-
  execve para todo lo demás con PATH=/bin lookup. (2) oxnotepad
  acepta path via `argv[1]` (file browser pasa el path al click).
  (3) Parser ANSI completo en oxterm: state machine ESC→CSI→final,
  soporta cursor positioning (`ESC[H/A/B/C/D`), erase (`ESC[J/K`),
  SGR (reset, reverse, 30-37/40-47/90-97/100-107, **truecolor 38;2;
  R;G;B + 48;2;...**). (4) libc stdio `drain_write` retry on EAGAIN
  (~200ms cap) — output largo no se trunca silente (TCC compilando,
  `cat big.txt`). (5) Watchdog en consrv + kbdsrv: chequea
  `service_lookup(SERVER_OX)` cada ~500ms; si oxsrv desapareció
  (`kill -9`, crash) auto-RESUME. (6) `oxsrv` coalesce mouse MOVE
  events a 1/frame (no IPC storm).

- **FASE 12.0 — Ox mini-X window system** (cerrada): server `/bin/
  oxsrv` (~700 LOC) owns el framebuffer vía 2 ioctls nuevos del
  driver (`FBIOGET_VSCREENINFO=0x4600`, `FBIO_BLIT=0x4680`).
  Cliente libc en `lib/libc/ox.{c,h}` con API estilo mini-Xlib
  (`ox_init/window_create/draw_rect/draw_text/draw_image/present/
  poll_event/wait_event`). Wire protocol IPC opcodes `0x60-0x7F`
  (CONNECT/WINDOW_CREATE/DESTROY/DRAW_*/PRESENT/SET_TITLE/EVENT_KEY/
  MOUSE/EXPOSE/CLOSE/RELOAD_SETTINGS/RESPONSE). 5 apps GUI: **oxfiles**
  (file browser), **oxnotepad** (text editor con argv[1] path),
  **oxcalc** (calculadora), **oxterm** (PTY + uxsh, parser ANSI),
  **oxsettings** (wallpaper picker). Wallpapers PPM P6 generados al
  build via `tools/gen_wallpapers.sh` (ImageMagick si está, sino
  placeholder procedural samurai/girl en `tools/gen_placeholder.c`,
  zero intervención). Menú estilo Openbox vía right-click. Nuevos
  opcodes `IPC_CONSOLE_SUSPEND/RESUME` + `IPC_KEYBOARD_SUSPEND/
  RESUME` permiten que oxsrv tome posesión exclusiva del FB + ring
  de keyboard sin pelearse con consrv/kbdsrv. Signal handler SIGTERM/
  SIGINT envía RESUME al exit. `ipc_send` ahora rutea SID O pid
  directo (server→client events). **Bug crítico fixed** en el
  camino: `keyboard.c` no chequeaba `STAT_AUX_DATA` del 8042 →
  bytes del mouse se interpretaban como scancodes (números random
  aparecían en apps al mover mouse). `sd.img` bumpado 16→32 MiB
  con `mformat -c 8` para que cluster count siga ≤65525 (FAT16).

- **FASE 11.4 — PS/2 mouse driver + `/dev/mouse0`** (cerrada):
  driver polling PS/2 AUX en `src/drivers/mouse.{c,h}` (init via
  0xA8 + 0xF6 + 0xF4 con ACK loop bounded; poll de 3-byte packets
  con sign extension + sync recovery por bit 3 + dy invertido).
  `mouse_server` cooperative kernel task pushea hasta 16 events/tick
  a un ring de 32 `mouse_event_t` en devfs. `/dev/mouse0` char
  device read-only (EROFS en write, EAGAIN en empty ring). Userland
  ABI en `<sys/mouse.h>` (`mouse_event_t` + `MOUSE_BTN_LEFT/MIDDLE/
  RIGHT`). `/bin/mousetest` muestra `dx/dy/buttons + abs(x,y)` en
  vivo. **Habilita la línea gráfica futura** (cursor overlay,
  window system, eventual TinyX); IRQ 12 sigue masked porque el PIC
  está unwired global — polling cost ~1 inb por tick (imperceptible).

- **FASE 11.2 — Lua 5.4 port** (cerrada): `/bin/lua` (Lua 5.4.7
  vendored a `vendor/lua/`) — REPL interactivo + script runner.
  Segundo lenguaje self-host (después de C/TCC). Compilado con
  `LUA_USE_C89` path (ISO C only) — no signal handlers, no readline,
  no popen, no os.execute. Libc gap-fill: `locale.h/c` (C-locale
  stub), math.h gana `asin/acos/sinh/cosh/tanh/frexp/modf`,
  time.h gana `clock/mktime/difftime/strftime`, stdlib gana
  `system` stub, stdio gana `tmpnam/remove/L_tmpnam`. `/bin/luatest`
  smoke automatizado. Hito: osnos ahora ejecuta scripts Lua
  interactivamente sin tocar host.

- **FASE 11.1 polish — FAT true append + offset-native VFS** (cerrada):
  `fat_extend_existing` reemplaza el O(N) RMW-the-whole-file con
  cluster-chain extend real (O(len) por call). `fat_get_entry`
  caching del último FAT sector. `BUFSIZ` libc bumped 512 → 4096.
  Combined: TCC compile time pasó de "tarda mucho" a instantáneo.
  `/bin/readelf -S` ahora muestra section headers (debugging
  output de TCC, etc.). `/bin/tcctest` smoke automatizado. 16/16.

- **FASE 11.0 — TinyCC port + self-hosting** (cerrada): `/bin/tcc`
  (TinyCC 0.9.27 vendored a `vendor/tinycc/`) compila programas C
  desde dentro de osnos. Stack completo: TCC source ~30K LOC
  cross-compilado con USER_CFLAGS, libc gap-fill (`ldexp`, `strtod`,
  `struct tm`, `fdopen`, `mprotect` no-op, `sscanf`, `gettimeofday`),
  TCC headers osnos-trimmed (stdarg.h sin anon-union, stdint.h sin
  builtins clang-only), aliasfs mounts `/lib + /usr → /sd/`, sysroot
  con `crt1/crti/crtn/libc.a/libtcc1.a` en disco. **2 bugs críticos
  de VFS encontrados**: `sys_read` truncaba files >1KB (stack scratch
  1024B) → reescrito offset-native con `vfs_read_at(off)` API +
  backend offset support en ramfs/fat/devfs/sysfs/binfs/aliasfs;
  `fat_append_path` truncaba writes >8KB (static scratch) → reescrito
  kmalloc 4MB cap. **Patch crítico en TCC**: static-link convierte
  `R_X86_64_PLT32 → R_X86_64_PC32` direct cuando símbolo resuelto,
  evita PLT/GOT vacíos que jumpaban a RIP=garbage sin dynamic loader.
  Output: ELF EXEC limpio con solo LOAD segments. `tcc hello.c -o
  hello && ./hello` end-to-end funcional. **osnos es self-hosting**.

- **FASE 10 — Servers a ring 3** (cerrada completa): console + keyboard
  + shell viven como ELFs en `elfs/osn-server/`. kmain solo bootea
  drivers + spawnea los 3 servers + scheduler. Trayecto:
  - 10.0 pre-reqs: per-task fd tables (16/task), pipe(2) syscall,
    /dev/fb0 + /dev/input0, osnos_ipc_abi.h compartido, SYS_TASKINFO,
    /bin/kerntest ELF de ABI tests.
  - 10.1 consrv: ring-3 console server, IPC_CONSOLE_WRITE → /dev/fb0.
  - 10.2 kbdsrv: ring-3 keyboard policy, /dev/input0 → sys_tty_input.
  - 10.3 fs_server eliminado (-290 LOC): shell habla VFS directo.
  - 10.4 shellsrv: shell ring-3 con line editor (raw mode + history +
    flechas + cursor visible), pipes/redirects/jobs, fg/bg/Ctrl+Z,
    $VAR expansion, .oshrc, export/unset, PATH search.
- **Disk-resident /bin (Fase 1 + Fase 2)**: bootstrap dumpea los ROM
  ELFs (consrv/kbdsrv/shellsrv/banner) a /sd/bin al primer boot, y
  el **build script** (GNUmakefile via mtools) **popula sd.img con
  los 64 ELFs al hornear**. aliasfs /bin → /sd/bin. exec.c prefiere
  disco; embedded ROM es fallback. FAT16 dir-chain extension fix
  permite >9 entries por subdir. NT case-bits respetados →
  lowercase SFNs (hello/head/httpd) visibles correctamente.
  **Kernel binary: 7.6 MB → 1.1 MB (85% reducción)**.
- **execve(2) real** (SYS_EXECVE=59): in-place ELF replacement.
  `proc_execve_replace` swap pml4/entry/stack/heap, address_space_destroy
  el viejo, sched_resume_jump. Preserva pid + fds + cwd. libc
  `execve` + `execv` + `execvp` listos. `exec` builtin en shellsrv
  con osn_set_fg(getpid) para que Ctrl+C funcione tras el exec.
- **fork(2) real** (SYS_FORK=57): deep-copy del user pml4 via nuevo
  `address_space_clone` (full page copy, no COW todavía). Clona la
  fd table con pipe refcount bumps (nuevo `pipe_dup_reader/writer`).
  Snapshot del iret frame + GPRs del parent al kstack del child →
  child resume via `user_task_trampoline` con `rax=0`. Parent
  retorna child pid. libc `fork()` listo. `/bin/forktest` verifica
  semánticas (pid distintos, fd inheritance, stack COW correcto,
  exit code propagation).
- **wait(2) / waitpid(2) real** (SYS_WAIT4=61) + nuevo estado
  `TASK_ZOMBIE`: child muerto queda zombie con exit_code preservado
  hasta que parent waitea; orphan se va directo a DEAD. parent
  BLOCKED en wait4 se despierta vía proc_exit_current_user del
  child (escribe status vía vmm_lookup en el pml4 del parent,
  saved_rax = child_pid). WNOHANG retorna 0; ECHILD si no hay
  children. libc `wait()` + `waitpid()` + WIFEXITED/WEXITSTATUS/
  WIFSIGNALED/WTERMSIG macros. `/bin/waittest` valida.
- **sigaction(2) real** (SYS_RT_SIGACTION=13, SYS_RT_SIGRETURN=15)
  modelo sa_handler-only: handlers user-mode reales que reciben
  signum. Delivery en `user_task_resume` antes del iretq: pushea
  sigframe (orig iret+GPRs) al user stack, redirige rip=handler /
  rsp=sigframe_base / rdi=signum; el ret del handler salta al
  libc `__sigtramp` (sigtramp.S) que llama SYS_RT_SIGRETURN para
  restaurar. SIG_DFL = exit con 128+sig, SIG_IGN = swallow.
  SIGKILL/SIGSTOP uncatchable (POSIX). `/bin/sigtest` valida
  raise(SIGUSR1)/SIG_IGN/SIGKILL/signal()-wrapper/fork+SIGTERM.
- **EINTR** en blocking syscalls: cuando un signal llega a un task
  BLOCKED (nanosleep/wait4/etc), sys_kill / tty_signal escriben
  `saved_rax = -EINTR` antes de despertar; el handler corre primero
  y al volver, syscall retorna -1 + errno=EINTR. POSIX-compliant.
- **ABI POSIX core 100% COMPLETO**: read/write/open/close/pipe/dup/
  fork/execve/exit/kill/wait/sigaction/sigreturn/EINTR.
- **FASE 10.6 — Job control + OFD + PTY** (3 sesiones):
  - **Sesión 1** (SIGCHLD + process groups): SIGCHLD automático al
    child exit; task_t.pgid + sid + 6 syscalls Linux x86_64
    (setpgid/getpgid/setsid/getsid/getpgrp/getppid); sys_kill
    POSIX-completo (pid>0, pid==0 own pgid, pid==-1 broadcast,
    pid<-1 target pgid); `/bin/alltest` runner consolidado.
  - **Sesión 2** (OFD refactor + FD_CLOEXEC): nuevo `osnos_ofd_t`
    en pool de 128, refcounted. `osnos_fd_slot_t` thin per-task
    (12 B vs 150+ B antes). dup/dup2/fork comparten OFD =
    shared offset POSIX-strict. fork ya no llama pipe_dup_*
    (OFD refcount lo maneja). sys_spawn MOVE semantics.
    proc_execve_replace cierra fds CLOEXEC. Backend cleanup
    automatic via `ofd_unref`.
  - **Sesión 3** (PTY pairs): `/dev/ptmx` + `/dev/pts/N` con
    pool de 8 pty_pair_t; canon/raw mode con line accumulator;
    ECHO + VERASE; EOF cuando master cierra; EPIPE cuando slave
    escribe sin master. ioctls TIOCGPTN/TCGETS/TCSETS per-pair.
    libc `posix_openpt`/`ptsname_r`/`grantpt`/`unlockpt`.
  - **Sesión 4** (WUNTRACED + WCONTINUED + fan-out + shellsrv
    migrate): `task_t.wait_change` enum trackea state transitions;
    `find_waitable_child` detecta ZOMBIE/STOPPED/CONTINUED.
    Nuevo `notify_parent_stop_continue` despierta parents en
    wait4 cuando child cambia state. user_task_resume SIG_DFL
    para SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU → TASK_STOPPED.
    sys_kill SIGCONT a STOPPED → resume + WAIT_CONTINUED.
    **Fan-out de Ctrl+C/Z a fg pgid** completo (tty_signal +
    tty_stop_signal walk task table broadcasting). shellsrv
    `wait_pid_capture` migrado de polling sys_taskinfo + nanosleep
    a `waitpid(pid, &status, WUNTRACED)` real — no más race vs
    reaper. libc `<sys/wait.h>` gains `WCONTINUED` flag +
    `WIFCONTINUED(s)` macro.
  - Tests nuevos: `/bin/sigchldtest`, `/bin/pgrouptest`,
    `/bin/ofdtest`, `/bin/ptytest`, `/bin/fdedgetest`,
    `/bin/jobtest`, `/bin/alltest`. **13/13 PASS** end-to-end.
- **init-respawn watchdog**: kernel task que checkea cada ~100ms
  si consrv/kbdsrv/shellsrv siguen vivos; si murieron (e.g. exec
  /bin/top + Ctrl+C), los respawnea + re-registra SERVER_*.
  Sleep via BLOCKED + wakeup_at_ms — sin spin.
- **Shellsrv polish FINAL**: `;` / `&&` / `||` operators con
  tracking de `$?` (exit_code agregado a osnos_taskinfo_t); glob
  `*` con matcher recursivo + walk dir; `cd` con `..` `.` paths
  relativos; `ls /` muestra mount points sintéticos; `do_ls`
  POSIX multi-arg (files + dirs); ovi output buffering 16 KB +
  framebuffer chunk safe-split (CSI no parte); task_t.name inline
  32 B; ipc_send rewrite SID→pid; `$VAR` / `${VAR}` expansion +
  `export` / `unset` / `setenv` + .oshrc auto-load + opendir
  valida ENOTDIR + task_reap_dead grace de 4 pasadas para que
  shellsrv capture exit_code de tasks fast-finishing.
- **Coreutils completos**: env, wc, pwd, uname, basename, dirname,
  tail, seq, yes, tee, date, printf, grep, sort, uniq, cut, tr,
  banner, which, clear, tree — todos como ELFs disk-resident.
- **FASE 8.5 — Networking completo**: driver RTL8139 + Ethernet/ARP +
  IPv4/ICMP + UDP + TCP + DNS + sockets POSIX. `/bin/httpd` sirve
  FAT16 sobre HTTP a Firefox real; `selectserver.c` de Beej verbatim.
- **FASE 9 — Scheduler preempt** + sleep real + Ctrl+C/Z live.
- **FASE 8 — Disco real**: ATA PIO + FAT16 read/write + persistencia.

Las próximas fases grandes:

1. **FASE 13.1 — musl printf path fix**: arreglar `__ofl_lock` /
   stdout init lazy de musl para que `printf` via FILE* funcione.
   Una vez listo, migrar oxnotepad / oxterm / oxfiles a musl
   (acceso a wide chars, locale, regex serio).
2. **FASE 14 — Dirty regions en oxsrv**: blit solo los rects que
   cambiaron (cursor anterior + nuevo + window dirty) en vez de full-
   screen blit por frame. Mata la vibración del cursor + baja CPU/MMIO
   ~95%. ~200 LOC en `oxsrv.c`.
3. **FASE 15 — Drivers a ring 3**: PS/2, framebuffer, ATA, RTL8139
   como ELFs en `elfs/osn-driver/`. Requiere IRQ delegation, MMIO
   mapping per-task, port-IO syscall, DMA bouncing.
4. **FASE 16 — Window mgmt avanzado**: resize/drag de ventanas,
   minimize/maximize, taskbar, multi-monitor.
5. **FASE 17 — tinyX / X11 port**: aprovechar que ya tenemos auxv +
   TLS + ioctls FB + IPC client/server protocol — tinyX puede portar
   contra `<linux/fb.h>` que ya exponemos.
6. **SMP** (mucho después).
7. **Copy-on-write fork** — hoy fork hace full page copy. Con COW
   se ahorra RAM hasta el primer write.

Detalle en `osnos/PLAN.md` y `osnos/STATUS.md`.

---

## Inspiración y agradecimientos

Este proyecto nació viendo este video:

📺 **[¿Y si hacemos un sistema operativo? (YouTube)](https://youtu.be/JU3Fn-cL88Q?si=7Bxp5a5zN2kjKuDL)**

Me dio las ganas de hacerlo y un mapa mental de por dónde arrancar.
Gracias enormes al autor por compartir el camino — sin ese video,
osnos no existiría. Si te interesa entender desde cero cómo funciona
un OS y querés algo que te empuje a probarlo vos mismo, mirá el video
primero.

---

## Licencia

Pendiente de definir. Por ahora considerá el código como "todos los
derechos reservados, contactar al autor". Las dependencias bajo
`osnos/kernel-deps/` tienen sus propias licencias (mayormente 0BSD /
BSD), respetadas en sus respectivos archivos `LICENSE`.
