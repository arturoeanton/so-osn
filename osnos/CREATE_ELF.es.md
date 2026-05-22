# Cómo crear un ELF para osnos

Una guía paso a paso para escribir un programa C, compilarlo en Linux
como ELF64, embedderlo en el kernel y correrlo en ring 3 sobre osnos.

> **Nota de organización (post-FASE 10)**: este doc fue escrito cuando
> los ELFs vivían en `tests/`. Ese directorio se renombró a `elfs/` con
> subcategorías:
>   - `elfs/shell/` — osh (script interpreter)
>   - `elfs/tools/` — coreutils ELFs (~40+ comandos)
>   - `elfs/net/` — sockets (tcpclient, httpd, ...)
>   - `elfs/tests/` — libc + ABI smoke tests (kerntest, pipetest,
>     spawntest, libctest, fptest, mmaptest, user_hello bare)
>   - `elfs/osn-server/` — servers ring-3 de FASE 10 (consrv, kbdsrv,
>     shellsrv).
>
> El linker script compartido es `elfs/libc.lds`. Donde el tutorial dice
> "`tests/`", leer "`elfs/<categoría>/`". El resto del flujo es idéntico.

Hay **dos formas** de escribir un ELF en osnos:

1. **Libc-linked (lo normal)** — escribís `int main(int argc, char **argv)`
   y linkeás contra `libosnos_c.a`. Tenés `printf`, `malloc`, `fopen`,
   `qsort`, `setjmp`, todo lo del Tier 1 de la libc. Es lo que usan los
   ~40 ELFs de `elfs/tools/` y `elfs/net/`.
2. **Bare ELF (edge case)** — escribís tu propio `_start`, ningún
   linkeo, syscall wrappers a mano. Se usa para casos especiales:
   binarios de prueba del loader, tools que prueban una syscall sin
   indirección de libc. El único ejemplo vivo es
   `elfs/tests/user_hello.c`.

Esta guía lleva la libc-linked por defecto. La bare está al final.

> Compañero de este tutorial: `CREATE_BUILTINS.es.md` (builtins
> kernel-mode que también viven en `/bin/`). La diferencia es que un
> builtin corre en ring 0 reusando estructuras del kernel, mientras
> que un ELF corre en **ring 3** dentro de su propio address space.

---

## Qué tenés a tu disposición

Cuando el kernel carga tu ELF libc-linked, vivís en ring 3 con esto:

- **Tu propio PML4** (low-half tuyo, high-half kernel compartido pero
  inaccesible desde CPL=3).
- **Tu binario mapeado** según los `PT_LOAD` que emite el linker
  script (`elfs/libc.lds`). El layout estándar es `.text`+`.rodata`
  R+X en `0x400000`, `.data`+`.bss` R+W en `0x401000`.
- **Stack** de una página en `0x7FFFE000-0x7FFFF000`, RSP arrancando
  en `0x7FFFF000`.
- **Heap** vía `brk()` syscall — `malloc()` lo usa por debajo. Empieza
  en `USER_HEAP_BASE = 0x10000000` y crece hacia arriba a medida que
  pedís páginas.
- **mmap anónimo** (`PROT_*`, `MAP_ANONYMOUS`) para regiones grandes
  o aisladas. `mmap_next` per-task lleva el cursor.
- **argc / argv / envp** populated por el kernel (`build_argv_block`
  en `src/proc/exec.c` arma el bloque System V x86_64 al top del
  stack). `envp` ahora se hereda real del proceso padre — shellsrv
  pasa `environ` vía `pack_envp` → `sys_spawn(envp_flat)`.
- **`crt0.S`** que el linker pone como primer .text: hace
  `main(argc, argv, envp)` y entrega el return a `_exit`.
- **Toda la libc Tier 1**: stdio (FILE\* con fopen, fread, fwrite,
  fgets, printf, fprintf), stdlib (malloc/free, qsort, bsearch,
  strtoll, atexit), string (strdup, strstr, strtok_r, strerror,
  strcasecmp), setjmp/longjmp, time (struct timespec, nanosleep),
  errno, inttypes (PRId64 etc.), endian.h, arpa/inet.h (htons,
  inet_pton, etc.).
- **Syscall ABI Linux x86_64** completa abajo de la libc — podés
  bypassear y llamar `syscall` directo si querés.

Lo que tenés **a partir de FASE 10**:

- **`osn_spawn`** (libc helper sobre `SYS_SPAWN`): podés spawnear
  otro ELF desde tu programa con fd inheritance (stdin/stdout via
  los fds del caller). Equivalente a posix_spawn. Ver `osnos_ipc.h`.
- **`pipe(2)`**, **`dup`/`dup2`**, **`fcntl`** (F_GETFL/SETFL,
  F_DUPFD): per-task fd tables ya soportan todo el pipeline POSIX.
- **`mmap`/`munmap`** anónimo (PROT_*/MAP_ANONYMOUS).
- **FPU/SSE** ya tiene FXSAVE/FXRSTOR per-task. Si querés usar
  float en tu ELF, sacá `-mno-80387 -mno-sse` del Makefile de tu
  binario.
- **IPC entre tasks** vía SYS_IPC_SEND/RECV + SYS_SERVICE_REGISTER/
  LOOKUP. Cualquier ELF puede registrarse como un SERVER_*
  (definir nuevo SID en `osnos_ipc_abi.h`).

Lo que tenés **a partir de Fase 2 disk-resident** (la sesión que
cerró ABI POSIX core):

- **`fork(2)` real**: SYS_FORK=57. Deep-copy del pml4 del parent (no
  COW yet — full page copy), clona la fd table con pipe refcount
  bumps, snapshot del syscall context → child resume en la instr
  siguiente con `rax=0`. Tanto el clásico `if (fork()==0) ...` como
  `fork() + wait_via_taskinfo` funcionan. Ver `elfs/tests/forktest.c`.
- **`execve(2)` real**: SYS_EXECVE=59. Reemplaza la imagen user-mode
  del task actual, mismo pid + fds + cwd. libc tiene `execve` /
  `execv` / `execvp` listos (PATH search). Combinable con fork
  para el patrón clásico fork+exec.
- **Coexisten con `osn_spawn`** (SYS_SPAWN=266) que es fork+exec
  atómico estilo posix_spawn con fd inheritance MOVE-semantics.

Lo que **no** tenés todavía:

- **Signal handlers**. `kill()` existe pero como exit forzado, no
  hay `sigaction`. `sigsetjmp`/`siglongjmp` aliasean setjmp/longjmp
  pero no salvan signal mask. Ctrl+C/Ctrl+Z se entregan vía
  kill_pending / stop_pending del task struct, sin handlers user-mode.
- **Threads**. Un task = un thread.
- **TLS / FS register**. Cero soporte.
- **Loader dinámico**. Sólo `ET_EXEC` estático, nunca `ET_DYN`/PIE.

---

## Pre-requisitos en tu Linux

```bash
# Compilador y linker (clang trae lld)
sudo dnf install -y clang lld    # Fedora / RHEL
# sudo apt install -y clang lld   # Debian / Ubuntu

# objcopy del binutils para embedderlo
which objcopy   # binutils, ya lo tenés casi seguro

# make
which make
```

El kernel mismo de osnos se compila con clang+lld, así que si pudiste
hacer `make run-bios` ya tenés todo lo necesario.

---

## Ejemplo 1 — `hello` libc-linked

Esto es el patrón que usan la mayoría de los tools.

### `tests/hello.c`

```c
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("hello, world\n");
    return 0;
}
```

Eso es todo. `crt0.S` provee `_start`, llama a `main`, pasa el return
a `_exit`. `printf` está implementado en `lib/libc/stdio.c` y baja a
`sys_write` por debajo.

### No hace falta `.lds` propio

Todos los ELFs libc-linked comparten **`tests/libc.lds`**. El Makefile
aplica la regla `$(BUILD)/tests/%.elf` automáticamente para cualquier
`tests/*.c` que pongas en `USER_ELF_LIBC_SRCS`. No tenés que escribir
ni copiar un linker script.

Si querés ver el linker script:

```bash
cat tests/libc.lds
```

Layout: `.text + .rodata` R+X en `0x400000`, `.data + .bss` R+W en
`0x401000`. Una página por sección.

---

## La syscall ABI (referencia)

Casi siempre vas a llamar libc, pero si necesitás algo crudo:

Cargás `rax` con el número de syscall, los argumentos en
`rdi, rsi, rdx, r10, r8, r9` (R10, **no RCX**, igual que Linux x86_64),
y ejecutás `syscall`. El return queda en `rax`:

- `>= 0` → resultado válido (fd, byte count, pid, etc.)
- `<  0` → `-errno` (`-2` = ENOENT, `-9` = EBADF, etc.)

Los registros `rcx` y `r11` se clobbean (CPU los usa para guardar
user RIP/RFLAGS). El stub del kernel los preserva al volver, pero
**el inline asm tiene que listarlos como clobbered**.

Para hacerlo desde tu .c sin libc: `#include "syscall.h"` desde
`lib/libc/syscall.h` te da `osnos_syscall0..4` listos.

Números de syscall que existen hoy (ver `src/micro/syscall.h`):

**Linux ABI compatible** (mismos números que Linux x86_64):

| Nº  | Nombre        | Args                                              |
|----:|---------------|---------------------------------------------------|
|   0 | `read`        | `(int fd, void *buf, size_t n)`                   |
|   1 | `write`       | `(int fd, const void *buf, size_t n)`             |
|   2 | `open`        | `(const char *path, int flags, mode_t mode)`      |
|   3 | `close`       | `(int fd)`                                        |
|   5 | `fstat`       | `(int fd, struct stat *out)`                      |
|   8 | `lseek`       | `(int fd, off_t off, int whence)`                 |
|   9 | `mmap`        | `(void *addr, size_t len, int prot, int flags, int fd, off_t)` |
|  11 | `munmap`      | `(void *addr, size_t len)`                        |
|  12 | `brk`         | `(uintptr_t new_brk)` — base de `malloc`          |
|  22 | `pipe`        | `(int pipefd[2])`                                 |
|  32 | `dup`         | `(int oldfd)`                                     |
|  33 | `dup2`        | `(int oldfd, int newfd)`                          |
|  35 | `nanosleep`   | `(const struct timespec *req, struct timespec *)` |
|  39 | `getpid`      | `(void)` → pid                                    |
|  41 | `socket`      | `(int domain, int type, int proto)`               |
|  42 | `connect`     | `(int fd, const struct sockaddr*, socklen_t)`     |
|  43 | `accept`      | `(int fd, struct sockaddr*, socklen_t*)`          |
|  44 | `sendto`      | `(int fd, const void*, size_t, int, ...)`         |
|  45 | `recvfrom`    | `(int fd, void*, size_t, int, ...)`               |
|  47 | `recvmsg`     | `(int fd, struct msghdr*, int)`                   |
|  48 | `shutdown`    | `(int fd, int how)`                               |
|  49 | `bind`        | `(int fd, const struct sockaddr*, socklen_t)`     |
|  50 | `listen`      | `(int fd, int backlog)`                           |
|  55 | `getsockopt`  | `(int fd, int level, int opt, ...)`               |
|  57 | `fork`        | `(void)` → child pid (parent) / 0 (child) / -errno |
|  59 | `execve`      | `(const char *path, argv[], envp[])` — never returns on success |
|  60 | `exit`        | `(int code)`                                      |
|  62 | `kill`        | `(pid_t pid, int sig)`                            |
|  72 | `fcntl`       | `(int fd, int cmd, ...)` (F_GETFL/SETFL/DUPFD)    |
|  79 | `getcwd`      | `(char *buf, size_t n)`                           |
|  80 | `chdir`       | `(const char *path)`                              |
|  82 | `rename`      | `(const char *old, const char *new)`              |
|  83 | `mkdir`       | `(const char *path, mode_t mode)`                 |
|  84 | `rmdir`       | `(const char *path)`                              |
|  87 | `unlink`      | `(const char *path)`                              |
|  96 | `gettimeofday`| `(struct timeval*, struct timezone*)`             |
|  16 | `ioctl`       | `(int fd, unsigned long req, ...)` (termios+TIOC) |
| 201 | `time`        | `(time_t*)` *(actually `time` in Linux too)*      |
| 217 | `getdents64`  | `(int fd, void *buf, size_t n)`                   |

**osnos-specific** (≥ 250, no chocan con Linux):

| Nº  | Nombre               | Args                                       |
|----:|----------------------|--------------------------------------------|
| 260 | `ipc_send`           | `(const ipc_msg_t *)`                      |
| 261 | `ipc_recv`           | `(ipc_msg_t *out, int blocking)`           |
| 262 | `service_register`   | `(int sid)`                                |
| 263 | `service_lookup`     | `(int sid)` → pid o 0                      |
| 264 | `tty_input`          | `(int c)` *(solo el SERVER_KEYBOARD task)* |
| 265 | `taskinfo`           | `(size_t idx, osnos_taskinfo_t *out)`      |
| 266 | `spawn`              | `(path, args, envp_flat, stdin_fd, stdout_fd)` |
| 267 | `set_fg`             | `(pid_t pid)` *(0 = clear)*                |
| 268 | `resume`             | `(pid_t pid)` *(SIGCONT-equivalent)*       |

Cualquier syscall fuera de esa lista devuelve `-ENOSYS` (-38).
Los wrappers libc están en `lib/libc/include/osnos_ipc.h` (ipc + spawn),
`unistd.h` (estándar) y `sys/socket.h` (red).

---

## Ejemplo 2 — `wc` con argv + file I/O + formatos

Algo más interesante que ejercita argv parsing, FILE* y printf.

### `tests/wc.c`

```c
/*
 * tests/wc.c — wc(1) chiquito: cuenta líneas, palabras y bytes.
 *
 *   wc FILE         → "N L W B FILE"
 *   wc              → lee stdin
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { long lines, words, bytes; } counts_t;

static void count_stream(FILE *f, counts_t *c) {
    int in_word = 0;
    for (int ch; (ch = fgetc(f)) != EOF; ) {
        c->bytes++;
        if (ch == '\n') c->lines++;
        if (isspace(ch)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            c->words++;
        }
    }
}

int main(int argc, char **argv) {
    counts_t c = { 0, 0, 0 };
    const char *label = "";
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "wc: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        count_stream(f, &c);
        fclose(f);
        label = argv[1];
    } else {
        count_stream(stdin, &c);
    }
    printf("%ld %ld %ld %s\n", c.lines, c.words, c.bytes, label);
    return 0;
}
```

Notá que `errno` viene incluído desde `<errno.h>` (transitive en `stdio.h`).
Si te molesta el warning de `errno` no declarado:

```c
#include <errno.h>
```

---

## Embedderlo en el kernel

Cada ELF se compila junto al kernel y queda **embebido como bytes**
dentro de la imagen. En el primer boot, `bootstrap_fs` los dumpea a
`/sd/bin/` (FAT16) y aliasa `/bin` → `/sd/bin`. Si borrás un binario
del disco, el embedded sigue sirviendo como ROM fallback. Tres pasos:

### Paso 1 — Listar el `.c` en el Makefile

Edit `osnos/GNUmakefile`. Busca `USER_ELF_LIBC_SRCS`:

```make
USER_ELF_LIBC_SRCS := elfs/tests/hello_libc.c \
                      elfs/tools/hello.c \
                      ...
                      elfs/tools/top.c \
                      elfs/tools/which.c
```

Y añadí tu archivo:

```make
USER_ELF_LIBC_SRCS := ... \
                      elfs/tools/which.c \
                      elfs/tools/wc.c
```

La regla `$(BUILD)/elfs/%.elf` y `$(BUILD)/elfs/%.elf.o` se aplica
automáticamente, usando `elfs/libc.lds`.

### Paso 2 — Declarar el símbolo en `builtin.c`

`objcopy` emite tres símbolos a partir del nombre del archivo:

- `_binary_wc_elf_start` — primer byte
- `_binary_wc_elf_end`   — un byte después del último
- `_binary_wc_elf_size`  — tamaño (raramente útil)

Abrí `src/proc/builtin.c` y agregá:

```c
DECLARE_ELF(wc);
```

(El macro `DECLARE_ELF(name)` expande a los dos `extern` declarations.)

### Paso 3 — Registrar el builtin

En el array `builtins[]`, agregá:

```c
static const builtin_t builtins[] = {
    /* ... */
    ELF(wc, "wc(1) — cuenta líneas/palabras/bytes"),
};
```

`ELF(name, desc)` expande a `USERELF("name", _binary_name_elf_start,
_binary_name_elf_end, desc)`.

### Build y run

Desde el directorio padre del repo:

```bash
./build_and_run.sh
```

Adentro de shellsrv:

```
shellsrv:/$ echo "hola mundo cruel" > /home/sample.txt
shellsrv:/$ wc /home/sample.txt
1 3 17 /home/sample.txt
shellsrv:/$
```

End-to-end: tu C → ELF64 → embed (objcopy) → primer boot lo dumpea a
/sd/bin/<name> → exec.c lo lee via VFS (FAT) → elf_load + mapeo de
páginas → iretq a ring 3 → libc → syscall → IPC_CONSOLE_WRITE →
consrv ring-3 → /dev/fb0 → framebuffer.

---

## Bare ELF — sin libc

Casos donde tiene sentido:

- Estás probando algo del syscall path en aislamiento (sin que `crt0`,
  `malloc` o `printf` te metan ruido).
- Tu programa es asm puro o C super simple y querés un binario chico
  (los bare ELFs pesan ~4 KiB vs ~20 KiB de los libc-linked).
- Estás escribiendo un test del loader o de la ABI.

El único ejemplo vivo es `elfs/tests/user_hello.c` +
`elfs/tests/user_hello.lds`. Si necesitás otro, esa es la plantilla.

Diferencias clave vs libc-linked:

| Aspecto              | libc-linked              | bare                          |
|----------------------|--------------------------|-------------------------------|
| Entry point          | `int main(int, char**)`  | `void _start(void)`           |
| `_start`             | provisto por `crt0.S`    | escribís el tuyo              |
| Linker script        | `elfs/libc.lds`          | `elfs/<categoría>/<n>.lds`    |
| Makefile var         | `USER_ELF_LIBC_SRCS`     | `USER_ELF_SRCS`               |
| stdlib funcs         | sí (`printf` etc.)       | escribís wrappers a mano      |
| `argc`/`argv`        | parseados por crt0       | el bloque está en el stack    |
|                      |                          | pero lo tenés que leer vos    |
| `errno`              | sí                       | mirás `rax < 0`               |
| Tamaño binario       | ~20 KiB                  | ~1-4 KiB                      |

Para escribir un bare ELF: copiá `elfs/tests/user_hello.c` y
`elfs/tests/user_hello.lds`, cambiale los nombres, agregalo a
`USER_ELF_SRCS` (no a `USER_ELF_LIBC_SRCS`). El símbolo en
`builtin.c` se declara igual con `DECLARE_ELF(name)`.

El linker script bare es más libre — controlás todos los segmentos.
Si tu programa necesita escribir a globals, separá en dos PT_LOAD
(uno R+X, otro R+W):

```ld
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS {
    . = 0x400000;
    .text : { *(.text .text.*) *(.rodata .rodata.*) } :text

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .bss  : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
```

El loader respeta `PF_W` por PT_LOAD: el segundo se mapea con `PTE_W`.

---

## Cuando algo falla — debugging

### "exec failed" inmediato

Causas comunes:
- Nombre mal en el array `builtins[]` (no matchea con `/bin/<nombre>`).
- Símbolo `_binary_*_start/end` no declarado o el path en
  `USER_ELF_LIBC_SRCS` no matchea el sufijo del símbolo.
- Te olvidaste de poner el `.c` en el Makefile.

Verificá con:

```bash
nm build/kernel | grep _binary_
```

Tendrían que aparecer dos líneas (`_start` y `_end`) por cada ELF.

### "*** EXCEPTION 13 General protection fault from USER mode"

Tu ELF cargó pero alguna instrucción es inválida. Causas:
- Globals sin PF_W en el linker script — **no aplica a libc-linked**
  porque `libc.lds` ya da el segundo PT_LOAD R+W. Para bare ELFs sí.
- Inline asm sin clobber `"memory"`/`"rcx"`/`"r11"` y clang movió un
  load a través del syscall.
- Stack overflow profundo (raro con la libc, más probable con
  recursión).

Inspeccioná con:

```bash
objdump -d build/elfs/<categoría>/<nombre>.elf | less
readelf -l build/elfs/<categoría>/<nombre>.elf
```

El RIP impreso en pantalla te dice qué instrucción explotó. Restale
`0x400000` (o lo que sea tu base) para encontrar el offset.

### "*** ring-3 task killed: Page fault cr2=..."

Mismo flujo pero #PF. Casi siempre:
- Deref de un puntero NULL o no inicializado (CR2 = 0 o número raro).
- Stack overflow (CR2 cerca de `0x7FFFE000` o por debajo — agotaste
  la única página de stack).
- Acceder a heap antes de `malloc` (el rango `0x10000000+` está
  mapeado on-demand cuando `brk` crece; antes de eso es unmapped).

Si necesitás más stack, no se expande automático. Mantenelo chico o
usá `mmap` para tu propia área grande de scratch.

### Tu ELF compila pero no anda

```bash
file build/elfs/<categoría>/<nombre>.elf
# debería decir "ELF 64-bit LSB executable, x86-64, ... statically linked"

readelf -h build/elfs/<categoría>/<nombre>.elf | grep -E "Type|Machine|Entry"
# Type:     EXEC
# Machine:  Advanced Micro Devices X86-64
# Entry:    0x400000   (libc-linked siempre arranca acá vía crt0)
```

Si `Type` dice `DYN`, te falta `-no-pie` o `-fno-pie -fno-PIC`. El
loader rechaza `ET_DYN`.

### "Loop iteration cap reached" o tu programa se cuelga

Si tu task es CPU-bound infinito, el scheduler la preemptea cada
50ms desde FASE 9.3b, así el resto del sistema sobrevive. Para
matarla:

- **Si es foreground**: `Ctrl+C` en el shell → kill en ≤10ms.
- **Si es background (`&`)**: `kill PID` (PID lo ves en `ps`).

---

## Stack + data segments — cosas para tener presente

- **Stack**: una página de 4 KiB. RSP arranca en `0x7FFFF000`;
  acceso ≤ `0x7FFFE000` faultea. No crece automáticamente.
- **Heap**: existe via `malloc`/`free` (libc) o `brk` directo.
  Empieza en `0x10000000` y crece con cada `sbrk`. Free list libc
  es first-fit sin split/merge — sirve para casos chicos.
- **`.bss`**: si está dentro de un PT_LOAD con `p_memsz > p_filesz`,
  el loader zero-fillea. Variables globales no inicializadas
  empiezan en 0.
- **`.data`**: vive en el blob; se copia tal cual al mapearse.

---

## Toolchain reference — qué hace cada flag

`USER_CFLAGS` en el Makefile:

| Flag                              | Por qué                                              |
|-----------------------------------|------------------------------------------------------|
| `-nostdinc`                       | sin headers del sistema host                         |
| `-ffreestanding`                  | no asumas la libc del host                           |
| `-fno-stack-protector`            | sin canaries                                         |
| `-fno-PIC -fno-pie`               | ET_EXEC a address fijo, no PIE                       |
| `-target x86_64-unknown-none-elf` | freestanding x86_64                                  |
| `-mno-red-zone`                   | no usar la zona roja                                 |
| `-mno-{80387,mmx,sse,sse2}`       | no usar FPU/SIMD (no salvamos esos registros)        |
| `-Werror`                         | porque sí                                            |
| `-isystem .../freestnd-c-hdrs/include` | headers freestanding (stdint, stddef, etc.)     |
| `-I lib/libc/include`             | headers libc osnos (stdio, stdlib, etc.)             |

`USER_LDFLAGS`:

| Flag                  | Por qué                                       |
|-----------------------|-----------------------------------------------|
| `-nostdlib`           | sin crt0/libc del host                        |
| `-static -no-pie`     | sin loader dinámico, sin PIE                  |
| `-z noexecstack`      | marcar stack como no-ejecutable (defensiva)   |
| `-T elfs/libc.lds`    | linker script común (libc-linked)             |
| `crt0.S.o + main.o + libosnos_c.a` | orden de objetos para linkeo             |

---

## Cosas que YA podés usar (FASE 8 / 8.5 / 9 / 10 + Fase 2 cerradas)

- **Disco real**: `fopen` / `fread` / `fwrite` / `open` / `read` /
  `write` sobre `/home/*` o `/sd/*` persisten entre reboots (FAT16).
- **`fork(2)` + `execve(2)`** (ABI POSIX core completo): patrón clásico
  ```c
  pid_t pid = fork();
  if (pid == 0) {
      char *argv[] = {"ls", "/home", NULL};
      execve("/bin/ls", argv, environ);
      _exit(127);          // execve solo retorna en error
  } else if (pid > 0) {
      // parent: esperar al child via taskinfo polling
      // (wait/waitpid POSIX todavía no — ver forktest.c)
  }
  ```
- **`osn_spawn`** (SYS_SPAWN=266): si querés fork+exec atómico (más
  barato que fork+execve separados), con fd inheritance MOVE-semantics.
  Útil cuando shellsrv arma pipes/redirects para los children.
- **Pipelines Unix**: `cat archivo | grep foo | sort | head` —
  shellsrv soporta pipes multi-stage y redirects `< > >>`. Desde C,
  `pipe(2)` + `dup2(2)` + `fork(2)` + `execve(2)` arman cualquier
  pipeline (también `osn_spawn` si preferís el atómico).
- **Operadores de shell**: `;` `&&` `||` con tracking de `$?`,
  glob `*` (`ls *.txt`), background `&`, redirects `< > >>`,
  `$VAR` / `${VAR}` expansion + `$?`.
- **Sockets POSIX completos**: TCP cliente/servidor, UDP, `select`,
  DNS via `getaddrinfo`. Ver `elfs/net/httpd.c` y `selectserver.c`.
- **Background jobs**: `&`, `jobs`, `fg`, `bg`, Ctrl+Z (en shellsrv).
- **Preempción real**: scheduler timer-driven cada 50ms en CPL=3 —
  tu programa puede loopear sin colgar el sistema.
- **FXSAVE/FXRSTOR per-task**: doubles/floats funcionan; saca
  `-mno-sse` del Makefile de tu binario si los querés.
- **mmap anónimo + brk/sbrk**: heap grande o regiones aisladas.
- **IPC con otros ring-3**: SYS_IPC_SEND/RECV permite escribir
  servers como `consrv` / `kbdsrv` / `shellsrv` desde ring 3.

### Ejemplo: fork(2) + execve(2) classic POSIX

```c
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

int main(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: errno=%d\n", errno);
        return 1;
    }
    if (pid == 0) {
        /* CHILD: same memory image, same fds, same cwd, rax=0.
         * Replace ourselves with /bin/echo. */
        char *argv[] = { "echo", "hola desde el child", NULL };
        execve("/bin/echo", argv, environ);
        /* Only reached on failure. */
        fprintf(stderr, "execve failed: errno=%d\n", errno);
        _exit(127);
    }
    /* PARENT: pid is the child's pid. wait() POSIX no existe todavía;
     * polleá via sys_taskinfo (#265) — ver forktest.c, función
     * `wait_child`. */
    printf("forked child pid=%d\n", (int)pid);
    return 0;
}
```

Output esperado:
```
forked child pid=N
hola desde el child
```

---

## Checklist rápida para crear un ELF nuevo

### libc-linked (default):

```
[ ] elfs/<categoría>/<nombre>.c    — código C con int main(argc, argv)
[ ] GNUmakefile                    — agregar a USER_ELF_LIBC_SRCS
[ ] src/proc/builtin.c             — DECLARE_ELF(<nombre>);
[ ] src/proc/builtin.c             — ELF(<nombre>, "desc"),
[ ] ./build_and_run.sh             — build + boot
[ ] shellsrv:/$ <nombre>           — verificar que corre
```

### bare ELF (raro):

```
[ ] elfs/tests/<nombre>.c          — _start handwritten, sin libc
[ ] elfs/tests/<nombre>.lds        — linker script propio
[ ] GNUmakefile                    — agregar a USER_ELF_SRCS
[ ] GNUmakefile                    — pattern rule específico (mirá
                                     la regla de user_hello.elf)
[ ] src/proc/builtin.c             — DECLARE_ELF(<nombre>);
[ ] src/proc/builtin.c             — USERELF(...) directo
[ ] ./build_and_run.sh
[ ] shellsrv:/$ <nombre>
```

Cuando termines, `cat /bin/<nombre>` te muestra los bytes del ELF,
`ls /bin/` lo lista, y `<nombre>` o `/bin/<nombre>` lo ejecuta
(shellsrv hace PATH search via `$PATH`).

Listo.
