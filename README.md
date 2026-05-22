# OSnOS

Sistema operativo hobby x86_64, estilo microkernel, escrito en C desde
cero. Bootea con Limine, corre en QEMU, y trae un shell interactivo,
filesystem virtual, syscalls compatibles con Linux x86_64 y una
mini-libc propia para programas de usuario en ring 3.

```
   osnos x86_64 — shellsrv (ring 3, FASE 10.4)
   shellsrv:/home$ ls
   README.TXT  HELLO.TXT  .oshrc  .history
   shellsrv:/home$ cat README.TXT
   Welcome to osnos.
   shellsrv:/home$ hello
   hello, world
   shellsrv:/home$ echo persistente > /sd/note
   shellsrv:/home$ # reboot
   shellsrv:/home$ cat /sd/note
   persistente
   shellsrv:/home$ env
   PATH=/bin
   HOME=/home
   SHELL=/bin/shellsrv
   OSNAME=osnos
   shellsrv:/home$ ls /home | grep TXT | sort   # full pipes + redirects
   HELLO.TXT
   README.TXT
   shellsrv:/home$ httpd &        # background
   [1] pid=7 &
   shellsrv:/home$ jobs
   [1] pid=7 running httpd
   shellsrv:/home$ # desde otro host:  curl http://localhost:8080/
   shellsrv:/home$ ovi .oshrc     # editor modal vim-style
   # i = insert, Esc = normal, hjkl = move,
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
- [Inspiración y agradecimientos](#inspiración-y-agradecimientos)
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
    │   │                   #   block_ata, rtl8139
    │   ├── servers/        #   keyboard_server (poll PS/2 → /dev/input0).
    │   │                   #   console/fs/shell migrados a ring 3 (FASE 10).
    │   ├── fs/             #   VFS + backends (ramfs, sysfs, devfs, binfs,
    │   │                   #         fat, aliasfs)
    │   ├── proc/           #   exec, ELF loader, builtins
    │   ├── lib/            #   helpers freestanding (memcpy, strlcpy, printf)
    │   └── include/        #   headers públicos (osnos_status, osnos_keys, ...)
    │
    ├── lib/libc/           # === libc de usuario (ring 3) ===
    │   ├── crt0.S          #   _start → main → _exit
    │   ├── include/        #   stdio.h, stdlib.h, string.h, unistd.h, ...
    │   └── *.c             #   wrappers de syscalls + stdio + malloc
    │
    ├── elfs/               # programas user-mode (60+ ELFs ring 3)
    │   ├── shell/          #   osh (mini script interpreter)
    │   ├── tools/          #   coreutils completos: ls cat cp mv rm
    │   │                   #     mkdir rmdir touch echo head tail wc
    │   │                   #     grep sort uniq cut tr seq yes tee
    │   │                   #     env pwd which printf date uname
    │   │                   #     basename dirname clear tree banner
    │   │                   #     calc top kill sleep ovi tcc hello
    │   ├── net/            #   tcpclient httpd selectserver udptest
    │   │                   #     echotcp selecttest
    │   ├── tests/          #   libctest ttytest fptest mmaptest
    │   │                   #     pipetest fbtest inputtest kerntest
    │   │                   #     spawntest envtest user_hello (bare)
    │   ├── osn-server/     #   FASE 10 servers ring 3:
    │   │                   #     consrv (console), kbdsrv (keyboard),
    │   │                   #     shellsrv (shell de verdad)
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

### Diagrama de capas (post-FASE 10)

```
┌─────────────────────────────────────────────────────────────┐
│ ring-3 servers (FASE 10):                                   │
│   consrv  — IPC_CONSOLE_WRITE → /dev/fb0                    │
│   kbdsrv  — /dev/input0 → sys_tty_input + IPC_KEY_EVENT     │
│   shellsrv — line editor + dispatch + pipes/redirs/jobs     │
│              (registra SERVER_SHELL, ES EL shell del OS)    │
├─────────────────────────────────────────────────────────────┤
│ ring-3 user tasks: ~60 ELFs en /bin (coreutils + net + ...) │
├─────────────────────────────────────────────────────────────┤
│             lib/libc — mini-libc osnos                      │
│       printf, malloc, fopen, ... → syscall                  │
├─────────────────────────────────────────────────────────────┤
│  Syscall ABI Linux x86_64 + osnos-specific (>= 250):        │
│   SYS_IPC_SEND/RECV (260/261), SERVICE_* (262/263),         │
│   SYS_TTY_INPUT (264), SYS_TASKINFO (265),                  │
│   SYS_SPAWN (266), SYS_SET_FG (267), SYS_RESUME (268)       │
│   int 0x80 + syscall (LSTAR) → syscall_dispatch(frame)      │
├─────────────────────────────────────────────────────────────┤
│ VFS: ramfs (/) │ sysfs (/sys) │ devfs (/dev: null/zero/fb0/ │
│                │                  input0)                   │
│      aliasfs (/home → /sd/home, /bin → /sd/bin)             │
│      binfs (/bin fallback diskless) │ fat16 (/sd, sd.img)   │
├─────────────────────────────────────────────────────────────┤
│  IPC: 1 cola, 64 slots, payload 1024B. ipc_send rewrite     │
│       SID → pid; ring-3 receivers filtran por t->pid        │
├─────────────────────────────────────────────────────────────┤
│  micro/ core: task (per-task fds[16] + fpu_state), pipe,    │
│  scheduler (preempt @ CPL=3 + coop), ipc, gdt/idt/tss,      │
│  pmm/vmm/kmalloc, syscall, uaccess, service                 │
├─────────────────────────────────────────────────────────────┤
│  drivers/: PS/2, framebuffer + font 8x8 + VT100 CSI parser, │
│            PIC, LAPIC, PIT, ATA PIO, RTL8139                │
│  kernel-side servers/: keyboard feeder (poll → ring buffer  │
│            de /dev/input0 — único server kernel-side)       │
├─────────────────────────────────────────────────────────────┤
│                    Limine bootloader                         │
└─────────────────────────────────────────────────────────────┘
```

Diagrama completo + walkthroughs paso a paso ("una tecla viaja por
todo el sistema", "un comando `ls /home` viaja por todo el sistema")
están en [`osnos/ARCH.md`](osnos/ARCH.md).

### Secuencia de boot (`kmain` en `src/kernel/main.c`, post-FASE 10)

1. Validar Limine + framebuffer → `framebuffer_init`.
2. Memoria: `pmm_init → vmm_init → kheap_init`.
3. CPU: `gdt_init → tss_init → idt_init → uaccess_init → syscall_msr_init`.
4. Interrupciones: `pic_init → lapic_init → timer_init` (PIT @ 100 Hz).
   `block_ata_init` corre IDENTIFY contra el primary IDE master; si
   responde, FAT16 se monta en `/sd`.
5. Microkernel: `ipc_init → pipe_init → task_init → reaper_init →
   scheduler_init → syscall_init → ramfs_init → bootstrap_fs`.
6. `bootstrap_fs` monta `/`, `/sys`, `/dev`, `/sd` (si hay FAT). En
   primer boot dumpea los ~60 ELFs embedded a `/sd/bin` (FASE2-1 fix
   permite dir-chain extension), luego monta `/bin → /sd/bin` via
   aliasfs. Diskless: `/bin` queda servido por binfs sintético.
7. Crear el keyboard feeder kernel-side (`task_create("keyboard",
   keyboard_server_tick)`) — solo hace `keyboard_poll` →
   `devfs_input_push`.
8. Spawn de los 3 servers ring 3 vía `proc_execve("/bin/consrv")`,
   `/bin/kbdsrv`, `/bin/shellsrv`. Cada uno se auto-registra contra
   su SERVER_*; kmain también lo pre-registra para evitar la race
   inicial.
9. `keyboard_server_init()` (hardware), `sti`.
10. `scheduler_loop()` — guarda un punto de longjmp y nunca vuelve.

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
| Syscalls Linux x86_64 + osnos-specific (>= 250) | ✅ |
| `copy_from_user` / `copy_to_user` con fault recovery | ✅ |
| VFS + ramfs + sysfs + devfs + binfs + aliasfs + fat16 | ✅ |
| **FASE 10 — Servers en ring 3** (consrv, kbdsrv, shellsrv) | ✅ |
| **Shell ring-3** con line editor + history + flechas + pipes + redir + jobs + `cd ..` + `$VAR` + .oshrc | ✅ |
| **Coreutils** (60+ ELFs: ls cat cp mv rm mkdir touch echo wc head tail grep sort uniq cut tr seq yes tee env pwd which printf date uname basename dirname clear tree banner …) | ✅ |
| Wildcards `*` en ls/cat/rm | ✅ |
| ELF loader (Elf64 ET_EXEC, PT_LOAD, ring 3) | ✅ |
| mini-libc (stdio, stdlib, string, unistd, malloc, env) | ✅ |
| Per-task fd tables (16 fds) + pipe(2) + dup/dup2/fcntl | ✅ |
| Scheduler preemptivo timer-driven (CPL=3, 50 ms quantum) | ✅ |
| FXSAVE/FXRSTOR per-task (FP/SSE multi-task seguro) | ✅ |
| Job control: `&`, `jobs`, `fg`, `bg`, Ctrl+Z, `kill` | ✅ |
| `sys_spawn(2)` con fd inheritance + `osn_spawn` libc | ✅ |
| Driver ATA PIO + FAT16 read/write + dir-chain extension + persistencia | ✅ |
| **/bin disk-resident** via /bin→/sd/bin aliasfs + ROM fallback | ✅ |
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
| Pre-populate sd.img al build (Fase 2 final) | ⏳ |
| `fork` / `exec` real (hoy sólo `osn_spawn`) | ❌ |
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
- **Disk-resident /bin (Fase 1)**: bootstrap dumpea los ~60 ELFs a
  /sd/bin al primer boot. aliasfs /bin → /sd/bin. exec.c prefiere
  disco; embedded ROM es fallback. FAT16 dir-chain extension fix
  (extend_dir_chain) permite >9 entries por subdir.
- **Coreutils completos**: env, wc, pwd, uname, basename, dirname,
  tail, seq, yes, tee, date, printf, grep, sort, uniq, cut, tr,
  banner, which, clear, tree — todos como ELFs.
- **Polish + bug-fixes finales** (post-FASE 10.4): shellsrv `cd` con
  `..` `.` paths relativos (`path_normalize`); `ls /` muestra mount
  points sintéticos (vfs_readdir injection); ovi output buffering
  16 KB + framebuffer chunk safe-split (CSI sequences no se parten
  al medio); task_t.name inline 32 B (sys_taskinfo ya no faulta con
  user-pointer leaks de proc_execve VFS path); ipc_send rewrite
  SID→pid (ring-3 receivers filtran por t->pid); shellsrv `$VAR` /
  `${VAR}` expansion + `export` / `unset` / `setenv` builtins +
  .oshrc auto-load al boot.
- **FASE 8.5 — Networking completo**: driver RTL8139 + Ethernet/ARP +
  IPv4/ICMP + UDP + TCP + DNS + sockets POSIX. `/bin/httpd` sirve
  FAT16 sobre HTTP a Firefox real; `selectserver.c` de Beej verbatim.
- **FASE 9 — Scheduler preempt** + sleep real + Ctrl+C/Z live.
- **FASE 8 — Disco real**: ATA PIO + FAT16 read/write + persistencia.

Las próximas fases grandes:

1. **Fase 2 final disk-resident**: popular sd.img al build (mtools en
   GNUmakefile) + borrar embedded blobs del kernel. Kernel shrink:
   ~5MB → ~500KB.
2. **fork/exec** POSIX real (hoy hay `osn_spawn` con fd inheritance,
   estilo posix_spawn).
3. **FASE 11 — Drivers a ring 3**: PS/2, framebuffer, ATA, RTL8139
   como ELFs en `elfs/osn-driver/`. Requiere IRQ delegation, MMIO
   mapping per-task, port-IO syscall, DMA bouncing.
4. **TUI potente** (FASE 12): mini Norton Commander, viewer, syntax
   highlighting en ovi, multi-pane.
5. **Gráfico** (FASE 13): window server + terminal en ventana + mouse.
6. **SMP** (mucho después).

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
