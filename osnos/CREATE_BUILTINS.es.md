# Builtins en osnos: ELFs y blobs

Esta guía explica las dos formas de poner algo en `/bin/` en osnos.
El modelo viejo de **builtin kernel-mode** (función C compilada en el
kernel, expuesta como `bn_xxx`) **fue eliminado en FASE 7.5**: el
macro `KERN`, el campo `main` del `builtin_t` y `builtin_trampoline`
ya no existen. Todo en `/bin/` corre en ring 3.

Los dos modos vivos hoy:

1. **USERELF** — el modo por defecto. Tu programa es un ELF libc-linked
   (`int main(int argc, char **argv)`). Es lo que usan los ~20 tools
   en `tests/`. Documentado en detalle en **`CREATE_ELF.es.md`** — esta
   doc no lo repite.
2. **USER** — un blob de asm "flat" embebido en el kernel. Sin libc,
   sin crt0, sin linker script. Se usa sólo para probar el path
   end-to-end de la ABI: `ring3hello`, `ring3int80`, `ring3fault`.

Si vas a escribir una herramienta o un comando, **usá USERELF**
(`CREATE_ELF.es.md`). Si estás depurando el flujo bajo (SYSCALL vs
`int 0x80`, fault recovery, etc.), seguí leyendo acá.

---

## El modo USER — blob de asm flat

Lo que el kernel hace con un USER builtin:

1. Aloca una página de código en `USER_CODE_VIRT = 0x400000` con
   `PTE_U|PTE_X` (sin PTE_W; el código no se modifica a sí mismo).
2. Copia los bytes del blob `[start, end)` ahí.
3. Rellena el resto de la página con `0xCC` (int3) para que cualquier
   fall-through trapee inmediatamente.
4. Aloca una página de stack en `0x7FFFE000-0x7FFFF000` con
   `PTE_U|PTE_W`, RSP en `0x7FFFF000`.
5. iretq con CS=0x23 (user code), SS=0x1b (user data), RFLAGS=0x202
   al primer byte del blob.

A diferencia de USERELF, **no hay loader que parsee program headers,
no hay address space layout configurable, no hay argv**. Una página
de código + una página de stack. RIP empieza al byte 0 del blob.

### Cuándo usarlo

- Probar SYSCALL/sysretq vs int 0x80 sin que clang/libc metan
  prologue/epilogue ni stack frames raros.
- Verificar que el fault handler reciba un #GP/#PF desde CPL=3 y
  termine bien la task. `ring3fault` es exactamente eso.
- Demos minimales del syscall ABI: `ring3hello` tiene 10 instrucciones
  asm que llaman `sys_write` + `sys_exit`. Sirve para ver el byte
  exacto que el CPU ejecuta cuando hace `syscall`.

### Cuándo NO usarlo

- Para cualquier programa "de verdad" — el código mismo en C cuesta
  más escribirlo en asm puro y no ganás nada vs un USERELF de 20 KiB.

### Ejemplo vivo

`src/proc/builtin.c`:

```c
__asm__ (
    ".section .text\n"
    ".global user_hello_start\n"
    ".global user_hello_end\n"
    "user_hello_start:\n"
    "    movq $1, %rax\n"                /* sys_write */
    "    movq $1, %rdi\n"                /* fd = stdout */
    "    leaq user_hello_msg(%rip), %rsi\n"
    "    movq $17, %rdx\n"               /* len("hello from ring3\n") */
    "    syscall\n"
    "    movq $60, %rax\n"               /* sys_exit */
    "    movq $0, %rdi\n"
    "    syscall\n"
    "user_hello_msg:\n"
    "    .ascii \"hello from ring3\\n\"\n"
    "user_hello_end:\n"
);

extern const uint8_t user_hello_start[];
extern const uint8_t user_hello_end[];

/* en builtins[]: */
USER("ring3hello", user_hello_start, user_hello_end,
     "ring-3 task: prints via SYSCALL fast path"),
```

Notá:
- **`movq $imm, %rax`** carga sí mismo el syscall number — no hace
  falta `#define` macros porque es asm puro.
- **PIE-style `leaq user_hello_msg(%rip), %rsi`** — el blob no
  conoce su dirección base hasta que el loader lo copia, así que
  todas las refs son RIP-relative. El loader lo pone en `0x400000`
  pero el asm no asume nada.
- **`.ascii`** dentro del mismo `.text` — sin separación rodata.
  Como el blob entero queda en una página R+X, está bien meter
  strings ahí.

### Registrar un USER nuevo

Tres pasos en `src/proc/builtin.c`:

1. Escribir el bloque `__asm__(...)` con las labels `start`/`end`.
2. Declarar los `extern const uint8_t` de las labels.
3. Agregar entrada en `builtins[]`:

   ```c
   USER("nombre", start_label, end_label, "descripción"),
   ```

No hace falta tocar el `GNUmakefile` — los blobs son `__asm__`
file-scope, compilan junto a `builtin.c`.

---

## Resumen de macros en `builtin.c`

```c
#define USER(n,    s, e, desc)  { n, s, e, 0, 0, desc }
#define USERELF(n, s, e, desc)  { n, 0, 0, s, e, desc }
#define ELF(n, desc) \
    USERELF(#n, _binary_##n##_elf_start, _binary_##n##_elf_end, desc)

#define DECLARE_ELF(name) \
    extern const uint8_t _binary_##name##_elf_start[]; \
    extern const uint8_t _binary_##name##_elf_end[]
```

| Macro      | Para qué                                              |
|------------|-------------------------------------------------------|
| `USER`     | Blob de asm flat (hand-rolled, sin libc, sin ELF)     |
| `USERELF`  | ELF64 embebido — uso directo si necesitás controlar   |
|            | los nombres de símbolos                               |
| `ELF`      | Shortcut sobre USERELF para el patrón estándar        |
|            | `_binary_<n>_elf_start/_end`                          |
| `DECLARE_ELF` | extern declarations a los dos símbolos `_binary_*` |

Para 95% de los casos: `DECLARE_ELF(foo);` + `ELF(foo, "desc")`.

---

## Qué pasó con los builtins kernel-mode

Hasta FASE 7.5 existía un tercer flavor: `KERN("nombre", fn, "desc")`
donde `fn` era una función C `static int fn(const char *args)`
compilada en el kernel. Corría en ring 0 pero solo podía usar la
ABI de syscalls (sin tocar VFS/IPC directo), para que migrar a ELF
fuera un copy-paste cuando llegara FASE 6.

En FASE 7.5 todos esos fueron migrados a ELFs libc-linked en
`tests/<n>.c`. El cleanup removió:

- `KERN` macro
- el campo `main` del `builtin_t`
- `builtin_trampoline` en `src/proc/exec.c`
- el dispatcher kernel-vs-user en `proc_exec`
- los campos `priv` y `args` de `task_t`

El `proc_exec` actual sólo elige entre USER (blob plano) y USERELF.
Si necesitás un builtin con lógica C, **escribilo como USERELF**:
es el mismo C, lo único que cambia es que el linker lo empaqueta
como ELF y `crt0.S` te llama `main`.

---

## Cuando algo falla

Para USER blobs:

- **#UD inmediato (invalid opcode)**: el blob arranca en una offset
  raro o no contiene asm válida en byte 0. Verificá con
  `nm build/kernel | grep user_<n>_start` y mirá el byte ahí.
- **#GP al hacer `syscall`**: probablemente clobbeaste RCX o R11
  antes del syscall (el CPU los usa). Si tu blob hace syscall
  múltiple, guardá RCX/R11 en el stack si los necesitás.
- **#PF en address rara**: leíste/escribiste fuera de los 4 KiB de
  code o los 4 KiB de stack. USER no tiene heap, ni `.data`, ni
  PT_LOAD múltiples — todo lo que tenés son esas dos páginas.

Para USERELF, ver la sección "Debugging" de `CREATE_ELF.es.md`.

---

## Checklist rápida — USER builtin nuevo

```
[ ] __asm__("...start: ... end:") en src/proc/builtin.c
[ ] extern const uint8_t <name>_start[], <name>_end[];
[ ] USER("nombre", <name>_start, <name>_end, "descripción"),
[ ] make run-bios
[ ] osnos> exec /bin/<nombre>
```

Para todo lo demás: **leé `CREATE_ELF.es.md`**.
