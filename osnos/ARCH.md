# OSnOS — Arquitectura

Sistema operativo microkernel-style minimalista. Boot por Limine. Los
servers (shell, console, keyboard, fs) corren en ring 0 todavía; user
tasks reales corren en ring 3 con su propio address space y entran al
kernel vía `syscall` (preferido) o `int 0x80` (legacy). FASE 10 del
ROADMAP eventualmente moverá los servers a ring 3.

## Capas

```
+----------------------------------------------------------------+
|    ring-3 user tasks (flat blobs + ELF64 ET_EXEC + libc-linked) |
|   /bin/ring3hello /bin/ring3int80 /bin/ring3fault               |
|   /bin/hello_elf  /bin/hello_libc                                |
+----------------------------------------------------------------+
|                 lib/libc — osnos mini-libc                      |
|   stdio (printf/fprintf/snprintf), stdlib (malloc/free/exit),   |
|   string (mem*, str*), unistd (syscall wrappers), crt0          |
|              ↓                                                   |
|             syscall (via inline asm in syscall.h)               |
+----------------------------------------------------------------+
|             syscall ABI Linux x86_64 (rax,rdi,rsi,rdx,r10,r8,r9)|
|       int 0x80 (IDT[0x80] DPL=3)      syscall  (LSTAR=entry)    |
|              int80_entry asm                    syscall_entry   |
|                       \                          /              |
|                        syscall_dispatch(frame)                  |
+----------------------------------------------------------------+
|                       shell_server                              |
|         (line editor, history, command dispatch table)          |
+----------------------------------------------------------------+
| keyboard_server  | console_server  |       fs_server            |
|  (PS/2 -> IPC)   | (IPC -> framebuf)| (vfs -> ramfs/sysfs/...)  |
+----------------------------------------------------------------+
|                          IPC layer                              |
|   ipc_send / ipc_recv / ipc_recv_block                          |
|   single shared queue, IPC_QUEUE_SIZE=64, payload 1024 bytes    |
+----------------------------------------------------------------+
|                       micro/ core                               |
|  task (16 slots), scheduler (cooperative + resume_jump), reaper |
|  ipc, service, fd, extable, uaccess (fault-recoverable)         |
|  syscall, syscall_msr, syscall_entry, int80, task, tss, gdt, idt|
|  pmm, vmm, kmalloc — paging, heap, per-task address spaces      |
+----------------------------------------------------------------+
|                         proc/ layer                             |
|  builtin (registry), exec (proc_exec, proc_exit_current_user),  |
|  elf (Elf64 loader; PT_LOAD -> page-by-page map + zero-fill)    |
+----------------------------------------------------------------+
|                          drivers/                               |
|     keyboard (PS/2, scancode + Shift + Ctrl + arrows)           |
|     framebuffer (linear FB + 8x8 bitmap font)                   |
+----------------------------------------------------------------+
|                       Limine bootloader                         |
+----------------------------------------------------------------+
```

## Flujo de una tecla típica

```
   user types 'l'
        |
        v
   PS/2 controller
        |  scancode 0x26 in port 0x60
        v
   keyboard_server_tick()  <-- polled by scheduler each round
        |
        |  reads scancode, applies shift/ctrl/extended state
        |  builds keyboard_event_t { ascii='l', keycode=0 }
        |
        v
   ipc_send(to=SERVER_SHELL, type=IPC_KEY_EVENT,
            arg0=0, data[0]='l')
        |
        |  --> task_unblock(shell_pid)
        v
   scheduler picks shell next round
        |
        v
   shell_server_tick()
        |  ipc_recv_block -> got 'l'
        |  c = 'l', not '\n', not '\b', not Ctrl+C, not arrow
        |  append to input[], echo to console
        v
   ipc_send(to=SERVER_CONSOLE, type=IPC_CONSOLE_WRITE,
            arg0=color, data="l")
        |
        v
   console_server_tick() drains the message
        |
        v
   framebuffer_draw_string("l", color)
        |
        v
   pixel writes to linear framebuffer
```

## Flujo de un comando: `ls /home`

```
shell receives '\n'
   |
   |  history_save("ls /home"); reset history_pos
   |
   v
run_command("ls /home")
   |
   |  linear scan over commands[] table; match "ls"
   |  cmd_ls("/home")
   |     make_absolute_path -> "/home"
   |     shell_send_fs1(IPC_FS_LIST, "/home")
   |     check_fs(status)
   v
ipc_send(to=SERVER_FS, type=IPC_FS_LIST, data="/home")
   |
   |  shell tick returns; task_unblock(fs_pid)
   v
fs_server_tick() drains queue
   |  ipc_recv -> got IPC_FS_LIST
   |  ramfs_list_dir("/home", response.data, IPC_DATA_SIZE)
   |  set response.arg0 = OSNOS_OK
   |  set response.arg1 = matches
   |
   v
ipc_send(to=SERVER_SHELL, type=IPC_FS_RESPONSE,
         arg0=OK, data="README.TXT\nHELLO.TXT\n")
   |
   |  task_unblock(shell_pid)
   v
shell_server_tick()
   |  ipc_recv -> got IPC_FS_RESPONSE
   |  shell_send_console("\n")
   |  shell_send_console_color(msg.data, yellow)
   |  prompt()
   v
console_server_tick() renders the listing + prompt
```

## Contratos clave

### IPC (`src/micro/ipc.h`)

- Mensajes de tamaño fijo, copiados al `ipc_send`. Sender puede descartar
  su buffer en el momento que retorna.
- Opcodes en rangos numéricos:
  - `0x00–0x0F` sistema (KEY_EVENT, COMMAND_RUN)
  - `0x10–0x1F` consola
  - `0x20–0x3F` fs / vfs (espacio para VFS)
- Convención de respuesta:
  - `arg0` = `osnos_status_t` (0 = OK, >0 = errno-like).
  - `arg1` = tamaño del payload o conteo cuando aplica.
  - `data` = payload textual o binario (null-terminated cuando es texto).
- `ipc_send` retorna `OSNOS_OK` / `OSNOS_EAGAIN` (queue full) /
  `OSNOS_ESRCH` (target no registrado). Callers DEBEN chequear.
- `ipc_recv_block` marca el caller como `TASK_BLOCKED` si no hay mensaje;
  el `task_unblock` desde `ipc_send` lo revive.

### Status codes (`src/include/osnos_status.h`)

Valores numéricos **idénticos** a Linux x86_64 errno (`asm-generic/errno-base.h`,
`errno.h`). Invariante project-wide: cualquier cosa que vaya a cruzar la
frontera user/kernel en el futuro usa estos valores tal cual. No inventar
números en el rango ocupado por Linux; si necesitamos un código sin
equivalente, reservar a partir de 200.

### Key codes (`src/include/osnos_keys.h`)

Subconjunto de Linux `input-event-codes.h`: `KEY_UP=103`, `KEY_DOWN=108`,
`KEY_LEFT=105`, `KEY_RIGHT=106`. Para `/dev/input/*` forwarding futuro sin
traducción.

### Ramfs (`src/fs/ramfs.h`)

- Array plano de `RAMFS_MAX_FILES` slots; cada slot guarda path completo +
  data.
- **Slot ownership**: el índice de un slot no cambia durante su vida. Un
  `const ramfs_file_t *` devuelto por `ramfs_find` queda válido hasta que
  ese mismo slot se borre. Crítico para el futuro FD layer del VFS.
- Borrar = `slot.used = false`. Nunca se compacta el array.
- `ramfs_move` de un directorio renombra los hijos atómicamente o aborta.

### Shell (`src/servers/shell_server.c`)

- Comandos vía tabla `commands[]` con macros `CMD(name, handler, help)` /
  `ALIAS(name, handler)`. El `help` se genera iterando la tabla.
- Senders FS unificados en `shell_send_fs1(type, path)` y
  `shell_send_fs2(type, src, dst)`. Pasan el `osnos_status_t` al caller.
- History: ring buffer de 16 entradas con dedup consecutivo, navegación
  con flechas up/down y scratch line para preservar la línea original.
- Ctrl+C (ASCII 0x03 vía `ctrl_down` en el driver): cancela el input
  actual sin matar nada (no preempción todavía).

## Boot sequence

`kmain` en `src/kernel/main.c`:

1. Validar Limine base revision y framebuffer; `framebuffer_init`.
2. Memoria: `pmm_init` → `vmm_init` → `kheap_init`.
3. CPU: `gdt_init` → `tss_init` → `idt_init` → `uaccess_init` (registra
   el span de copy_*_user en la extable) → `syscall_msr_init` (habilita
   EFER.SCE + programa STAR/LSTAR/FMASK).
4. Microkernel: `ipc_init`, `task_init`, `reaper_init`, `scheduler_init`,
   `syscall_init`, `ramfs_init` (slots vacíos), `bootstrap_fs` (crea
   /home, /sys, /dev, /bin + archivos seed).
5. `task_create` para los 4 servers (estado: `READY`).
6. `service_register` mapea ID -> PID.
7. Llamar `*_init` de cada server. **Después** del registro: el shell init
   manda banner y prompt, que llegan a una consola ya direccionable.
8. `scheduler_loop`: guarda resume point (longjmp host) y entra a
   `for(;;) scheduler_tick()`. Cada tick primero hace `reaper_drain`
   (libera kstacks de tareas muertas, reaja DEAD → UNUSED) y luego
   dispatchea la próxima READY.

## Flujo de un syscall desde ring 3 (`exec /bin/ring3hello`)

```
ring 3:  mov $1,%rax; mov $1,%rdi; lea msg,%rsi; mov $17,%rdx; syscall
            │
            ▼
CPU sets   CS=GDT_KCODE, SS=GDT_KDATA  (de STAR[47:32])
           RCX=user RIP, R11=user RFLAGS, RFLAGS &= ~FMASK
           RIP=LSTAR=syscall_entry
            │
            ▼
syscall_entry asm:
  - movq %rsp, syscall_user_rsp     ; CPU no cambió RSP, RSP=user stack
  - movq tss_kernel_rsp0, %rsp      ; ahora en kernel stack per-task
  - push r11 / rcx                   ; preservar user RIP/RFLAGS
  - push frame (rax,rdi,rsi,rdx,r10,r8,r9)
  - andq $-16, %rsp; call int80_dispatch_wrapper
            │
            ▼
int80_dispatch_wrapper:
  - syscall_dispatch(frame) → sys_write → console IPC
  - console_server_tick() → drena el IPC al framebuffer
  - return retval en rax
            │
            ▼
syscall_entry asm (cont):
  - pop frame, pop rcx/r11
  - movq syscall_user_rsp, %rsp
  - sysretq                          ; CPU: CS=GDT_UCODE, SS=GDT_UDATA
                                     ; RIP=RCX, RFLAGS=R11
            │
            ▼
ring 3:  next instruction después del syscall
```

El path `int 0x80` es equivalente excepto que: (a) el CPU sí pushea
un iret frame (SS, RSP, RFLAGS, CS, RIP) en el kernel stack, así que
`int80_entry` no necesita guardar RSP a mano; (b) sale con `iretq`,
no con `sysretq`.

## Fault recovery (ring 0 y ring 3)

Cualquier vector de excepción pasa primero por `fault_try_recover`
en `src/micro/idt.c`:

1. **Kernel-mode con RIP en extable** → reescribe `frame->rip` al
   recovery label y retorna del handler. El callee "vuelve" con
   `OSNOS_EFAULT` (ej.: `copy_from_user` sobre página unmapped).
2. **User-mode (CPL=3) con un user task vigente** → imprime
   "ring-3 task killed: <vec>" y llama `proc_exit_current_user(139)`
   que destruye el AS, manda IPC_PROC_EXITED y hace
   `sched_resume_jump`. El shell sigue vivo.
3. **Else** → panic clásico (`hcf`).

## Heap del proceso user (sys_brk)

Cada user task tiene su propio "break": dirección virtual baja del
heap. La libc `malloc` se apoya en `sbrk` que se apoya en el syscall
`brk` (Linux #12):

```
                user pml4
  USER_CODE_VIRT (0x400000)  ┌──────────────┐
                             │   PT_LOAD    │
                             │   text+rodata│
                             ├──────────────┤
                             │   PT_LOAD    │
                             │   data+bss   │
                             └──────────────┘

  USER_HEAP_BASE (0x10000000)
    heap_start ────► ┌──────────────┐  ← heap_brk inicial == heap_start
                     │  (vacío)     │
                     │              │  sys_brk(addr > heap_brk) → pmm_alloc + vmm_map
                     │  heap        │  sys_brk(addr < heap_brk) → vmm_unmap + pmm_free
                     │              │
    heap_brk ──────► └──────────────┘

  USER_STACK_VIRT  ┌──────────────┐  ← 4 KiB page, single
    (0x7FFFE000)   │     stack    │  user RSP = 0x7FFFF000
                   └──────────────┘
```

`sys_brk(new)` con `new == 0` reporta el break actual; en otro caso
zero-fillea + mapea (o desmapea) las páginas que correspondan y
actualiza `task.heap_brk`. Si el requested está fuera de rango
(< heap_start o ≥ USER_VIRT_MAX) devuelve el break actual sin tocar
nada — la libc lo detecta y setea `errno = ENOMEM`.

## libc (FASE 7)

`lib/libc/` es una mini-libc local (~700 LOC). Se compila aparte del
kernel con `USER_CFLAGS` (sin `-mcmodel=kernel`), se empaqueta en
`libosnos_c.a` + un `crt0.S.o` standalone, y se linka contra cada ELF
user que la quiera.

```
tests/hello_libc.c
    │
    │ clang USER_CFLAGS -I lib/libc/include
    ▼
hello_libc.o
    │
    │ ld.lld -T tests/hello_libc.lds
    │      crt0.S.o (provides _start) +
    │      hello_libc.o +
    │      libosnos_c.a (printf, malloc, strlen, ...)
    ▼
hello_libc.elf        (ELF64 ET_EXEC, two PT_LOAD)
    │
    │ objcopy -B i386:x86-64 -I binary -O elf64-x86-64
    ▼
hello_libc.elf.o      (symbols _binary_hello_libc_elf_start/end)
    │
    │ kernel link
    ▼
build/kernel          (embeds the bytes)
```

En runtime:

```
exec /bin/hello_libc
   │
   ▼
proc_exec → task_create_user_elf → elf_load(blob,...)
   │  ← maps PT_LOADs, allocates stack page,
   │     sets task.user_entry = 0x400000 (= _start),
   │     task.heap_start = task.heap_brk = 0x10000000
   ▼
scheduler dispatch → user_task_trampoline → iretq → CPL=3
   ↓
_start (crt0.S):
   andq $-16, %rsp
   call main(0, NULL, NULL)
   ↓
main:
   printf("hola %s, %d!\n", "mundo", 7);
        ↓ vfprintf → sink_flush → write(1, buf, n) → syscall #1
   buf = malloc(64);
        ↓ first-fit walks NULL list, sbrk(80) → syscall #12 (brk)
        ↓ kernel maps a heap page at 0x10000000
   strcpy(buf, ...); puts(buf);
   free(buf);
   return 0;
        ↓ crt0 sees rc, calls _exit(0) → syscall #60
        ↓ proc_exit_current_user → AS destroy + IPC_PROC_EXITED + sched_resume_jump
   ↓
shell prompt re-aparece
```

## ELF loader (FASE 28)

`elf_load(blob, size, *pml4, *entry, *stack_top)`:

1. Valida ELF magic / class=64 / little-endian / ET_EXEC / EM_X86_64.
2. Crea address space (`address_space_create`).
3. Para cada `PT_LOAD`:
   - aloca + mapea páginas en `[p_vaddr, p_vaddr + p_memsz)` con
     `PTE_U` (y `PTE_W` si `PF_W`)
   - copia `p_filesz` bytes desde el blob, zero-fill el resto
4. Aloca una página de user stack en `0x7FFFE000-0x7FFFF000`.
5. Devuelve PML4, entry (`e_entry`) y stack top.

`task_create_user_elf` toma el resultado, suma kstack + slot de task,
y pone `t->user_entry / user_stack_top` para que `user_task_trampoline`
arme el iretq frame correcto.

## Limitaciones conscientes

- **Cooperativo**: si un tick entra en loop infinito, el sistema cuelga.
  Ctrl+C tipea pero no rescata. Fix real = timer IRQ + preemption (FASE 9
  del ROADMAP).
- **Cola IPC única compartida**: 64 slots para todo el sistema. Un server
  ruidoso puede llenarla y bloquear a los demás con EAGAIN. Mitigado por
  empaquetar outputs grandes (cmd_help, cmd_history) en un solo IPC. Fix
  real = cola por servidor o backpressure explícito.
- **Sin VFS**: hoy fs_server llama a ramfs_* directo. Esa frontera se va
  a abstraer en FASE 2 del ROADMAP (sketch en `src/fs/vfs.h`).
- **Sin permisos, sin uid/gid, sin atime/mtime**: estado correcto para
  este punto del desarrollo.
