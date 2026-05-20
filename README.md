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
    │   ├── drivers/        #   framebuffer, teclado PS/2, PIC, LAPIC, timer
    │   ├── servers/        #   shell, console, keyboard, fs (en ring 0 hoy)
    │   ├── fs/             #   VFS + backends (ramfs, sysfs, devfs, binfs)
    │   ├── proc/           #   exec, ELF loader, builtins
    │   ├── lib/            #   helpers freestanding (memcpy, strlcpy, printf)
    │   └── include/        #   headers públicos (osnos_status, osnos_keys, ...)
    │
    ├── lib/libc/           # === libc de usuario (ring 3) ===
    │   ├── crt0.S          #   _start → main → _exit
    │   ├── include/        #   stdio.h, stdlib.h, string.h, unistd.h, ...
    │   └── *.c             #   wrappers de syscalls + stdio + malloc
    │
    ├── tests/              # programas user-mode embebidos en el kernel
    │                       # cada tests/<name>.c + <name>.lds → /bin/<name>
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

OSnOS es un microkernel cooperativo. Todo corre en ring 0 todavía,
pero los subsistemas están separados como si fueran servidores
userspace y se comunican exclusivamente por IPC (excepto los drivers
de bajo nivel). Cuando lleguen ring 3 + preempción "de verdad" (fase
10 del roadmap), mover servers a userspace debería ser mecánico.

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
│  IPC: 1 cola, 64 slots, payload 1024B, blocking + wakeup    │
├─────────────────────────────────────────────────────────────┤
│  micro/ core: task, scheduler (cooperativo), ipc, gdt/idt/  │
│  tss, pmm/vmm/kmalloc, syscall, uaccess (fault-recoverable) │
├─────────────────────────────────────────────────────────────┤
│  drivers/: PS/2, framebuffer + font 8x8, PIC, LAPIC, PIT    │
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
5. Microkernel: `ipc_init → task_init → reaper_init → scheduler_init →
   syscall_init → ramfs_init → bootstrap_fs (monta `/`, `/sys`, `/dev`,
   `/bin`)`.
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
| Scheduler preemptivo, timer-driven | ⏳ (fase 9 del roadmap) |
| Drivers de disco real (ATA / VirtIO) | ⏳ |
| Networking | ❌ |
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

Crear `tests/miprog.c`:

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

Las próximas fases grandes:

1. **Scheduler preemptivo** real con quantum de timer, no cooperativo.
2. **Driver de disco** (ATA PIO primero, después VirtIO) + FAT16/FAT32
   sobre `sd.img`.
3. **Mover servers a ring 3** (shell, fs, console, keyboard como
   procesos de usuario).
4. **fork/exec** real (hoy `exec` reemplaza la task actual, no hay
   `fork`).
5. **Stack de red** (eventualmente).
6. **SMP** (mucho después).

Detalle en `osnos/PLAN.md` y `osnos/STATUS.md`.

---

## Licencia

Pendiente de definir. Por ahora considerá el código como "todos los
derechos reservados, contactar al autor". Las dependencias bajo
`osnos/kernel-deps/` tienen sus propias licencias (mayormente 0BSD /
BSD), respetadas en sus respectivos archivos `LICENSE`.
