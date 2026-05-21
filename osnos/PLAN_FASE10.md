# PLAN — FASE 10 · Servers a ring 3

> **Goal**: sacar `console_server`, `keyboard_server`, `fs_server` y
> `shell_server` del ring 0 cooperativo y correrlos como ELFs ring-3
> bajo `elfs/osn-server/`. El kernel queda con **drivers + IPC + VFS
> core**; toda lógica de servicio vive en user-mode con syscalls bien
> definidas.
>
> **Scope explícito**: FASE 10 toca SOLO los servers. Los drivers
> (PS/2 keyboard hardware poll, framebuffer pixel push, ATA PIO,
> RTL8139, PIT, LAPIC, PIC) se quedan en el kernel. Sacarlos a
> ring 3 es **FASE 11** (mucho más complejo: requiere IRQ
> delegation, MMIO mapping per-task, port I/O delegation, IRQ-driven
> IPC). NO mezclar las dos fases en una sesión.
>
> Este es el último gran refactor antes de poder llamar a OSnOS un
> microkernel "de verdad" (a nivel de servicios; a nivel de drivers
> habrá que esperar a FASE 11). Lo más importante es que cada
> sub-fase sea **verificable de punta a punta** antes de pasar a la
> siguiente. Si rompemos algo, sabemos exactamente cuándo.

---

## Tabla de contenidos

1. [Estado actual](#estado-actual-baseline)
2. [Diagrama before / after](#diagrama-before--after)
3. [Inventario de symbols a tocar](#inventario-de-symbols-a-tocar)
4. [Sub-fase 10.0 — Pre-reqs detallados](#sub-fase-100--pre-reqs-detallados)
   - 10.0.a per-task fd tables
   - 10.0.b pipe(2) syscall
   - 10.0.c /dev/fb0 + /dev/input0
   - 10.0.d **headers ABI compartidos**
   - 10.0.e **cmd_test → /bin/kerntest ELF**
5. [Sub-fase 10.1 — console_server a ring 3](#sub-fase-101--console_server-a-ring-3)
6. [Sub-fase 10.2 — keyboard_server a ring 3](#sub-fase-102--keyboard_server-a-ring-3)
7. [🛑 Checkpoint — consolidar 10.1 + 10.2](#-checkpoint--consolidar-101--102-antes-de-seguir)
8. [Sub-fase 10.3 — fs_server: eliminación](#sub-fase-103--fs_server-eliminación)
9. [Sub-fase 10.4 — shell_server a ring 3](#sub-fase-104--shell_server-a-ring-3)
10. [Sub-fase 10.5 — Cleanup + docs](#sub-fase-105--cleanup--docs)
11. [ABI de IPC user-mode](#abi-de-ipc-user-mode)
12. [Riesgos + mitigaciones](#riesgos--mitigaciones)
13. [Test plan integral](#test-plan-integral)
14. [Helpers + snippets de partida](#helpers--snippets-de-partida)
15. [Checklist al retomar](#checklist-al-retomar)
16. [Después de FASE 10 (incluye FASE 11 drivers)](#después-de-fase-10)

---

## Estado actual (baseline)

Antes de tocar nada al volver, **correr** y **anotar** los números:

```sh
make clean && make           # debe ser cero warnings + cero errors
./build_and_run.sh           # bootea
# Dentro del shell:
test                         # esperá 640/640 pass aprox.
test                         # 2da vez — idempotente
libctest                     # ~110 pass
mmaptest                     # 12 pass
fptest 200                   # imprime a/b/c
sleep 5                      # Ctrl+Z para job control
jobs / fg / bg               # verificar que andan
```

Si algún número difiere, **no avanzar** — perdiste regression entre
sesiones. Revisar `git status` y `git diff` antes.

### Inventario de servers actuales

| Server | Archivo | LOC | Dependencias internas (no-IPC) |
|---|---|---|---|
| `console_server` | `src/servers/console_server.c` | 48 | `framebuffer_*` |
| `keyboard_server` | `src/servers/keyboard_server.c` | 52 | `keyboard_*`, `tty_input` |
| `fs_server` | `src/servers/fs_server.c` | 240 | `vfs_*` (todas) |
| `shell_server` | `src/servers/shell_server.c` | 3775 | `vfs_*`, `tty_*`, `fb_*`, `task_*`, `fd_*`, `pipe_*`, `proc_*`, `socket_*`, `kheap_*`, `timer_*`, `kmalloc`, todo |

`shell_server` es el caso grave — toca **media docena** de capas
internas del kernel. Hay que mapear cada una a un syscall (o decidir
si el server ring-3 simplemente NO necesita esa capacidad).

### Estado a la fecha (lo que ya tenemos a favor)

- ✅ FXSAVE/FXRSTOR per-task — multi-task FP seguro.
- ✅ mmap anónimo — los servers ring-3 pueden alocar buffers grandes.
- ✅ Pipes + redirects multi-stage.
- ✅ Job control (Ctrl+Z, fg, bg, jobs).
- ✅ Per-task cwd via `getcwd`/`chdir`.
- ✅ Env passing via crt0.
- ✅ libc Tier 1 + math + ctype + signal stubs.

Falta (FASE 10 = scope de este plan):
- ❌ Per-task fd tables.
- ❌ Pipe(2) syscall expuesto a userland.
- ❌ Dispositivos en `/dev` que abstraigan framebuffer + keyboard.
- ❌ Syscalls IPC para tasks ring-3.
- ❌ Headers ABI compartidos kernel ↔ ring-3 servers.
- ❌ ELF separado para `cmd_test` (hoy live en shell, lo movemos
  ANTES de tocar el shell).

Lo que NO entra en FASE 10 (futuro FASE 11 = drivers a ring 3):
- 🚫 Driver PS/2 (keyboard hardware poll + scancode decode).
- 🚫 Driver framebuffer (pixel push, font glyphs, CSI parser).
- 🚫 Driver ATA PIO + FAT16 backend.
- 🚫 Driver RTL8139 + IP/TCP stack.
- 🚫 Driver PIT / LAPIC / PIC.
- 🚫 IRQ delegation, MMIO mapping, port-IO via syscall.

Esos siguen kernel-side post-FASE-10. Los servers ring-3 los
usan vía abstracciones de fd (`/dev/fb0`, `/dev/input0`) o via
syscalls genéricos (open/read/write/ioctl) — el server no toca
hardware directamente.

---

## Diagrama before / after

### Hoy (pre-FASE-10)

```
┌──────────────────────────────────────────────────────────────┐
│  ring-3 ELFs en /bin (hello, ls, ovi, httpd, ...)            │
├──────────────────────────────────────────────────────────────┤
│  Syscall ABI Linux x86_64                                    │
├──────────────────────────────────────────────────────────────┤
│  shell_server.c (3775 LOC)  ─┐                               │
│  console_server.c (48 LOC)   ├─ ring 0, cooperative          │
│  keyboard_server.c (52 LOC)  │  llaman vfs_*, framebuffer_*  │
│  fs_server.c (240 LOC)       ┘  directo, no syscall          │
├──────────────────────────────────────────────────────────────┤
│  IPC + VFS + task + scheduler + drivers (kernel core)        │
└──────────────────────────────────────────────────────────────┘
```

### Después (post-FASE-10)

```
┌──────────────────────────────────────────────────────────────┐
│  ring-3 ELFs en /bin (mismos)                                │
├──────────────────────────────────────────────────────────────┤
│  ring-3 servers en /bin/osn-server/                          │
│   - consrv      (lee IPC → write /dev/fb0)                   │
│   - kbdsrv      (lee /dev/input0 → IPC al shell)             │
│   - shellsrv    (line editor + commands)                     │
│   (fs_server: ELIMINADO — shell habla vfs via syscalls)      │
├──────────────────────────────────────────────────────────────┤
│  Syscall ABI Linux x86_64 + SYS_IPC_SEND/RECV                │
├──────────────────────────────────────────────────────────────┤
│  IPC + VFS + task + scheduler + drivers (kernel core,        │
│  ~4 KLoC más chico)                                          │
│  /dev/fb0 + /dev/input0 backends nuevos en devfs             │
└──────────────────────────────────────────────────────────────┘
```

**Cambio user-visible**: cero. Mismo prompt, mismos comandos, mismas
teclas. Cambio interno: refactor masivo.

---

## Inventario de symbols a tocar

Antes de empezar es útil tener la lista completa **acá** así no
hay que grep-ear de nuevo a mitad de sesión.

### Lo que cada server kernel usa hoy (calls directas)

**console_server** (48 LOC, fácil):
```
framebuffer_draw_string(s, color)
framebuffer_clear(color)
ipc_recv_block, ipc_recv (vía while loop)
```

**keyboard_server** (52 LOC, fácil):
```
keyboard_poll() → keyboard_event_t
tty_input(c)                         (para feed al TTY)
ipc_send(...) (IPC_KEY_EVENT al shell)
```

**fs_server** (240 LOC, candidato a eliminar):
```
vfs_stat / vfs_read / vfs_write / vfs_append /
vfs_mkdir / vfs_rmdir / vfs_unlink / vfs_readdir /
vfs_list_dir / vfs_tree / vfs_copy / vfs_move /
vfs_glob_list / vfs_glob_read / vfs_glob_unlink /
vfs_touch / vfs_path_has_wildcard
```

Cada uno tiene un opcode IPC. Hoy el shell hace
`shell_send_fs1(IPC_FS_READ, path)` y el fs_server lo recibe y
llama `vfs_read`. Eso es un **wrapper inútil** una vez que el shell
sea user-mode: el shell puede usar `open()` + `read()` syscalls
directamente. Conclusión: **borrar fs_server entero**.

**shell_server** (3775 LOC, el monstruo):
```
Kernel core APIs (directo):
  - vfs_* (toda la VFS)
  - tty_* (todos los helpers de TTY)
  - framebuffer_draw_string (vía shell_send_console_color)
  - keyboard_* (no directo, vía IPC_KEY_EVENT que recibe)
  - task_* (task_by_pid, task_slot, MAX_TASKS, task_current)
  - fd_get, fd_alloc (en cmd_test)
  - pipe_create / pipe_close_writer / pipe_close_reader
  - proc_execve / proc_execve_redir / proc_execve_pipeline
  - sock_close (para fd cleanup)
  - kheap_* (counters en /sys)
  - timer_ms, timer_ticks, timer_irqs, timer_preempts
  - kmalloc / kfree
  - service_register / service_lookup
  - ipc_send / ipc_recv_block / ipc_recv / ipc_peek
  - shell_fg_pid (publicado al TTY)

Lo que expone:
  - shell_server_init, shell_server_tick (kmain las llama)
  - shell_fg_pid() (lo lee tty.c)
  - shell_send_console* helpers (lo llaman fs_server y otros)
```

Una buena fracción de eso **desaparece** cuando el shell migre:
- `cmd_test` con bypass de copy_*_user en kernel ya no aplica.
- Acceso directo a `task_slot[]` se reemplaza por leer `/sys/tasks/N`
  o por un syscall `sys_taskinfo()` minimal.
- `tty_*` directo se reemplaza por ioctl/tcsetattr (que ya existe).
- `framebuffer_*` se reemplaza por `write(stdout, ...)` ahora que
  consrv intermedia.

### Symbols del kernel que necesitarán syscall nuevo

Para que el shell ring-3 funcione, hay que exponer:

| Funcionalidad hoy | Syscall nuevo | Notas |
|---|---|---|
| `ipc_send(&msg)` | `sys_ipc_send(msg)` | Copia user→kernel del struct |
| `ipc_recv_block()` | `sys_ipc_recv(out, block)` | Blocks via libc EAGAIN loop |
| `service_register(id, pid)` | `sys_service_register(id)` | Server self-registers |
| `service_lookup(id)` | `sys_service_lookup(id)` | Returns pid |
| `task_slot(i)` (para top, ps) | `sys_taskinfo(i, out)` | Copy fields a struct |
| `proc_execve_pipe...` | (queda kernel, lo llaman syscalls) | Sin cambio |
| `kheap_*` counters | `/sys/meminfo` read (ya está) | Sin syscall — leer file |
| `timer_ms`, etc | (no necesario en shell ring-3) | Borrar el comando |

---

## Sub-fase 10.0 — Pre-reqs detallados

### 10.0.a — Per-task fd tables

**Estimate**: 500-800 LOC modificadas, **5-8 hr**.

#### Archivos a modificar

| Archivo | Cambio |
|---|---|
| `src/micro/fd.h` | Mover `typedef struct osnos_fd_t` + macros, eliminar global array decl |
| `src/micro/fd.c` | Reescribir fd_alloc/get/free para trabajar sobre task_current()->fds |
| `src/micro/task.h` | Agregar `osnos_fd_t fds[OSNOS_MAX_FDS]` a task_t |
| `src/micro/task.c` | task_clear inicializa stdin/stdout/stderr en el slot |
| `src/proc/exec.c` | task_create_user_elf llama fd_init_for(t), NO al global |
| `src/proc/exec.c` | proc_exit_current_user limpia t->fds (no el global) |
| `src/micro/syscall.c` | TODOS los `fd_get(fd)` → `fd_get(task_current(), fd)` (37 sites) |
| `src/servers/shell_server.c` | cmd_test "stdin tests" — necesita tocar shell's fds |

#### Refactor mecánico

**fd.h** — cambiar API:

```c
/* Antes */
int fd_alloc(void);
void fd_free(int fd);
osnos_fd_t *fd_get(int fd);

/* Después */
int fd_alloc(task_t *t);
void fd_free(task_t *t, int fd);
osnos_fd_t *fd_get(task_t *t, int fd);

/* O variantes "current": */
static inline int fd_alloc_cur(void) { return fd_alloc(task_current()); }
/* ... */
```

Plan: agregar las versiones con `task_t *t` explícito + helpers
"_cur" para syscall.c que es el caller más común. shell_server's
cmd_test pasaría task_by_pid(shell_pid) explícito si quiere
inspeccionar otros fds (que probablemente no necesita).

**fd.c** — `fd_init(task_t *t)` por task:

```c
void fd_init_for_task(task_t *t) {
    for (size_t i = 0; i < OSNOS_MAX_FDS; i++) {
        t->fds[i].used       = false;
        /* ... */
    }
    /* stdin/stdout/stderr para el task */
    t->fds[OSNOS_FD_STDIN].used       = true;
    t->fds[OSNOS_FD_STDIN].is_special = true;
    t->fds[OSNOS_FD_STDIN].flags      = O_RDONLY;
    /* ... stdout / stderr ... */
}
```

**syscall.c** — patrón repetitivo:

```c
/* Antes */
osnos_fd_t *f = fd_get(fd);

/* Después */
osnos_fd_t *f = fd_get(task_current(), fd);
```

37 sites. Usar `sed -i 's/fd_get(\([^,)]*\))/fd_get(task_current(), \1)/g'`
con cuidado de no romper líneas con función nested.

**task_t.fds size**: cada `osnos_fd_t` ~152 bytes (path[128] + flags +
offset + bits). × 32 fds = ~5 KB per task. × 16 tasks = 80 KB BSS.
Aceptable.

#### Eliminaciones simultáneas

Ahora que cada task tiene sus fds, lo siguiente queda obsoleto y
hay que **borrar**:

- `task_t.stdin_redir` / `stdout_redir` / `stdin_redir_off` /
  `stdout_redir_off` / `stdout_append` → reemplazar por fds que
  apunten al archivo. shell hace `open(file)` + `dup2(file_fd, 0/1)`
  antes del exec del child.
- `task_t.pipe_in` / `pipe_out` → reemplazar por fds que apunten a
  un pipe object. `sys_pipe(int fd[2])` se vuelve real.
- `sys_read(fd=0)` y `sys_write(fd=1)` simplifican: ya no chequean
  esos campos del task, todo va por la fd table.

Eso simplifica `sys_read`/`sys_write` MUCHO y limpia ~150 LOC de
hacks acumulados de las últimas sesiones.

#### Test plan 10.0.a

- `test` (kernel test suite) sigue 640/640 pass.
- `libctest` sigue ~110 pass.
- Redirects siguen andando: `echo hi > /home/x.txt && cat /home/x.txt`.
- Pipes siguen andando: `cat /home/x.txt | head -n 1`.
- Multi-stage: `a | b | c`.
- Job control: `sleep 30` + Ctrl+Z + `fg`.

Si algún test rompe, **NO AVANZAR** — el bug es en este refactor,
no en lo siguiente.

---

### 10.0.b — pipe(2) syscall + pipe object refactor

**Estimate**: 200 LOC, ~2 hr (dependiente de 10.0.a).

Ahora que hay per-task fd tables, `pipe()` deja de ser un mecanismo
exclusivo del shell y se vuelve syscall de userland:

```c
SYS_PIPE   = 22  /* Linux x86_64 */
int sys_pipe(int fd_out[2]);
```

#### Cambios

| Archivo | Cambio |
|---|---|
| `src/micro/syscall.h` | + SYS_PIPE = 22, decl |
| `src/micro/syscall.c` | impl: pipe_create + alloc 2 fds del task + linkear |
| `src/micro/fd.h` | + osnos_fd_t.pipe (struct pipe *) si is_pipe |
| `src/micro/syscall.c` | sys_read/sys_write chequean f->is_pipe |
| `lib/libc/include/unistd.h` | + int pipe(int fd[2]) |
| `lib/libc/unistd.c` | wrapper |
| `src/proc/exec.c` | proc_execve_pipeline: en vez de mutar pipe_in/out, hace pipe()/dup2 |

Estructura nueva osnos_fd_t:

```c
typedef struct {
    bool          used, is_special, is_dir, is_socket, is_pipe;
    int           sock_idx;
    struct pipe  *pipe_ref;     /* NEW — when is_pipe */
    int           pipe_side;    /* 0=read, 1=write */
    int           flags;
    uint64_t      offset;
    char          path[OSNOS_PATH_MAX];
} osnos_fd_t;
```

sys_read:
```c
if (f->is_pipe) {
    if (f->pipe_side != 0) return -EBADF;
    return pipe_read(f->pipe_ref, buf, count);
}
```

#### Test plan 10.0.b

```c
int p[2];
if (pipe(p) == 0) {
    write(p[1], "ping", 4);
    close(p[1]);
    char buf[8];
    read(p[0], buf, 4);   /* "ping" */
    close(p[0]);
}
```

ELF `/bin/pipetest` minimal. Y los pipes del shell deben seguir
andando (cat | head).

---

### 10.0.c — /dev/fb0 + /dev/input0

**Estimate**: 160 LOC, ~2 hr.

#### /dev/fb0 — character device escribible

```c
/* src/fs/devfs.c — agregar handler para "/dev/fb0" */
static osnos_status_t devfs_fb0_write(void *priv, const char *path,
                                        const char *buf, size_t n) {
    /* Echo a un buffer string aware? Mejor: pasar bytes raw.
     * framebuffer_draw_string toma un C string — necesitamos un
     * helper framebuffer_write(buf, n, color) que escriba N bytes. */
    framebuffer_write_bytes((const uint8_t *)buf, n, 0xCCCCCC);
    return OSNOS_OK;
}
```

Agregar `framebuffer_write_bytes(buf, n, color)` a `src/drivers/
framebuffer.h`. Itera n bytes y dispatcha cada uno como
`draw_string` lo hace (incluye los CSI escape sequences).

#### /dev/input0 — character device leíble

```c
/* Cuando el server ring-3 hace read(fd, &ev, sizeof(keyboard_event_t)),
 * el kernel bloquea hasta que keyboard_poll() devuelva un evento. */

static osnos_status_t devfs_input0_read(void *priv, const char *path,
                                          char *buf, size_t buf_size,
                                          size_t *out_size) {
    keyboard_event_t ev;
    if (!keyboard_poll(&ev)) {
        return OSNOS_EAGAIN;
    }
    size_t copy = sizeof(ev) < buf_size ? sizeof(ev) : buf_size;
    for (size_t i = 0; i < copy; i++) ((uint8_t *)buf)[i] = ((uint8_t *)&ev)[i];
    *out_size = copy;
    return OSNOS_OK;
}
```

libc read() ya loopea en EAGAIN — listo.

#### Test plan 10.0.c

ELF `/bin/fbtest`:
```c
int fd = open("/dev/fb0", O_WRONLY);
write(fd, "directo al framebuffer\n", 23);
close(fd);
```

ELF `/bin/inputtest`:
```c
int fd = open("/dev/input0", O_RDONLY);
keyboard_event_t ev;
while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
    printf("ascii=%d keycode=%d\n", ev.ascii, ev.keycode);
}
```

---

### 10.0.d — Headers ABI compartidos kernel ↔ ring-3

**Estimate**: 100 LOC, **~1 hr**.

Hoy los servers kernel-side `#include "../micro/ipc.h"` con
`ipc_type_t`, `ipc_msg_t`, `SERVER_*` IDs definidos del lado
kernel. Cuando un server ring-3 quiera mandar IPC, necesita ver
los mismos tipos + IDs. Hacer esto ANTES de migrar evita
divergencia ABI silenciosa.

#### Archivos nuevos

- `src/include/osnos_ipc_abi.h` — definiciones COMPARTIDAS:
  - `struct ipc_msg_t` (layout exacto, ABI estable)
  - `enum ipc_type_t` (todos los `IPC_*` opcodes)
  - `SERVER_*` IDs (`SERVER_FS=4`, etc.)
- `lib/libc/include/osnos_ipc.h` — wrapper que `#include`s el
  anterior, listo para uso desde ELFs ring-3.

#### Cambios

| Archivo | Cambio |
|---|---|
| `src/micro/ipc.h` | `#include "../include/osnos_ipc_abi.h"` y deja solo las funciones kernel-internas (`ipc_send`, `ipc_recv_block`, etc.) |
| `lib/libc/include/osnos_ipc.h` | nuevo, expone `ipc_msg_t` + `SERVER_*` + helpers de syscall |
| `src/include/osnos_status.h` | mover a `osnos_abi/` si no estaba accesible para user |
| `src/include/osnos_keys.h` | idem — los keycodes son ABI |

**Regla**: los headers en `src/include/` que terminan en `_abi.h`
o que ya se compartían (keys, status) son la **frontera ABI**.
Cambiarlos requiere coordinar kernel + libc + todos los ELFs.

#### Test plan 10.0.d

- `make clean && make` sin warnings.
- Todos los tests siguen verdes (no debería cambiar nada
  funcional — solo refactor de includes).

#### Por qué hacerlo aquí

Sin esto, en 10.1 vamos a duplicar definiciones entre kernel y
servers ring-3, y la próxima vez que cambiemos `ipc_msg_t` (e.g.
agregar `msg.timestamp`) tendríamos que tocar dos lugares y un
mismatch silencioso corrompería todos los IPCs. ABI primero.

---

### 10.0.e — Mover cmd_test a `/bin/kerntest` ELF separado

**Estimate**: ~200 LOC mover + 50 nuevo helper, **~2 hr**.

Hoy `cmd_test` vive dentro de `shell_server.c` y llama
syscalls + funciones kernel-internas con kernel-mode bypass. Es
**la parte más complicada del shell**. Si la sacamos AHORA (antes
de tocar nada del shell ring-3), el shell_server.c se queda más
chico para 10.4, y `test` sigue funcionando idéntico de cara al
usuario.

#### Archivos nuevos

- `elfs/tests/kerntest.c` — todos los 640 checks que hoy hace
  `cmd_test`.
- `src/include/osnos_taskinfo.h` — struct ABI para `sys_taskinfo`
  (los CHECKs que leen task_slot necesitan equivalente userland).

#### Archivos modificados

- `src/servers/shell_server.c` — borrar `cmd_test` entero (~400
  LOC). Reemplazar la entry del table por:
  ```c
  CMD("test", cmd_test_exec, "run /bin/kerntest"),
  ```
  donde `cmd_test_exec` simplemente hace `proc_execve("/bin/kerntest"...)`.

#### Reescritura de checks

Cada CHECK que tocaba kernel internals se traduce:

| Antes (cmd_test) | Después (kerntest.c) |
|---|---|
| `osnos_fd_t *f = fd_get(fd)` | `struct stat st; fstat(fd, &st)` |
| `task_slot(i)` | `sys_taskinfo(i, &info)` (nuevo syscall) |
| `vfs_stat(path, ...)` | `stat(path, &st)` |
| `vfs_read(path, buf, sz, &got)` | `int fd = open(path); read(fd, buf, sz)` |
| `sock_close(idx)` | (no aplica — ELF no inspecciona) |
| `kheap_used_bytes()` | parse `/sys/meminfo` |
| `kheap_grow_events()` | parse `/sys/meminfo` |
| `sys_open` con kernel ptr | `open()` normal (es ring 3 real) |

Hay ~20 checks que necesitan `sys_taskinfo` o counters. Agregarlo
acá adelanta el syscall a 10.0 en vez de 10.4.

#### Syscall nuevo: SYS_TASKINFO

Adelantado de 10.4. Signature:

```c
struct osnos_taskinfo {
    uint64_t pid;
    char     name[32];
    uint8_t  state;          /* TASK_READY / RUNNING / BLOCKED / STOPPED / DEAD */
    uint8_t  is_user;        /* pml4 != 0 */
    uint8_t  pad[6];
    uint64_t dispatches;     /* contador para top */
};
int64_t sys_taskinfo(size_t idx, struct osnos_taskinfo *out);
```

#### Test plan 10.0.e

- Boot. `test` → `kerntest` ELF arranca y produce mismos 640
  PASS que antes.
- `libctest` ELF sigue ~110 pass.
- Comportamiento user-visible **idéntico** — solo cambió que
  ahora `test` es un ELF.

#### Por qué hacerlo aquí (no en 10.4)

Cuando lleguemos al refactor del shell (10.4), `cmd_test` ya está
fuera. El shell queda con ~3375 LOC (de 3775) y todos los
syscalls de inspección kernel quedan solo en kerntest.c —
zero overlap con la lógica del shell. Reduce el riesgo del
refactor grande sustancialmente.

---

## Sub-fase 10.1 — console_server a ring 3

**Estimate**: 100 LOC, **1 hr**.

### Archivos nuevos

- `elfs/osn-server/consrv.c`

### Archivos eliminados

- `src/servers/console_server.c` (48 LOC)
- `src/servers/console_server.h`

### consrv.c skeleton

```c
/*
 * /bin/osn-server/consrv — console server in ring 3.
 *
 * Bridges IPC_CONSOLE_WRITE / IPC_CONSOLE_CLEAR from anywhere in
 * the system to /dev/fb0. Kernel keeps the framebuffer driver;
 * we are just an IPC→fd shim.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ipc.h"        /* TODO: extract from kernel header to userland */

int main(void) {
    int fb = open("/dev/fb0", O_WRONLY);
    if (fb < 0) { fprintf(stderr, "consrv: cannot open /dev/fb0\n"); return 1; }

    /* Self-register so the shell + others find us by SERVER_CONSOLE. */
    syscall_service_register(SERVER_CONSOLE);

    ipc_msg_t msg;
    for (;;) {
        if (ipc_recv_block(&msg) < 0) continue;
        switch (msg.type) {
        case IPC_CONSOLE_WRITE:
            /* arg1 = byte count, data[] payload (may carry colour
             * escapes inline via VT100 CSI \x1b[3xm). */
            write(fb, msg.data, msg.arg1);
            break;
        case IPC_CONSOLE_CLEAR:
            write(fb, "\x1b[2J\x1b[H", 7);  /* VT100 clear + home */
            break;
        default:
            break;
        }
    }
}
```

### kmain change

```c
/* Antes */
int console_pid = task_create("console", console_server_tick);
service_register(SERVER_CONSOLE, console_pid);
console_server_init();

/* Después */
int64_t console_pid = proc_execve("/bin/osn-server/consrv", "", 0);
/* No service_register acá — el server lo hace solo via syscall. */
```

### Test 10.1

- `hello` imprime "hola, mundo" en el framebuffer ✓
- `cat /home/x.txt` muestra contenido ✓
- `ls`, `top` siguen funcionando ✓

---

## Sub-fase 10.2 — keyboard_server a ring 3

**Estimate**: 80 LOC, **1 hr**.

### Archivos nuevos

- `elfs/osn-server/kbdsrv.c`

### Archivos eliminados

- `src/servers/keyboard_server.c` (52 LOC)
- `src/servers/keyboard_server.h`

### kbdsrv.c skeleton

```c
/*
 * /bin/osn-server/kbdsrv — keyboard server in ring 3.
 *
 * Reads from /dev/input0, feeds the TTY layer via a new
 * sys_tty_input syscall, and forwards key events to whichever
 * task registered as the focus listener (shell today).
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int kbd = open("/dev/input0", O_RDONLY);
    if (kbd < 0) return 1;

    syscall_service_register(SERVER_KEYBOARD);

    for (;;) {
        keyboard_event_t ev;
        ssize_t n = read(kbd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;

        /* Feed printables + arrow-keys-as-CSI to the TTY same as
         * the kernel keyboard_server did. */
        if (ev.ascii != 0 && ev.keycode == 0) {
            syscall_tty_input((char)ev.ascii);
        } else if (ev.keycode != 0) {
            char final = 0;
            switch (ev.keycode) {
                case KEY_UP:    final = 'A'; break;
                case KEY_DOWN:  final = 'B'; break;
                case KEY_RIGHT: final = 'C'; break;
                case KEY_LEFT:  final = 'D'; break;
            }
            if (final) {
                syscall_tty_input(0x1B);
                syscall_tty_input('[');
                syscall_tty_input(final);
            }
        }

        /* IPC_KEY_EVENT to the shell for arrow nav / history. */
        ipc_msg_t msg = {
            .from = getpid(),
            .to   = SERVER_SHELL,
            .type = IPC_KEY_EVENT,
            .arg0 = ev.keycode,
            .arg1 = 0,
        };
        msg.data[0] = ev.ascii;
        msg.data[1] = 0;
        ipc_send(&msg);
    }
}
```

### Syscall nuevo necesario

`sys_tty_input(char c)` — el server ring-3 NO puede llamar a
`tty_input` directo (vive en el kernel). Necesitamos un syscall
mínimo que invoque `tty_input(c)` desde el kernel. Solo el server
con SERVER_KEYBOARD lo debería poder llamar — chequear vía
service registry.

### Test 10.2

- Tipear teclas en el shell → eco visible.
- Ctrl+C → señal a fg task.
- Ctrl+Z → stop fg task.
- Arrow keys en ovi → cursor se mueve.

---

## 🛑 Checkpoint — consolidar 10.1 + 10.2 antes de seguir

**No tocar el shell hasta que console + keyboard ring-3 estén
100% estables**. Los crashes del shell son catastróficos (el
usuario queda sin UI); los crashes de console/keyboard ring-3
también, pero al menos los podemos restartear sin perder la
"interfaz primaria".

### Criterios para pasar a 10.3 / 10.4

- [ ] **Una sesión COMPLETA de uso normal** con console+kbd
      ring-3, sin reboots por bugs. "Uso normal" = abrir/cerrar
      ovi, browse FAT, ejecutar tests, navegar history, Ctrl+Z
      varios procesos, `ps`/`top` ELFs.
- [ ] Todos los tests verdes 3 veces consecutivas (test, libctest,
      mmaptest, fptest×5 en paralelo, pipes multi-stage, redirs).
- [ ] `ps` muestra console+kbd como tasks ring-3 (`pml4 != 0`)
      y se ven sus dispatches creciendo.
- [ ] Sin "lag visible" tipeando — si lo hay, aplicar la
      mitigación R1 (keyboard batch) antes de pasar.
- [ ] Borrar `console_server.c` + `keyboard_server.c` del
      repo, no solo dejarlos sin uso.

Si alguno falla, **NO empezar 10.3**. Es preferible quedarse
una sesión más solidificando que arrancar 10.4 sobre cimientos
frágiles — porque cuando el shell migre y crashee, vas a
debuguear DOS cosas (shell nuevo + console/kbd reciente) en
vez de UNA.

### Si surgen mejoras durante esta pausa

Cosas que pueden aparecer y vale la pena hacer **antes** de
10.3:

- Respawn loop para servers caídos en kmain (R3 mitigación).
- Performance counters de IPC + context-switches para detectar
  regresiones temprano.
- Migrar más helpers del shell que ya sabemos vamos a necesitar
  ring-3 (e.g., una `shell_send_console_color` que sea wrapper
  de `printf` con escapes ANSI, lista para usar desde el shell
  ring-3).

---

## Sub-fase 10.3 — fs_server: eliminación

**Estimate**: shell refactor ~200 LOC, **2 hr**.

### Por qué eliminar

`fs_server` hoy es un IPC→VFS wrapper. Cuando el shell sea ring 3,
puede llamar directamente a `open()`/`read()`/`stat()`/etc. via
syscalls. **No necesita un broker intermedio**.

### Archivos eliminados

- `src/servers/fs_server.c` (240 LOC)
- `src/servers/fs_server.h`
- Símbolos del IPC: `IPC_FS_READ`, `IPC_FS_LIST`, `IPC_FS_TREE`,
  `IPC_FS_MKDIR`, ... en `src/micro/ipc.h`.

### Cambios en shell

| Función actual | Reemplazo |
|---|---|
| `shell_send_fs1(IPC_FS_READ, path)` | `int fd = open(path); read(...); close(...)` |
| `shell_send_fs1(IPC_FS_LIST, path)` | `opendir + readdir loop` |
| `shell_send_fs1(IPC_FS_MKDIR, path)` | `mkdir(path)` |
| `shell_send_fs2(IPC_FS_COPY, src, dst)` | `open(src) + read + open(dst) + write` loop |
| `shell_send_fs1(IPC_FS_TREE, path)` | helper que llame `opendir` recursivo |

Esto se hace **MIENTRAS** el shell sigue en ring 0, así no mezclamos
cambios. Funcionalidad idéntica, solo cambia el path.

### Test 10.3

- `ls /home`, `cat /home/x.txt`, `cp foo bar`, `mkdir /tmp/x`,
  `rm /tmp/x/foo`, todo idéntico a antes.

---

## Sub-fase 10.4 — shell_server a ring 3

**Estimate**: ~1100 LOC modificadas (sin cmd_test, que ya está
fuera desde 10.0.e), **4-5 hr**.

### Pre-requisito interno (ya cubierto)

`cmd_test` está fuera en `/bin/kerntest` desde sub-fase **10.0.e**.
El shell queda con ~3375 LOC de lógica pura (line editor, history,
command dispatch, IPC handlers, parser de pipes/redirects). Nada
de introspección kernel directa.

`cmd_top` — todavía vive en shell pero usa `sys_taskinfo` (también
agregada en 10.0.e). Ya es ring-3 friendly.

### Archivos nuevos

- `elfs/osn-server/shellsrv.c` — el shell propiamente dicho.

### Archivos eliminados

- `src/servers/shell_server.{c,h}` (3775 LOC) — todo.

### Refactor del shell

El esqueleto del shell post-FASE-10:

```c
int main(void) {
    syscall_service_register(SERVER_SHELL);

    /* line editor loop */
    for (;;) {
        ipc_msg_t msg;
        if (ipc_recv_block(&msg) < 0) continue;
        if (msg.type == IPC_KEY_EVENT) handle_key(&msg);
        if (msg.type == IPC_PROC_EXITED)  handle_exit(&msg);
        if (msg.type == IPC_PROC_STOPPED) handle_stopped(&msg);
        /* etc */
    }
}
```

Internamente cada `cmd_*` (cmd_ls, cmd_cat, ...) sigue ahí pero
ahora hace syscalls puros. Es **el grueso del trabajo**: ~50
funciones a refactorear.

### Tests del shell

`cmd_test` ya está fuera como `/bin/kerntest` (sub-fase 10.0.e),
así que **el shell ring-3 ni necesita pensarlo**. El `test`
shell command es ahora solo `proc_execve("/bin/kerntest")` —
una línea.

`libctest` ya era ELF ring-3 desde siempre.

### Test 10.4

**Esto es el momento crítico**. Boot → shell ring-3 corriendo:

- Prompt aparece ✓
- Tipear teclas eco ✓
- Comandos shell builtin (ls/cat/cd/echo/env/export) ✓
- Comandos ELF (hello/httpd/ovi/etc) ✓
- Redirects (`>`, `>>`, `<`) ✓
- Pipes (`a | b | c`) ✓
- Background (`&`) ✓
- Ctrl+C / Ctrl+Z / fg / bg / jobs ✓
- `test` (que ahora es `kerntest` ELF) → 640+/640+ pass ✓
- `libctest` ELF → 110+ pass ✓
- mmaptest / fptest ✓
- ovi edit + save ✓

Si alguno falla → debug ANTES de declarar 10.4 cerrada.

---

## Sub-fase 10.5 — Cleanup + docs

**Estimate**: 100 LOC docs, **1 hr**.

### Cleanup técnico

- Borrar `src/servers/` entero (o dejar README explicando "vacío
  desde FASE 10").
- Borrar opcodes IPC_FS_* y IPC_KEY_EVENT del kernel (ya no se
  usan kernel-side; los servers ring-3 los conocen pero los
  define el ABI en un .h compartido).
- Eliminar `shell_server_init`, `shell_server_tick`,
  `keyboard_server_init`, `keyboard_server_tick`, etc, del kmain.

### Documentación

- `ARCH.md`: nuevo diagrama de capas, sin la fila "servers en
  ring 0". Walkthrough actualizado: "una tecla viaja por todo el
  sistema" post-FASE-10.
- `STATUS.md`: marca FASE 10 CERRADA + resumen ejecutivo updated.
- `CLAUDE.md`: explicar que los servers son ELFs en
  `elfs/osn-server/`, no .c en `src/servers/`.
- `README.md`: tabla de estado tachar "ring 0 hoy" del row
  "Servers en ring 3".

---

## ABI de IPC user-mode

### Syscalls nuevos

```c
/* No-Linux range — above 250 (osnos-specific). */
#define SYS_IPC_SEND          260
#define SYS_IPC_RECV          261
#define SYS_SERVICE_REGISTER  262
#define SYS_SERVICE_LOOKUP    263
#define SYS_TTY_INPUT         264    /* kbdsrv solo */
#define SYS_TASKINFO          265    /* leer state de otra task */
```

Signatures:

```c
int64_t sys_ipc_send(const ipc_msg_t *user_msg);
   /*  copy_from_user del msg, ipc_send interno.
    *  Returns 0 / -EAGAIN (cola llena) / -ESRCH (target no existe). */

int64_t sys_ipc_recv(ipc_msg_t *user_out, int blocking);
   /*  Si blocking == 0: pop o EAGAIN. Si blocking == 1: bloquea
    *  vía la cooperative wait que ya tiene ipc_recv_block. */

int64_t sys_service_register(int sid);
   /*  Asocia sid (SERVER_*) con task_current()->pid. */

int64_t sys_service_lookup(int sid);
   /*  Returns pid o -ENOENT. */

int64_t sys_tty_input(int c);
   /*  Solo si task_current()->pid == service_lookup(SERVER_KEYBOARD).
    *  Llama tty_input(c). Otros: -EPERM. */

int64_t sys_taskinfo(size_t idx, struct osnos_taskinfo *out);
   /*  Volcar fields públicos de tasks[idx] (state, pid, name) sin
    *  exponer las internals. Para cmd_top / cmd_ps / kerntest. */
```

### Security model

- Cualquier task puede SEND a cualquier sid (no enforcement de
  permisos hoy).
- Solo el "owner" del SERVER_KEYBOARD puede llamar SYS_TTY_INPUT.
- SYS_TASKINFO es read-only y solo expone campos seguros.

---

## Riesgos + mitigaciones

### R1 — Performance del shell

**Síntoma**: tipear se siente lento. Cada tecla pasa por keyboard
ring-3 → kernel → IPC → shell ring-3 (2 context switches).

**Mitigación**: keyboard server batch eventos. Si llegan 5 teclas
en un timer tick, las junta en un solo msg con `arg1 = N`. Shell
las procesa de a una. Reduce switches a O(N).

**Confirmar**: contadores en `/sys/meminfo` o nuevo `/sys/sched`
con `task_switches/sec`. Si crece >10×, batch.

### R2 — Init order race

**Síntoma**: shell empieza antes que console_server, primer
`shell_send_console` falla EAGAIN, banner perdido.

**Mitigación**: kmain spawn los servers en orden estricto:
1. console_server.
2. spin hasta service_lookup(SERVER_CONSOLE) != 0.
3. keyboard_server.
4. spin hasta lookup.
5. shell_server.

Spin sin yield es OK porque servers son tasks listos a despachar.

### R3 — Crash del shell deja el sistema sin UI

**Síntoma**: shell ELF falla, proc_exit_current_user, no hay
shell para tipear.

**Mitigación corto plazo**: kmain registra un "respawn loop" que
detecta IPC_PROC_EXITED de SERVER_SHELL y re-execs el shell.

**Mitigación largo plazo**: signal handler en el shell, debug
mode, init proper.

### R4 — Tests que tocan internals del kernel

**Síntoma**: cmd_test del shell-ring-3 no compila porque ya no
puede ver `fd_get`, `task_slot`.

**Mitigación**: mover esos checks a `elfs/tests/kerntest.c` con
sys_taskinfo + read /sys/* + open/close para inspeccionar fds.

### R5 — copy_*_user del kernel hace bypass cuando caller es kernel

**Síntoma**: shell antes corría con bypass (kernel-mode caller).
Ahora ring-3 = bypass desactivado. Si pasa puntero no-canónico,
EFAULT. Probablemente OK porque ring-3 user ptrs son
"naturales".

**Mitigación**: revisar `in_kernel_caller()` en `src/micro/
uaccess.c` — debería quedar solo para tests específicos. Considerar
eliminar el bypass entero post-FASE-10.

---

## Test plan integral

Antes de declarar FASE 10 cerrada, **ejecutar TODO esto**:

### Bater estado

```sh
make clean && make 2>&1 | grep -E "error|warning:" | wc -l   # debe ser 0
```

### Boot smoke

```sh
./build_and_run.sh
# Verifica:
#   - banner del shell aparece
#   - prompt "osnos:/home> " aparece
```

### Kernel tests

```sh
test    # corre 2 veces
```
Esperá ~640/640 ambas veces.

### Libc tests

```sh
libctest    # ~110 pass
```

### Subsystem tests

```sh
mmaptest    # 12 pass
fptest 500 &
fptest 500 &
fptest 500  # 3 paralelos, mismo output bit-exact
```

### Shell features

```sh
# Builtins
ls, cat, cd, mkdir, rmdir, touch, rm, mv, cp, echo, env, export

# ELFs
hello, httpd &, kill <pid>, top (Ctrl+C)

# Redirects + pipes
echo -e "a\nb\nc\nd" > /home/x.txt
cat /home/x.txt | head -n 2 > /home/y.txt
cat < /home/x.txt | head -n 3 | head -n 1

# Job control
sleep 30
^Z         # stops
jobs       # lista
fg         # resume
^C         # kill

# Editor
ovi /home/x.txt   # navigate hjkl + arrows, :w, :q
```

### Network smoke

```sh
tcpclient google.com 80    # DNS + outbound TCP
httpd &                     # listen on 8080
# curl from host:
#   curl http://localhost:8080/
```

Si **TODO** lo anterior pasa, FASE 10 está cerrada.

---

## Helpers + snippets de partida

### Helper para ELFs ring-3 que necesitan syscall directo

Algunos servers necesitan syscalls que la libc no expone (e.g.,
`sys_service_register`). Crear un header `lib/libc/include/
osnos_sys.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../syscall.h"

static inline int64_t syscall_service_register(int sid) {
    return osnos_syscall1(SYS_SERVICE_REGISTER, sid);
}
static inline int64_t syscall_service_lookup(int sid) {
    return osnos_syscall1(SYS_SERVICE_LOOKUP, sid);
}
static inline int64_t syscall_tty_input(int c) {
    return osnos_syscall1(SYS_TTY_INPUT, c);
}
static inline int64_t syscall_ipc_send(const ipc_msg_t *m) {
    return osnos_syscall1(SYS_IPC_SEND, (long)m);
}
static inline int64_t syscall_ipc_recv(ipc_msg_t *m, int blocking) {
    return osnos_syscall2(SYS_IPC_RECV, (long)m, blocking);
}
```

Los servers `#include <osnos_sys.h>` y listo.

### Pattern para spawn de server en kmain

```c
/* Spawn osnos servers in order, waiting for each registration. */
static void spawn_osn_server(const char *path, int expected_sid) {
    int64_t pid = proc_execve(path, "", env_default);
    if (pid < 0) panic("kmain: cannot spawn %s", path);
    /* Wait for the server to call sys_service_register(sid).
     * Cooperative: each task_run_next call gives the spawned
     * task a chance to run its init. */
    for (int i = 0; i < 100; i++) {
        if (service_lookup(expected_sid) != 0) return;
        scheduler_tick();
    }
    panic("kmain: %s never registered SID %d", path, expected_sid);
}

/* In kmain: */
spawn_osn_server("/bin/osn-server/consrv",  SERVER_CONSOLE);
spawn_osn_server("/bin/osn-server/kbdsrv",  SERVER_KEYBOARD);
spawn_osn_server("/bin/osn-server/shellsrv", SERVER_SHELL);
```

---

## Checklist al retomar

### Primera cosa al volver

1. Abrir este file.
2. `cd /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos`
3. `git status` — debe estar limpio o reflejar EXACTAMENTE dónde
   quedó la sesión previa.
4. `make clean && make` — debe compilar limpio.
5. `./build_and_run.sh` y correr `test` + `libctest` para
   confirmar baseline. **No avanzar si fallan**.

### Estado al cierre de cada sub-fase

Al terminar cada sub-fase, ANOTAR aquí en commit message + en
STATUS.md:

- Sub-fase: nombre + LOC
- Tests: qué pasaron
- Lo siguiente: qué sub-fase toca

### Reglas que no rompemos

- **Una sub-fase por sesión**. No empezar 10.2 antes de cerrar 10.1
  con tests verdes.
- **Tests antes de avanzar**. Si rompemos algo, paramos y
  arreglamos. No acumular bugs.
- **Cero warnings**. `-Werror` está activo.
- **No tocar `src/servers/` mientras 10.0 no esté cerrada**.
- **Commit (o snapshot) al final de cada sub-fase**.

### Punto de entrada concreto

**Próxima sesión arrancar con: Sub-fase 10.0.a — Per-task fd tables.**

Es el refactor más grande pero el más mecánico — leer cada caller
de `fd_get`/`fd_alloc`/`fd_free` (37 sites) y agregar
`task_current()` como primer arg. Una vez funciona, lo demás baja
en complejidad.

Tiempo estimado para 10.0.a sola: **5-8 hr concentradas**. Si la
sesión es de 2-3 hr, dividir en:
- Sesión 1: fd.{c,h} + task.h + syscall.c (todas las llamadas).
- Sesión 2: limpiar pipe_in/pipe_out hack + stdin_redir hack
  porque ahora son fds reales.
- Sesión 3: pipe(2) expose + /dev/fb0 + /dev/input0.
- Sesión 4: headers ABI (10.0.d) + extraer cmd_test a kerntest
  ELF (10.0.e). **Importante hacer estos dos juntos** — la ABI
  estable habilita el ELF y el ELF revela cualquier gap en la
  ABI.

Cada una se valida con el `test plan` correspondiente antes de
pasar a la siguiente.

### Reglas importantes (resumen de los ajustes)

1. **FASE 10 ≠ FASE 11.** Servers a ring 3 (este plan) vs drivers
   a ring 3 (futuro). NO mezclar.
2. **Headers ABI primero (10.0.d)** — antes de cualquier server
   ring-3, dejar la frontera kernel/ring-3 bien definida.
3. **NO migrar el shell** hasta que console + keyboard ring-3
   hayan sobrevivido una sesión completa de uso real (ver
   checkpoint entre 10.2 y 10.3).
4. **Extraer cmd_test a /bin/kerntest cuanto antes** (10.0.e)
   — reduce el alcance del refactor del shell sustancialmente
   y deja la introspección kernel en un único ELF testeable
   por separado.

---

## Estimate total (refinado)

| Sub-fase | Hr |
|---|---|
| 10.0.a per-task fd tables | 5-8 |
| 10.0.b pipe(2) syscall | 2 |
| 10.0.c /dev/fb0 + /dev/input0 | 2 |
| 10.0.d headers ABI compartidos | 1 |
| 10.0.e cmd_test → /bin/kerntest ELF | 2 |
| 10.1 console_server | 1 |
| 10.2 keyboard_server | 1 |
| **Checkpoint** (consolidar 10.1+10.2) | 0 LOC, 1 sesión de uso real |
| 10.3 fs_server (eliminar) | 2 |
| 10.4 shell_server (sin cmd_test) | 4-5 |
| 10.5 cleanup + docs | 1 |
| **Total** | **21-25 hr** |

≈ **5-6 sesiones de trabajo concentrado de 4-5 hr cada una**.

Distribución por sesión sugerida:

- **Sesión 1**: 10.0.a (per-task fd tables) — el grande.
- **Sesión 2**: 10.0.b + 10.0.c (pipe + /dev/*).
- **Sesión 3**: 10.0.d + 10.0.e (ABI + kerntest extraction).
- **Sesión 4**: 10.1 + 10.2 (console + keyboard). Termina con
  el checkpoint.
- **Sesión 5**: 10.3 + 10.4 (fs eliminar + shell ring-3).
- **Sesión 6**: 10.5 (cleanup + docs) + testing largo.

---

## Después de FASE 10

### FASE 11 — Drivers a ring 3 (no entra en este plan)

El gran refactor que sigue. Mucho más complejo que FASE 10
porque toca:

- **IRQ delegation**: hoy el timer IRQ corre en kernel y llama
  scheduler_tick. Para PS/2 ring-3 necesitamos que el IRQ
  handler kernel-side mande un IPC al driver, que despierta y
  consume el scancode. Latency adicional, locking, IRQ
  threading.
- **MMIO mapping per-task**: el framebuffer es una región de
  memoria fija del PCI. Para que un driver ring-3 lo escriba,
  el kernel le tiene que mapear esas páginas en su pml4 con
  permisos especiales.
- **Port I/O delegation**: ATA usa `in`/`out` en CPL=0. Hay que
  exponer un syscall `sys_inb`/`sys_outb` con whitelist por
  task, o usar IOPB en TSS para dar acceso selectivo.
- **DMA bouncing**: RTL8139 usa DMA. Driver ring-3 no puede
  programar direcciones físicas directamente; necesita
  buffer pool kernel-mediated.

**Scope FASE 11**: portar PS/2, framebuffer, ATA, RTL8139, PIT
a `elfs/osn-driver/`. Es **dos a tres veces** el trabajo de
FASE 10. Mejor cerrarla cuando FASE 10 esté sólida + tengamos
varias semanas de uso real validando que el modelo IPC
escala.

**No mezclar FASE 10 y FASE 11 en una sesión**. Cada una es
un capítulo en sí mismo.

### Otros pasos naturales post-FASE-10 (más cortos)

- **fork()** real. Per-task fd tables ✓ + COW pml4 (nuevo trabajo).
- **execve()** real con preservación de pid. Libc surface ya
  existe.
- **Real init process**. Hoy kmain spawn shell directo; con fork
  podríamos tener `/bin/init` que spawn shell + servers.
- **TCC port real** (cubierto en ROADMAP_APENDICE.md).

Postponed:
- PTY pairs (job control simple ya alcanza para mayoría de uso).
- Real-time signals (signal queue, sigaction, etc).
- SMP. Mucho más adelante; FASE 10/11 implican zero shared
  state, que ayuda enorme cuando lleguemos.

---

**Fin del plan.** Cualquier cosa que se vaya descubriendo durante
la implementación, escribirla AQUÍ en lugar de empezar otro file.
Mantener este archivo como la fuente de verdad para FASE 10.
