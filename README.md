# OSnOS

Sistema operativo hobby x86_64, estilo microkernel, escrito en C desde
cero. Bootea con Limine, corre en QEMU, y trae un shell interactivo,
filesystem virtual, syscalls compatibles con Linux x86_64 y una
mini-libc propia para programas de usuario en ring 3.

```
   OSnOS shell
   /home > ls
   readme.txt
   hello.txt
   /home > cat readme.txt
   bienvenido a osnos.
   /home > hello_libc
   hola desde ring 3 con libc!
   /home > echo persistente > /sd/note
   /home > # reboot
   /home > cat /sd/note
   persistente
   /home > env
   PATH=/bin
   HOME=/home
   PWD=/home
   SHELL=/bin/osh
   TERM=osnos
   /home > httpd &              # auto-exec /bin/httpd
   [3]
   /home > # desde otro host:
   /home > # curl http://localhost:8080/
   /home > ovi .oshrc           # editor modal estilo vim
   # i = insert, Esc = normal, hjkl = move,
   # 0/$ = inicio/fin línea, gg/G = inicio/fin archivo,
   # x = del char, dd = del línea, :w = save, :q = quit
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
- [Licencia](#licencia)

---

## Arrancar en un comando

```sh
./build_and_run.sh
```

Esto: limpia, compila el kernel, arma un ISO booteable y lo arranca en
QEMU (SeaBIOS, no UEFI, porque arranca más rápido).

Subcomandos:

```sh
./build_and_run.sh build   # solo compilar y armar el ISO
./build_and_run.sh run     # bootear un ISO ya existente
./build_and_run.sh clean   # borrar artefactos de build
```

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

`mtools` (mformat + mcopy) se usa para crear `sd.img` — la imagen FAT16
de 16 MiB que el kernel monta en `/sd`. No requiere sudo ni loopback
mount. Si no necesitás disco real podés ignorar la dependencia y borrar
el target `sd.img` del `GNUmakefile`, pero entonces `/sd` no aparece.

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
    │   │                   #   block_ata (ATA PIO sobre IDE primary master)
    │   ├── servers/        #   shell, console, keyboard, fs (en ring 0 hoy)
    │   ├── fs/             #   VFS + backends (ramfs, sysfs, devfs, binfs, fat)
    │   ├── proc/           #   exec, ELF loader, builtins
    │   ├── lib/            #   helpers freestanding (memcpy, strlcpy, printf)
    │   └── include/        #   headers públicos (osnos_status, osnos_keys, ...)
    │
    ├── lib/libc/           # === libc de usuario (ring 3) ===
    │   ├── crt0.S          #   _start → main → _exit
    │   ├── include/        #   stdio.h, stdlib.h, string.h, unistd.h, ...
    │   └── *.c             #   wrappers de syscalls + stdio + malloc
    │
    ├── elfs/               # programas user-mode embebidos en el kernel
    │   ├── shell/          #   osh (mini script interpreter)
    │   ├── tools/          #   ls cat cp mv rm mkdir top calc echo ...
    │   ├── net/            #   tcpclient httpd selectserver udptest ...
    │   ├── tests/          #   libctest ttytest hello_libc user_hello
    │   ├── osn-server/     #   FASE 10: servers movidos a ring 3
    │   └── libc.lds        #   linker script compartido
    │                       # objcopy strippea el dir; ELFs accesibles
    │                       # como /bin/<basename> sin importar carpeta
    │
    ├── build/              # outputs (gitignored)
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

### Diagrama de capas (resumen)

```
┌─────────────────────────────────────────────────────────────┐
│ ring-3 user tasks: ELFs en /bin (hello_libc, ls, cat, osh) │
├─────────────────────────────────────────────────────────────┤
│             lib/libc — mini-libc osnos                      │
│       printf, malloc, fopen, ... → syscall                  │
├─────────────────────────────────────────────────────────────┤
│  Syscall ABI Linux x86_64 (rax + rdi/rsi/rdx/r10/r8/r9)     │
│  int 0x80 (legacy)        ó       syscall (LSTAR)           │
│              ↓                       ↓                       │
│              └─→ syscall_dispatch(frame) ←─┘                │
├─────────────────────────────────────────────────────────────┤
│  shell_server  (line editor + history + tabla de comandos)  │
├─────────────────────────────────────────────────────────────┤
│ keyboard_server │ console_server │ fs_server (vfs wrapper)  │
├─────────────────────────────────────────────────────────────┤
│ VFS: ramfs (/) │ sysfs (/sys) │ devfs (/dev) │ binfs (/bin) │
│                │ fat16  (/sd, persistente sobre sd.img)     │
├─────────────────────────────────────────────────────────────┤
│  IPC: 1 cola, 64 slots, payload 1024B, blocking + wakeup    │
├─────────────────────────────────────────────────────────────┤
│  micro/ core: task, scheduler (preempt @ CPL=3 + coop),     │
│  ipc, gdt/idt/tss, pmm/vmm/kmalloc, syscall, uaccess        │
├─────────────────────────────────────────────────────────────┤
│  drivers/: PS/2, framebuffer + font 8x8, PIC, LAPIC, PIT,   │
│            ATA PIO (block_ata: primary IDE master)          │
├─────────────────────────────────────────────────────────────┤
│                    Limine bootloader                         │
└─────────────────────────────────────────────────────────────┘
```

Diagrama completo + walkthroughs paso a paso ("una tecla viaja por
todo el sistema", "un comando `ls /home` viaja por todo el sistema")
están en [`osnos/ARCH.md`](osnos/ARCH.md).

### Secuencia de boot (`kmain` en `src/kernel/main.c`)

1. Validar Limine + framebuffer → `framebuffer_init`.
2. Memoria: `pmm_init → vmm_init → kheap_init`.
3. CPU: `gdt_init → tss_init → idt_init → uaccess_init → syscall_msr_init`.
4. Interrupciones: `pic_init → lapic_init → timer_init` (PIT @ 100 Hz).
   Acto seguido `block_ata_init` corre IDENTIFY contra el primary IDE
   master; si responde, FASE 8 monta `/sd`.
5. Microkernel: `ipc_init → task_init → reaper_init → scheduler_init →
   syscall_init → ramfs_init → bootstrap_fs (monta `/`, `/sys`, `/dev`,
   `/bin`, y `/sd` si hay FAT16 válido)`.
6. Crear una task por server (`task_create`), registrar IDs
   (`service_register`), llamar a cada `*_init()`. **El orden importa:**
   el shell manda el banner inicial y necesita al console_server ya
   registrado.
7. `scheduler_loop()` — guarda un punto de longjmp y nunca vuelve.

---

## Estado actual (qué funciona hoy)

Resumen alto nivel. Detalle exhaustivo por fase en
[`osnos/STATUS.md`](osnos/STATUS.md).

| Subsistema | Estado |
|---|---|
| Boot Limine + framebuffer | ✅ |
| Teclado PS/2 (Shift, Ctrl, flechas, Ctrl+C) | ✅ |
| Microkernel cooperativo, IPC con queue de 64 | ✅ |
| GDT + IDT + TSS, ring 0/3 selectors | ✅ |
| PMM (bitmap) + VMM (paging 4-niveles propio) + kheap | ✅ |
| Syscalls Linux x86_64 (`int 0x80` + `syscall`) | ✅ |
| `copy_from_user` / `copy_to_user` con fault recovery | ✅ |
| VFS + ramfs + sysfs + devfs + binfs | ✅ |
| Shell con history, flechas, comandos (ls/cat/cp/mv/rm/echo/tree/…) | ✅ |
| Wildcards `*` en ls/cat/rm | ✅ |
| ELF loader (Elf64 ET_EXEC, PT_LOAD, ring 3) | ✅ |
| mini-libc (stdio, stdlib, string, unistd, malloc) | ✅ |
| Programas /bin en ring 3 (hello_libc, ls, cat, osh, top, …) | ✅ |
| Scheduler preemptivo timer-driven (CPL=3, 50 ms quantum) | ✅ |
| Sleep real + Ctrl+C live + background jobs (`&`) + `kill` | ✅ |
| Driver ATA PIO + FAT16 read/write + persistencia en `/sd` | ✅ |
| Driver RTL8139 + ARP + IPv4 + ICMP + UDP + TCP completo | ✅ |
| Sockets POSIX (socket/bind/listen/accept/connect/send/recv/select) | ✅ |
| DNS resolver + getaddrinfo (vía slirp 10.0.2.3) | ✅ |
| `/bin/httpd` sirviendo FAT16 sobre HTTP; `selectserver.c` de Beej verbatim | ✅ |
| TTY line discipline POSIX (termios canonical/raw, ISIG, ioctl) | ✅ |
| Shell con history persistente + `.oshrc` + env (PATH/HOME/PWD) | ✅ |
| `/home` alias a `/sd/home` vía aliasfs (bind-mount VFS) | ✅ |
| `/bin/ovi` editor modal vim-style (hjkl + flechas, i/a/o, :w/:q) | ✅ |
| `getcwd` / `chdir` syscalls + per-task cwd (POSIX) | ✅ |
| Servers en ring 3 (hoy todos ring 0) | ⏳ (fase 10) |
| `fork` / `exec` real | ❌ |
| Multi-core (SMP) | ❌ |

---

## Cómo extender el sistema

Cuatro patrones cubren el ~95% de las extensiones útiles.

### 1. Agregar un comando al shell

Editar `src/servers/shell_server.c`:

```c
static void cmd_hola(const char *args) {
    (void)args;
    console_write("hola humano\n");
}

static const command_t commands[] = {
    // ...
    CMD("hola", cmd_hola, "saluda"),
};
```

La línea de `help` se autogenera. Si tu comando necesita pedirle algo
al fs_server, usá `shell_send_fs1(IPC_FS_*, path)` o
`shell_send_fs2(...)` — devuelven `osnos_status_t` y propagan errores
al usuario.

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

### 4. Agregar un server nuevo

1. `foo_server.{c,h}` exportando `foo_server_init()` y `foo_server_tick()`.
2. Constante `SERVER_FOO` en `src/micro/service.h`.
3. Opcodes nuevos `IPC_FOO_*` en `src/micro/ipc.h` (respetar los rangos
   numéricos: `0x00–0x0F` sistema, `0x10–0x1F` console, `0x20–0x3F`
   fs/vfs, `0x40+` reservado para nuevos).
4. En `kmain`:
   ```c
   int foo_pid = task_create("foo", foo_server_tick);
   service_register(SERVER_FOO, foo_pid);
   foo_server_init();   // ¡este orden importa!
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
- **Scheduler cooperativo.** Una task que entra en un loop infinito
  cuelga el kernel entero. Ctrl+C no salva. Operaciones largas
  simplemente **no deben existir** en este código hasta que llegue la
  fase 9 (preempción real).
- **Cola IPC compartida única.** Outputs de N líneas se empaquetan en
  un solo mensaje (ver cómo `cmd_help` y `cmd_history` construyen un
  buffer con `os_strlcat` y emiten una sola vez). Mandar línea por
  línea desborda la cola y los mensajes se pierden silenciosamente
  como `EAGAIN`.

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

- **Arrow keys + `getcwd`/`chdir`**: `keyboard_server` traduce arrow
  keycodes a `ESC [ A/B/C/D` y los empuja al TTY, así programas raw
  (ovi) navegan con flechas además de hjkl. `SYS_GETCWD` (#79) y
  `SYS_CHDIR` (#80) per-task — `task_t.cwd` se siembra al exec desde
  `PWD=` del envp. `libc resolve_path` ahora consulta `getcwd` y cae
  a `$PWD` si falla.
- **`/bin/ovi` + VT100 mínimo + TIOCGWINSZ**: editor modal vim-flavour
  (hjkl + flechas, i/a/o/O, x/dd, gg/G, $/0, :w/:q/:wq/:q!). El
  framebuffer ahora parsea `ESC[2J`, `ESC[H`, `ESC[r;cH`, `ESC[K`,
  `ESC[7m` (reverse).
- **Shell rc + history persistente**: `/home/.oshrc` se ejecuta al boot
  (con guard anti-recursión, en silencio); `/home/.history` carga al
  inicio y se appendea por comando. Soporta env: PATH/HOME/PWD/TERM/
  SHELL, con `env`/`export`/`unset`. Auto-prefix `/bin/`. `getenv` /
  `setenv` / `execvp` en libc.
- **`/home` como alias de `/sd/home`** vía aliasfs (bind-mount VFS):
  con disco las ediciones a `/home/...` persisten cross-reboot.
- **TTY 1+2** (line discipline + termios ABI Linux): canonical / raw,
  ISIG (Ctrl+C → SIGINT), ioctl TCGETS/TCSETS/TIOCGWINSZ.
- **kheap robusto**: growth dinámico (cap 4 MiB) + slab allocator
  power-of-2 (16…2048) + 28 tests SOCK/KHEAP/SLAB. 603/603 pass.
- **FASE 8.5 — Networking completo**: driver RTL8139 + Ethernet/ARP +
  IPv4/ICMP + UDP + TCP completo (handshake, data, close, listen+accept
  multi-cliente, connect outbound, retransmisión RTO 500 ms) + DNS
  resolver + select/setsockopt. `/bin/httpd` sirve FAT16 sobre HTTP a
  Firefox real; `selectserver.c` de Beej corre verbatim.
- **FASE 9 — Scheduler preempt** con timer-driven quantum sobre CPL=3
  + sleep real + Ctrl+C live + background jobs.
- **FASE 8 — Disco real**: driver ATA PIO + FAT16 read/write sobre
  `sd.img` (16 MiB). `/sd` se monta solo si hay disco, con persistencia
  verificada (escribir → reboot → leer).

Las próximas fases grandes:

1. **Mover servers a ring 3** (FASE 10): shell, fs, console, keyboard
   como procesos de usuario con IPC kernel-mediated.
2. **fork/exec** real (hoy `exec` reemplaza la task actual, no hay
   `fork`).
3. **TUI potente** (FASE 11): mini Norton Commander, viewer, editor
   con flechas — `/bin/ovi` ya tiene hjkl + flechas funcionando, el
   VT100 parser, TIOCGWINSZ y keycode passthrough están listos.
   Falta: multi-pane split, file browser, mouse, syntax highlighting.
4. **Gráfico** (FASE 12): window server + terminal en ventana + mouse.
5. **SMP** (mucho después).

Detalle en `osnos/PLAN.md` y `osnos/STATUS.md`.

---

## Licencia

Pendiente de definir. Por ahora considerá el código como "todos los
derechos reservados, contactar al autor". Las dependencias bajo
`osnos/kernel-deps/` tienen sus propias licencias (mayormente 0BSD /
BSD), respetadas en sus respectivos archivos `LICENSE`.
