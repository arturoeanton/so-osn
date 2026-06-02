# CLAUDE.es.md

> **English version:** [`CLAUDE.md`](CLAUDE.md).

Este archivo da orientación a Claude Code (claude.ai/code) cuando
trabaja con código en este repositorio.

## Estructura del repositorio

`osnos/` es el árbol **self-contained** de kernel + userland. El
template de Limine ya no es un directorio padre — sus helpers
freestanding se vendoreron en `kernel-deps/` y el `GNUmakefile`
top-level construye todo (kernel ELF, libc, ELFs de usuario, ISO,
sd.img) y corre QEMU directamente.

- `GNUmakefile` — Makefile top-level único. Construye el kernel,
  cada ELF de usuario (`elfs/`), la libc (`lib/libc/`), los ports
  TCC/Lua/jq (`vendor/`), la ISO y la `sd.img` FAT16. También
  envuelve `qemu-system`.
- `limine.conf` — entry de boot; carga `boot():/boot/kernel`.
- `kernel-deps/` — helpers freestanding provistos por Limine:
  `cc-runtime/`, `freestnd-c-hdrs/`, `limine-protocol/`,
  `linker-scripts/`, más `get-deps` para bajarlos. El build los
  referencia vía paths `kernel-deps/...`.
- `src/` — fuentes del kernel.
- `lib/libc/` — libc user-side de osnos (compilada por separado con
  `USER_CFLAGS`, NO linkeada al kernel). Los headers en
  `lib/libc/include/` son los que los ELFs `#include`.
- `lib/sysroot/` — stubs `crti.S` / `crtn.S` para el sysroot de TCC.
- `elfs/` — programas user-mode compilados a ELFs ring-3.
  - `shell/` — `osh` (intérprete de scripts).
  - `tools/` — coreutils + extras (~60 entradas: `ls cat cp mv rm
    mkdir touch echo head tail wc grep sort uniq cut tr seq yes
    tee env pwd which printf date uname basename dirname clear tree
    banner calc top kill sleep ovi less readelf poweroff reboot
    minishell term mousetest`, más `tcc`, `lua`, `jq` desde vendor).
  - `net/` — `tcpclient udptest echotcp selecttest selectserver httpd`.
  - `tests/` — smoke tests libc / kernel (~28 entradas: `alltest
    libctest forktest exectest waittest sigtest sigchldtest
    pgrouptest jobtest pipetest ptytest mmaptest fbtest inputtest
    kerntest spawntest envtest fptest ofdtest fdedgetest termtest
    serialtest tcctest luatest jqtest ttytest hello_libc user_hello`).
  - `osn-server/` — **los servers ring-3 reales**: `consrv.c`,
    `kbdsrv.c`, `shellsrv.c` (FASE 10 cerrada).
  - `gui/` — **window system Ox** (FASE 12): `oxsrv.c` (server),
    más cuatro apps cliente GUI: `oxnotepad.c`, `oxcalc.c`,
    `oxterm.c` (PTY + minishell child), `oxsettings.c`.
  - `libc.lds` — linker script compartido por ELFs libc-linked.
  - `tests/user_hello.lds` — linker script propio del ELF bare.
- `vendor/` — `tinycc/` (0.9.27), `lua/` (5.4.7), `jq/` (1.7.1)
  porteados como ELFs ring-3 contra la libc de osnos. **`musl/`
  (1.2.5)** construida como `vendor/musl/build-osnos/lib/{libc.a,
  crt1.o, crti.o, crtn.o}` — los programas ring-3 hacen opt-in
  listándose en `USER_ELF_MUSL_SRCS` (vs el default mini libc
  `USER_ELF_LIBC_SRCS`). Ver FASE 13.0 en STATUS.es.md.
- `tools/` — helpers host-side. `gen_placeholder.c` +
  `gen_wallpapers.sh` construyen los PPMs de wallpapers de Ox al
  momento del build (PNG vía ImageMagick cuando está disponible,
  sino placeholders procedurales con tema).
- `res/wallpapers/source/` — directorio drop-in opcional para
  imágenes fuente `.png` reales con nombres `samurai.png` +
  `girl.png`. El build los detecta automáticamente y los convierte
  a PPM; sino genera placeholders temáticos. Siempre unattended.
- `build/` — todos los outputs (kernel ELF, object files, ELFs
  per-program, ISO en `build/osnos-x86_64.iso`).
- `sd.img` — disk image FAT16 de 32 MiB, reconstruida por el
  Makefile en cada build. Poblada con `/bin/<cada ELF>`, `/home/`,
  `/home/wallpapers/{samurai,girl}.ppm`, `/home/.oxrc`, `/lib/`
  (sysroot TCC: scaffolding crt + `libc.a` + `libtcc1.a`) y
  `/usr/include/` (headers libc completos + freestanding). Usada
  por QEMU como disco IDE primario. mformat se invoca con `-c 8`
  para que el cluster count se mantenga bajo el límite FAT16
  incluso a 32 MiB.

## Build & run

Requerimientos del host: `clang`, `ld.lld`, `xorriso`, `mtools`
(mformat / mcopy / mmd), `qemu-system-x86_64`, y una instalación
de **Limine** del sistema (`brew install limine` en macOS, paquete
de distro en Linux). El Makefile auto-detecta Limine bajo
`/opt/homebrew/share/limine`, `/usr/local/share/limine`, o
`/usr/share/limine`. Override con `make LIMINE_DIR=/path/to/limine ...`.

Desde este directorio:

- `make` (o `make all`) — construye `build/kernel` y `sd.img`.
- `make iso` — además produce la ISO booteable.
- `make run` / `make run-bios` — construye todo y bootea en QEMU
  (SeaBIOS, `-M pc`). Mismo target.
- `make clean` — borra `build/` y `sd.img`.

QEMU se lanza con `-M pc` (NO `q35`) — el driver `block_ata` habla
PIO a los puertos legacy 0x1F0; q35 attachea discos a AHCI y el
driver no vería nada. La NIC es `rtl8139`, con slirp NAT hostfwds
`tcp::8080-:80`, `tcp::9034-:9034`, `udp::1234-:1234` para que
`httpd` y los demos `net/` sean alcanzables desde el host.

Toolchain: `clang` + `ld.lld`. Los CFLAGS del kernel son
`-mcmodel=kernel -mno-80387 -mno-mmx -mno-sse -mno-sse2
-mno-red-zone` etc. Los ELFs de usuario se construyen con
`USER_CFLAGS` — small code model, ring-3 freestanding, **x87 + SSE
+ SSE2 dejados habilitados** (default del SysV ABI; el kernel
nunca toca registros FP, ring-3 puede usar double/float
libremente). El `GNUmakefile` del kernel hace glob de `src/**/*.c`
más `kernel-deps/cc-runtime/src/*.c` vía `find`, así que **nuevos
archivos `.c` bajo `src/` se detectan automáticamente**. `-Werror`
está activo para `src/` (no para cc-runtime / vendor).

## Arquitectura

OSnOS es un OS hobby estilo microkernel chico booteado por Limine
sobre un framebuffer lineal. El kernel hospeda drivers, el VFS,
IPC, el scheduler y una capa de syscalls. Console, política de
keyboard y la shell misma corren como ELFs ring-3 (`/bin/consrv`,
`/bin/kbdsrv`, `/bin/shellsrv`) desde FASE 10. Las user tasks
tienen sus propias page tables y entran al kernel vía `syscall`
(preferido) o `int 0x80` (legacy compat).

Scheduler: timer-driven preemptivo en CPL=3 (50 ms quantum),
todavía cooperativo para tareas ring-0 (los kernel servers deben
yield). PIT @ 100 Hz en IRQ 0.

Ver `ARCH.es.md` para un diagrama completo por capas + walkthroughs
de IPC y syscalls. Ver `STATUS.es.md` para la bitácora corriente de
qué funciona hoy (fase actual: **FASE 14.x** — POSIX make +
AF_UNIX + SHM + dynamic linking; FASE 11.0-11.3 agregaron
self-hosting de TCC/Lua/jq; FASE 13.3 agregó SQLite).

Path de boot (`src/kernel/main.c`, `kmain`):

1. `serial_init(COM1)` — UART primero así los panics tienen sink
   incluso si falla el framebuffer.
2. Validar Limine base revision + framebuffer response;
   `framebuffer_init`.
3. Capa de memoria: `pmm_init -> vmm_init -> kheap_init`.
4. Tablas CPU + IRQ: `gdt_init -> tss_init -> idt_init ->
   uaccess_init -> syscall_msr_init -> fpu_init -> pic_init ->
   lapic_init -> timer_init`.
   - `uaccess_init` registra el span de copy_*_user en la extable
     así un fault dentro del loop desenrolla a EFAULT en vez de
     panicar.
   - `syscall_msr_init` habilita EFER.SCE y programa
     STAR/LSTAR/FMASK para que código ring-3 pueda usar `syscall`.
   - `fpu_init` flipea CR0/CR4 + FNINIT para que ring-3 pueda
     usar SSE/x87.
   - `pic_init` remasquea todas las líneas 8259; `lapic_init`
     habilita LAPIC con LINT0=ExtINT; `timer_init` programa
     PIT@100Hz + IDT[0x20].
5. Init de devices: `block_ata_init` (IDENTIFY primary master),
   `rtl8139_init` (PCI scan; silencioso si no hay NIC), `net_init`
   (ARP + dispatch RX).
6. Estado del microkernel: `ipc_init -> pipe_init -> pty_init ->
   task_init -> reaper_init -> scheduler_init -> syscall_init ->
   ramfs_init -> bootstrap_fs`.
7. Spawn de los feeders kernel-side como tareas cooperativas
   ring-0: `keyboard` (drena PS/2 a `/dev/input0`), `mouse` (PS/2
   AUX -> `/dev/mouse0`), `serial-in` (COM1 RX -> `tty_input`).
8. Spawn de los servers ring-3 vía `proc_execve("/bin/consrv")`,
   `/bin/kbdsrv`, `/bin/shellsrv`, registrando cada uno contra su
   `SERVER_*` ID. Una tarea watchdog `init-respawn` respawnea
   cualquiera de los tres si mueren (e.g. `exec` desde la shell
   interactiva).
9. `keyboard_server_init()` + `mouse_server_init()` para los
   feeders.
10. `sti` para habilitar IRQs.
11. `scheduler_loop()` — guarda un resume point de longjmp al tope
    del `for(;;)`; llamado desde cualquier contexto anidado
    (`sys_exit`, fault handlers) vía `sched_resume_jump()`. Cada
    tick primero drena al reaper, luego dispatcha la próxima
    tarea READY.

### Mapa de subsistemas

- `src/micro/` — core del kernel.
  - `task.{c,h}` — tabla fija de 16 tasks. Estados
    UNUSED/READY/RUNNING/BLOCKED/STOPPED/ZOMBIE/DEAD. Cada task
    carga campos ring-3 (`pml4`, `kernel_stack_*`, `user_entry`,
    `user_stack_top`), iret guardado + set de GPRs (para
    fork/sleep/signals), fd table per-task, `pgid`, `sid`,
    `parent_pid`, tablas de signal disposition, etc.
  - `scheduler.{c,h}` — preemptivo (CPL=3 únicamente) sobre un
    loop cooperativo ring-0. `scheduler_loop` es el host del
    long-jump. El IRQ del timer chequea el quantum; las user tasks
    se preempten, las kernel tasks deben yield retornando.
  - `timer.{c,h}` — PIT @ 100 Hz, `timer_ms()` monotónico.
  - `ipc.{c,h}` + `src/include/osnos_ipc_abi.h` — `ipc_msg_t`
    (1024-byte payload + arg0/arg1, tipado por `ipc_type_t`).
    Cola compartida única de 64. **Los tipos ABI viven en
    `osnos_ipc_abi.h`** así los servers ring-3 ven la misma forma
    de wire; `src/micro/ipc.h` solo tiene los helpers internos del
    kernel. Ring-3 alcanza la cola vía `SYS_IPC_SEND` /
    `SYS_IPC_RECV`. `ipc_send` retorna `osnos_status_t` (OK /
    EAGAIN / ESRCH); los callers DEBEN chequear.
  - `service.{c,h}` — registry name->pid. `SERVER_KEYBOARD=1`,
    `SERVER_SHELL=2`, `SERVER_CONSOLE=3`, `SERVER_FS=4`
    (reservado pero ya no usado — la shell habla VFS directo vía
    syscalls desde FASE 10.3), `SERVER_OX=5` (FASE 12 — Ox
    window system).
  - `gdt.{c,h}` + `tss.{c,h}` — GDT (kcode 0x08, kdata 0x10,
    udata 0x18, ucode 0x20, TSS 0x28). User-data ANTES de
    user-code es requerido para que SYSRET64 caiga en los
    selectores correctos. `tss.rsp0` mirroreado en
    `tss_kernel_rsp0` para el stub de SYSCALL.
  - `idt.{c,h}` — IDT de 256 entradas. `fault_try_recover` corre
    antes de cada panic: RIP kernel-mode en extable -> reescribir
    frame->rip; CPL=3 user-mode -> `proc_exit_current_user(139)`;
    sino panic.
  - `extable.{c,h}` — tabla `{rip_start, rip_end, recovery_rip}`
    para recuperación de page-faults en kernel-mode.
  - `uaccess.{c,h}` — `copy_from_user` / `copy_to_user`, core asm
    redirigido a `__uaccess_copy_bytes_fault` vía la extable.
  - `fpu.{c,h}` — CR0/CR4 + FNINIT al boot. **Sin FXSAVE per-task
    todavía** — uso FP concurrente entre user tasks puede
    corromper estado.
  - `reaper.{c,h}` — cola de kstacks liberados al death de la
    task; drenada al tope de cada `scheduler_tick`. También
    recolecta slots DEAD.
  - `syscall.{c,h}` — números de syscall Linux x86_64 +
    `syscall_dispatch(frame)`. Implementa ~60 syscalls hoy: core
    POSIX completo (`fork`, `execve`, `wait4`, `kill`,
    `rt_sigaction`/`sigreturn`, `pipe`, `dup`/`dup2`,
    `mmap`/`munmap`, `brk`, `nanosleep`, `getdents`,
    `getcwd`/`chdir`, `stat`/`fstat`/`access`, `time`/
    `clock_gettime`, `mkdir`/`rmdir`/`unlink`/`rename`, full BSD
    sockets, `select`, `ioctl` para termios + framebuffer,
    `fcntl`, process groups + sessions, **`writev`, `arch_prctl`
    (TLS vía wrmsr MSR_FS_BASE), `set_tid_address`** — los
    últimos tres son requerimientos del bootstrap de musl), más
    osnos-specific (#250+): `SYS_ISATTY`, `SYS_IPC_SEND/RECV`,
    `SYS_SERVICE_REGISTER/LOOKUP`, `SYS_TTY_INPUT`,
    `SYS_TASKINFO`, `SYS_SPAWN`, `SYS_SET_FG`, `SYS_RESUME`.
  - `int80.c` — stub de entry de IDT[0x80] (ABI compat legacy).
  - `syscall_msr.{c,h}` — programa EFER.SCE, STAR, LSTAR, FMASK.
  - `syscall_entry.c` — stub de entry de la instrucción
    `syscall`. Mirror del stub int80 pero guarda user RSP
    (`syscall_user_rsp`) y preserva RCX/R11 para SYSRET64.
  - `pmm.{c,h}` / `vmm.{c,h}` / `kmalloc.{c,h}` — phys / virt /
    heap.
  - `fd.{c,h}` — fd table per-task (archivos regulares, pipes,
    ptys, sockets, tty, devfs entries). Offsets OFD compartidos
    across `dup`/`fork`.
  - `pipe.{c,h}` — objeto pipe del kernel (ring buffer, ends
    refcounted).
  - `pty.{c,h}` — pool `/dev/ptmx` + `/dev/pts/N` usado por
    `term` y la familia libc `posix_openpt`.
  - `tty.{c,h}` — POSIX line discipline. termios canonical / raw,
    echo, signal generation (Ctrl+C/Z -> SIGINT/SIGTSTP al pgid
    fg), EINTR en reads bloqueados. Backea tanto input PS/2 como
    serial.
- `src/drivers/`
  - `framebuffer` — maneja `\b`, `\n`, `\r`, `\t` en
    `draw_string`; también expone `/dev/fb0`. Dos modos de
    acceso: los writes pasan por el path text ANSI-cooked
    (consrv etc.), y los ioctls FASE 12 `FBIOGET_VSCREENINFO` +
    `FBIO_BLIT` dan al Ox window server acceso directo a píxeles
    vía `framebuffer_get_info` / `framebuffer_blit_kernel`.
  - `keyboard` — PS/2; trackea shift/ctrl, extended `0xE0`;
    emite `keyboard_event_t { ascii, keycode }` (keycode usa los
    valores Linux `input-event-codes.h`).
  - `mouse` — PS/2 AUX; packets de 3-byte ->
    `mouse_event_t { dx, dy, buttons }` empujados a
    `/dev/mouse0` (ring de 32 eventos).
  - `serial` — UART 16550 en COM1. RX/TX + IRQ 4; también expone
    `/dev/ttyS0`. Usado para dual-console y boot headless.
  - `block_ata` — ATA PIO en 0x1F0 (primary master). Backea
    FAT16. Solo se attachea bajo QEMU `-M pc`.
  - `pic` / `lapic` — remap 8259 + enable LAPIC.
  - `pci` — scan de bus; usado por `rtl8139_init`.
  - `rtl8139` — driver RTL8139 NIC (RX ring + TX descriptors).
    Backea `src/net/eth.c`.
- `src/net/` — stack IPv4. `eth -> arp -> ip -> {icmp, udp,
  tcp}`, más `socket.c` (el backend BSD-style fd usado por las
  syscalls de socket).
- `src/servers/` — solo feeders kernel-side (los "servers" reales
  están en `elfs/osn-server/`).
  - `keyboard_server` — `keyboard_poll` -> ring `/dev/input0`.
    NO posee `SERVER_KEYBOARD`; eso pertenece al `kbdsrv` ring-3.
  - `mouse_server` — mismo patrón para `/dev/mouse0`.
  - `serial_input_server` — drena COM1 RX a `tty_input`.
- `src/fs/`
  - `ramfs.{c,h}` — 32 slots x 128B path x 512B data.
    **Invariante de ownership de slot**: el índice de slot es
    estable durante la vida de la entry; deletion marca
    `used=false` sin compactar; el `const ramfs_file_t *` de
    `ramfs_find` queda válido hasta que esa entrada específica
    se borre.
  - `ramfs_vfs.{c,h}` — adapter VFS para ramfs.
  - `fat.{c,h}` + `fat_vfs.{c,h}` — driver FAT16 + adapter VFS.
    Backea el mount `/sd` cuando `block_ata` encuentra un disco.
    Cache de sectores; reads offset-native + append real; long
    names no soportados.
  - `aliasfs.{c,h}` — estilo bind-mount. `bootstrap_fs` aliasea
    `/bin -> /sd/bin`, `/home -> /sd/home`, `/lib -> /sd/lib`,
    `/usr -> /sd/usr` cuando hay disco presente.
  - `sysfs.{c,h}` — `/sys` synthetic read-only (tabla de tasks,
    pending count del ipc, mem stats, ...).
  - `devfs.{c,h}` — `/dev` synthetic (`fb0`, `input0`, `mouse0`,
    `tty`, `ttyS0`, `ptmx`, `pts/N`, ...).
  - `binfs.{c,h}` — `/bin` fallback synthetic sobre el builtin
    registry in-kernel (solo boot diskless).
  - `vfs.{c,h}` — contract del backend (`vfs_ops_t`) + dispatch
    longest-prefix.
  - `bootstrap.{c,h}` — setup de mounts + seeds en boot. Decide
    layout disk-backed vs ramfs-only. En el primer boot con
    disco siembra `/sd/bin/` con cada ELF embebido.
- `src/proc/` — lifecycle de user-task.
  - `builtin.{c,h}` — registry de entradas /bin/*. Tres flavors:
    kernel (C fn), user blob (bytes asm), user ELF
    (`elf_start..elf_end`).
  - `exec.{c,h}` — `proc_execve(path, args, envp)` despacha por
    flavor; `task_create_user_elf` invoca `elf_load`. También
    home de la maquinaria fork/exec/wait (`proc_fork`,
    `proc_exit_current_user`, zombie reaping, notificación al
    parent).
  - `elf.{c,h}` — loader mínimo ET_EXEC ELF64. Camina PT_LOAD,
    aloca + mapea páginas con PTE_U (+ PTE_W cuando PF_W). User
    stack en `0x7FFFE000-0x7FFFF000`.
- `src/lib/` — helpers C freestanding usados por el kernel.
  - `string.c` — `os_strlcpy / os_strlcat / os_strncmp /
    os_strstarts / os_strchr / os_strrchr / os_strlen / os_strcmp
    / os_streq / os_memcpy / os_memset`. Usen estos; no escriban
    su propio loop de copy.
  - `memory.c` — helpers de alignment / page math.
  - `printf.c` — `printf` / `snprintf` del kernel (panic + serial
    logging).
- `src/include/` — headers public, leaf. **Los archivos
  `osnos_*_abi.h` son la frontera kernel<->userland** —
  cambiarlos requiere recompilar kernel + libc + cada ELF.
  - `osnos_ipc_abi.h` — `ipc_msg_t`, `ipc_type_t`, `SERVER_*`,
    `IPC_DATA_SIZE=1024`, `IPC_QUEUE_SIZE=64`. Los rangos de
    opcode incluyen `0x60-0x7F` para mensajes del Ox window-
    system (FASE 12).
  - `osnos_fb_abi.h` — `osnos_fb_var_screeninfo`,
    `osnos_fb_blit_req`, `OSNOS_FBIOGET_VSCREENINFO=0x4600`,
    `OSNOS_FBIO_BLIT=0x4680`. Mirror user-side en
    `lib/libc/include/sys/ioctl.h` y
    `lib/libc/include/linux/fb.h`.
  - `osnos_status.h` — enum de errors. **Los valores numéricos
    matchean Linux x86_64 errno exacto** (`EPERM=1`, `ENOENT=2`,
    `EIO=5`, `EEXIST=17`, ...). Ver invariante de ABI más abajo.
  - `osnos_keys.h` — subset de Linux `input-event-codes.h`.
  - `osnos_elf.h` — subset del layout ELF64 para el loader.
  - `osnos_stat.h` — layout `osnos_stat_t` (mirrorea Linux
    `struct stat`).
  - `osnos_dirent.h` — layout del record getdents64.
  - `osnos_fcntl.h` — open flags, fcntl cmds, file modes.
  - `osnos_taskinfo.h` — record snapshot de `SYS_TASKINFO`.
  - `osnos_limits.h` — `OSNOS_PATH_MAX=128`,
    `OSNOS_NAME_MAX=64`, `OSNOS_INPUT_MAX=128`. Tiene
    `_Static_assert` chequeando que dos paths más slack caben
    dentro de `IPC_DATA_SIZE`.
  - `osnos_path.h`, `font.h`, `theme.h`.

### Userland: libc, ELFs, vendor

- `lib/libc/` — libc user-side (libosnos_c.a + crt0.o). Compilada
  con `USER_CFLAGS` (sin `-mcmodel=kernel`). Headers en
  `lib/libc/include/`: `stdio stdlib string unistd fcntl errno
  dirent signal time math setjmp termios pthread locale libgen
  inttypes ctype alloca assert endian float limits ox` más
  `sys/{ioctl mman mouse reboot select socket stat time types
  wait}`, `arpa/inet.h`, `netinet/in.h`, `linux/fb.h`,
  `osnos_ipc.h`. `ox.h` es la API cliente del window system Ox;
  `linux/fb.h` mirrorea `<linux/fb.h>` de Linux así un futuro
  port de tinyX resuelve los mismos identifiers. Internos:
  `crt0.S` (`_start` -> argc/argv/envp -> `main` -> `_exit`),
  `unistd.c` (convención errno Linux `-1 + errno`), `signal.c` +
  `sigtramp.S` (sigframe + `__sigtramp`), `setjmp.S`,
  `pthread.c`, `termios.c`, `stdio.c` full (BUFSIZ 4KB; `printf`
  soporta `%d %u %x %X %o %c %s %p %%` + flags + width +
  `l/ll/z`), `stdlib.c` (`malloc` sobre `sbrk` first-fit free
  list), `resolver.c` (DNS), `inet.c`, `time.c`, `mman.c`,
  `wait.c`, `dirent.c`, `reboot.c`, `pty.c`, `math.c`,
  `libgen.c`, `locale.c`, `netdb.c`, `errno.c`. **`ox.c`** +
  **`ox_font.c`** implementan el cliente Ox (creación de ventana
  / draw / eventos) — ver `lib/libc/include/ox.h`.
- `elfs/` — ver layout arriba. Patrón: dropear
  `elfs/<categoria>/<nombre>.c`, agregar a `USER_ELF_LIBC_SRCS`
  (libc-linked, provee `main`) o `USER_ELF_SRCS` (bare, provee su
  propio `_start`), y el build lo recoge. Los basenames deben ser
  únicos cross-categorías. `objcopy` strippea componentes de
  directorio al wrappear el ELF en `.o`, así que el símbolo es
  `_binary_<basename>_elf_start/end`. Solo un tiny ROM subset
  (`consrv`, `kbdsrv`, `shellsrv`, `banner`) realmente viaja
  dentro del kernel image; todo lo demás se copia a `::/bin` en
  `sd.img` por el build y se ejecuta desde `/bin` vía el aliasfs
  `/bin -> /sd/bin`.
- `vendor/tinycc`, `vendor/lua`, `vendor/jq` — fuentes
  third-party construidas como ELFs ring-3 contra la libc de
  osnos. El sysroot de TCC (`crt1.o crti.o crtn.o libc.a
  libtcc1.a` + builtin headers) se stagea en `sd.img` en `/lib/`
  y `/usr/include/`, así `tcc hello.c -o hello && ./hello`
  funciona dentro de osnos.

### Invariantes clave

**Linux ABI compat.** Cualquier valor numérico visible a userland
(errno, números de syscall, key codes, ELF constants,
ioctls/signals, sockaddr layouts, getdents records) debe matchear
Linux x86_64. Goal: correr ELFs Linux unmodified contra la libc
de osnos. No inventen números en el rango ocupado por Linux; si
osnos necesita un código no-Linux, reservar arriba de 200
(syscalls) / 250 (usados hoy: `SYS_ISATTY=250`,
`SYS_IPC_*=260..263`, `SYS_TTY_INPUT=264`, `SYS_TASKINFO=265`,
`SYS_SPAWN=266`, `SYS_SET_FG=267`, `SYS_RESUME=268`).

**Frontera ABI en `src/include/osnos_*_abi.h`.** Cualquier header
nombrado `*_abi.h` es el contract wire kernel<->userland. Cambiar
uno significa recompilar kernel + libc + cada ELF, así que el
cambio debería estar respaldado por una nota en `STATUS.es.md`.

**SYSCALL ABI.** Tanto `int 0x80` como `syscall` alcanzan el
mismo `syscall_dispatch` sobre un `syscall_frame_t` compartido.
Contract de registros: `rax = syscall #`,
`rdi/rsi/rdx/r10/r8/r9 = args` (R10, no RCX, matcheando Linux).
El stub SYSCALL también preserva user RCX/R11 en el kernel stack
así SYSRET puede restaurarlos — calls al kernel NO DEBEN
modificar esos dos antes de la restauración sysret.

**IPC contract** (`osnos_ipc_abi.h`). Rangos numéricos de opcode:
`0x00-0x0F` sistema, `0x10-0x1F` console, `0x20-0x3F` fs/vfs,
`0x40-0x5F` process lifecycle (`IPC_PROC_EXITED/STOPPED/
CONTINUED`), `0x60-0x7F` Ox window system (FASE 12: `IPC_OX_*`).
Cada response setea `arg0=status, arg1=size, data=text`.
`ipc_send` puede fallar con EAGAIN (queue full) o ESRCH (no hay
tal servicio) — nunca ignoren el return cuando la correctness
importa.

**Ownership de slot ramfs** (`src/fs/ramfs.h`). Punteros prestados
de `ramfs_find` sobreviven deletes de *otros* slots.

**Scheduling.** Tareas ring-3 son preempted por el quantum de 50
ms del timer; tareas ring-0 siguen **cooperativas** — un kernel
server que loopea sin retornar cuelga el kernel entero. Usar
patron `task_current()->wakeup_at_ms = timer_ms() + N; state =
TASK_BLOCKED;` (ver `server_respawn_tick` en `main.c`) para
tareas kernel periódicas.

**Cola IPC compartida única** de 64 slots. Outputs de N líneas
deben empaquetarse en un solo mensaje (la shell legacy usaba
`os_strlcat` para construir un buffer y emitir una vez). Sends
por línea desbordan la cola y se dropean silenciosamente como
EAGAIN.

**Sin per-task FPU save todavía.** FP single-task (e.g. TCC
compilando en foreground) está bien; mezclar FP entre múltiples
tareas ring-3 concurrentes puede corromper estado. FXSAVE real
llega cuando sea necesario.

**El tipo de máquina QEMU importa.** `block_ata` habla PIO al
legacy 0x1F0; `-M pc` attachea el disco IDE ahí. `-M q35`
attacheria vía AHCI y el driver no vería el disco.

### Agregar un nuevo feeder kernel-side (cooperativo ring-0)

1. Escribir `foo_server.{c,h}` exportando `foo_server_init()` y
   `foo_server_tick()`. Retornar después de una iteración; si
   necesitan pacear, setear
   `task_current()->wakeup_at_ms + state = TASK_BLOCKED`.
2. En `kmain`: `task_create("foo", foo_server_tick)` y llamar a
   `foo_server_init()` después. No registrar contra un
   `SERVER_*` ID a menos que estén reemplazando uno de los
   servers ring-3.

### Agregar un nuevo server ring-3 (el flavor real desde FASE 10)

1. Dropear `elfs/osn-server/<nombre>.c`. Usar
   `sys_service_register(SERVER_FOO)` adentro para reclamar el
   slot de servicio, después loopear sobre `sys_ipc_recv`.
2. Si introducen un servicio nuevo: agregar `SERVER_FOO` en
   `src/include/osnos_ipc_abi.h` (el valor numérico es parte de
   la ABI).
3. Agregar opcodes `IPC_*` nuevos en el rango numérico correcto
   en `osnos_ipc_abi.h`.
4. Agregar el source a `USER_ELF_LIBC_SRCS` y a
   `USER_ELF_ROM_SRCS` (para que se embeba al kernel como ROM
   de recovery).
5. En `kmain`: `proc_execve("/bin/<nombre>", "", 0)` y
   `service_register(SERVER_FOO, pid)`. Agregarlo a
   `server_respawn_tick` así se respawnea si muere.

### Agregar un nuevo comando de shell

La shell ahora es `elfs/osn-server/shellsrv.c` (ring 3). Su
tabla de comandos está ahí — agregar una entrada y el handler
`cmd_foo(const char *args)`.

### Agregar un nuevo builtin

Tres flavors viven en `builtins[]` de `src/proc/builtin.c`:

- `KERN("nombre", fn, "desc")` — un `int fn(const char *args)` C
  corriendo en ring 0 pero usando solo la syscall API.
- `USER("nombre", start, end, "desc")` — un blob flat de código
  máquina x86_64 (típicamente inline asm de file-scope en
  `builtin.c`). Copiado a una sola página de usuario en
  `USER_CODE_VIRT = 0x400000`.
- `USERELF("nombre", start, end, "desc")` — puntero a un ELF64
  embebido. El kernel lo parsea vía `elf_load` y lo dispone por
  program headers.

Agregar un nuevo ELF builtin:

1. Dropear `elfs/<categoria>/<nombre>.c` (elegir una categoría:
   `tools`, `net`, `shell`, `tests`). Para ELFs bare también
   dropear su propio `.lds` al lado (template:
   `elfs/tests/user_hello.lds`); los libc-linked comparten
   `elfs/libc.lds`.
2. Apendar el source a **`USER_ELF_SRCS`** (bare) o
   **`USER_ELF_LIBC_SRCS`** (libc-linked) en `GNUmakefile`. El
   build copia todos los ELFs construidos a `::/bin/<nombre>` en
   `sd.img` (la extensión strippeada, así `/bin/cat` matchea el
   path exec).
3. Si lo quieren embebido al kernel ROM (la mayoría de los ELFs
   no — viven solo en disco), agregar la entry `USERELF(...)` en
   `src/proc/builtin.c` con el extern matching
   `_binary_<basename>_elf_start/end`, y agregar el source a
   `USER_ELF_ROM_SRCS`.

`CREATE_BUILTINS.es.md` cubre el flavor kernel-mode;
`CREATE_ELF.es.md` es el tutorial extenso para ELFs bare hechos a
mano. Para programas libc-linked, el patrón es `int main(int,
char**)` + `#include <stdio.h>` etc.

## Roadmap & status

- `STATUS.es.md` — log corriente de qué funciona hoy, ordenado
  por fase newest-first, con secciones por subsistema. **La
  fuente de verdad más actualizada**; consultar antes de proponer
  dónde encajan features nuevas. Fase actual: **FASE 14.x** —
  POSIX make, AF_UNIX, SHM, dynamic linking, port lighttpd.
  Counterpart en inglés en `STATUS.md`.
- `ARCH.es.md` — diagrama de arquitectura + walkthroughs de
  flujo IPC / syscall. Counterpart en inglés en `ARCH.md`.
- `ROADMAP_APENDICE.md` — apéndice del plan multi-fase.
- `PLAN_FASE10.md` — plan detallado para FASE 10 (servers ring-3).
- `CREATE_BUILTINS.es.md` / `CREATE_ELF.es.md` — tutoriales para
  agregar builtins de kernel / ELFs hand-rolled (solo español por
  ahora).
