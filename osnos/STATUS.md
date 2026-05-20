## Estado actual

### Boot + drivers
OK boot con Limine + QEMU
OK framebuffer + font bitmap
OK Limine memmap + HHDM requests parseados al boot
OK teclado PS/2 con Shift, punto, mayúsculas
OK teclado: flechas up/down (scancode extendido E0 48/50)
OK teclado: Ctrl tracking + Ctrl+C → ASCII 0x03
OK backspace visual
OK boot sequence: pmm_init -> vmm_init -> kheap_init -> gdt_init -> tss_init
   -> idt_init -> uaccess_init (extable) -> syscall_msr_init (EFER.SCE +
   STAR/LSTAR/FMASK) -> pic_init (remap 8259) -> lapic_init (LINT0=ExtINT
   para q35) -> timer_init (PIT @ 100 Hz) -> ipc_init -> task_init ->
   reaper_init -> scheduler_init -> syscall_init -> ramfs_init ->
   bootstrap_fs -> task_create per server -> service_register ->
   server _init -> sti -> scheduler_loop (longjmp host)

### Microkernel
OK drivers separados (framebuffer, keyboard)
OK servers separados (console, keyboard, shell, fs)
OK IPC con queue de 64 slots, payload 1024B
OK IPC blocking + wakeup por task_unblock
OK IPC contract documentado en ipc.h (rangos de opcode, convención de respuesta)
OK ipc_send retorna osnos_status_t (OK / EAGAIN / ESRCH); shell propaga errores al usuario
OK IPC_PROC_EXITED (rango 0x40) para parent-notification de child death
OK service registry (SERVER_FS/KEYBOARD/SHELL/CONSOLE)
OK scheduler cooperativo round-robin + task.dispatches counter
OK GDT propia (kcode/kdata/ucode/udata + TSS slots)
OK IDT 256 entries con exception handlers (#PF, #GP, #DF, #UD, etc.)
OK TSS instalado (RSP0 al kernel stack), ltr OK, IOPB cerrado
OK PMM bitmap + VMM 4-niveles + kheap free-list + address_space_create/destroy
OK copy_from_user / copy_to_user con fault recovery via extable (FASE 6.3c)
OK syscalls Linux x86_64 con números exactos (read=0, write=1, open=2, close=3,
   fstat=5, lseek=8, brk=12, exit=60, rename=82, mkdir=83, rmdir=84,
   unlink=87, getdents=217, +isatty=201 osnos-specific)
OK syscall ABI dual: int 0x80 (legacy) y syscall (LSTAR fast path),
   mismo dispatcher + misma syscall_frame_t
OK osnos_status_t con valores Linux errno (EPERM=1, ENOENT=2, EIO=5, EEXIST=17,
   EINVAL=22, ENOSPC=28, EROFS=30, ENOTEMPTY=39, etc.)
OK osnos_keys.h con valores Linux input-event-codes (KEY_UP=103, KEY_DOWN=108)
OK osnos_dirent_t layout-compatible con linux_dirent64
OK osnos_stat_t layout-compatible con Linux x86_64 struct stat

### Shell
OK shell interactivo con tabla de comandos + auto-help
OK help / ls / cat / neof / clear / cls (alias) / banner / uname / version / whoami
OK pwd
OK cd
OK relative paths con make_absolute_path() (retorna bool si truncó)
OK ls / ls [PATH]
OK mkdir
OK rmdir (rechaza dir no vacío)
OK tree / tree [PATH] (DFS iterativo con stack explícito)
OK cat FILE
OK touch FILE
OK rm FILE
OK rm PATTERN (wildcard *)
OK cp SRC DST
OK mv SRC DST (atómico ante overflow de nombres)
OK echo "texto"
OK echo "texto" > FILE
OK echo "texto" >> FILE
OK paths tipo /home/readme.txt
OK history (16 entradas, dedupea consecutivos)
OK navegación con flechas up/down sobre history (con scratch line)
OK Ctrl+C → cancela input line + prompt fresco

### RAMFS
OK RAMFS en memoria (32 slots × 128B path × 512B data)
OK dirs explícitas (is_dir flag)
OK move recursivo de directorios (renombra hijos atómicamente)
OK no compacta el array al borrar: punteros a slots permanecen válidos
OK ramfs_iter_child para iteración encapsulada (slot interno no escapa)

### VFS
OK src/fs/vfs.h con contrato: vfs_node_type_t (Linux S_IF* values), vfs_stat_t, vfs_dirent_t, vfs_ops_t, vfs_mount_t
OK src/fs/vfs.c con mount table + longest-prefix dispatch
OK vfs_init / vfs_mount / vfs_stat / vfs_readdir / vfs_read / vfs_write / vfs_append / vfs_mkdir / vfs_rmdir / vfs_unlink
OK vfs_copy / vfs_move (con fast-path rename si el backend lo expone)
OK vfs_touch (stat-then-write-empty)
OK vfs_list_dir / vfs_tree (DFS iterativo, max depth 16) sobre vfs_readdir
OK vfs_path_has_wildcard / vfs_glob_list / vfs_glob_read / vfs_glob_unlink (glob '*' en última componente)
OK src/fs/ramfs_vfs.{c,h}: adapter que expone const vfs_ops_t ramfs_vfs_ops
OK bootstrap_fs: vfs_init + vfs_mount("/", &ramfs_vfs_ops, 0) + vfs_mkdir + vfs_write
OK fs_server migrado: 0 llamadas a ramfs_* directo; todo va por vfs_*
OK wildcard * en ls / cat / rm (vfs_glob_*)

### Build / saneamiento
OK lib/string.c con strlcpy / strlcat / strncmp / strstarts / strchr / strrchr
OK constantes en src/include/osnos_limits.h (OSNOS_PATH_MAX, OSNOS_NAME_MAX, OSNOS_INPUT_MAX)
OK _Static_assert verifica que OSNOS_PATH_MAX*2 + slack <= IPC_DATA_SIZE
OK -Werror para src/ (cc-runtime sigue permisivo)
OK cero warnings en build limpio

---

## SIGUIENTE

### Saneamiento pre-VFS — CERRADO
OK invariantes documentados (ramfs.h, ARCH.md, CLAUDE.md refresh)
OK mount points extraídos a src/fs/bootstrap.{c,h}
OK osnos_path_t definido en src/include/osnos_path.h (skeleton)

### FASE 2 — VFS real — CERRADA
OK 11a. VFS dispatch + mount table (vfs.c)
OK 11b. Adapter ramfs como backend (ramfs_vfs.{c,h})
OK 11c. fs_server consume vfs_*, no llama ramfs_* directo
OK 11d.1 sysfs read-only en /sys: version, tasks, mounts (synthetic)
OK 11d.2 devfs en /dev: /dev/null, /dev/zero (char devices, mode 0666)
OK 12. vfs_stat_t con type/size/inode/mode (atime/mtime/uid pendientes hasta FASE 9: clock)
OK 13. permisos en stat (0755 dirs ramfs, 0644 files ramfs, 0444 sysfs, 0666 chr devfs) — expuestos sin enforcement
OK 14. mount table interna (VFS_MAX_MOUNTS=8, longest-prefix dispatch)

### Comando test integrado
OK ~217 asserts cubriendo:
   - ramfs CRUD + dirs + globs
   - sysfs / devfs / binfs (RO, EROFS en writes)
   - stat modes (0755/0644/0444/0555/0666)
   - syscalls: open/read/write/close/lseek/fstat/isatty/exit
                mkdir/rmdir/unlink/rename/getdents
   - fd table: limites, EBADF, EMFILE, EISDIR, ENOTDIR
   - stdin ring buffer
   - PMM: alloc/free/reuso/round-trip via HHDM
   - VMM: kernel_pml4, map/unmap/lookup, AS create/destroy con free completo
   - kmalloc / kfree / coalesce / batch
   - copy_from_user / copy_to_user (validación de rango)
   - GDT: CS=0x08, GDTR/IDTR matchean, TR=0x28
   - builtins registrados (/bin/cat/mkdir/rmdir/rm/touch/mv/cp/ls/...)
   chunked-flush para que no se desborde la cola IPC al imprimir

### FASE 3 — Introspección — CERRADA
OK 15. ps (lee /sys/tasks vía vfs_read; sin atajos al kernel)
OK 16. /bin/top — viewer live (cerrado tras FASE 9.4 + ANSI). Ver detalle
   en la entrada de FASE 9.4 abajo.
OK 17. mem info (/sys/meminfo: tasks, ipc, ramfs, mounts used/total) + comando mem
OK 18. /sys/tasks (pid, name, state, dispatches contador)
OK 19. /sys/version (synthetic en sysfs)
OK extra: comando mount (lee /sys/mounts)
OK extra: scheduler_get_ticks() + task.dispatches contador instrumentado
OK extra: /sys/uptime (segundos.ms reales desde FASE 9.1; antes era
   proxy de scheduler ticks)
OK extra: /sys/cpuinfo (CPUID vendor + brand vía leaves 0 y 0x80000002-4)
OK extra: /sys/services (id -> name -> pid)
OK extra: /sys/build (__DATE__ + __TIME__ + __clang_version__)

### FASE 4 — Syscalls — CERRADA
OK 4.1 fd table (stdin/stdout/stderr + fd>=3, offset, flags, is_dir)
OK 4.2 sys_write (stdout/stderr -> console IPC; fd>=3 -> vfs_append)
OK 4.3 sys_open / sys_close (O_CREAT, O_TRUNC, O_EXCL, O_APPEND, O_RDONLY/WRONLY/RDWR)
OK 4.4 sys_read (con offset propio del fd, EOF al final)
OK 4.5 sys_fstat (osnos_stat_t layout-compatible con Linux x86_64 struct stat)
OK 4.5 sys_lseek (SEEK_SET / SEEK_CUR / SEEK_END)
OK 4.5 sys_isatty (1 para 0/1/2)
OK osnos_fcntl.h / osnos_stat.h con valores Linux exactos
OK syscall_dispatch frame-based (entry point para ring3 futuro)
OK sys_read sobre stdin (ring buffer 256B, keyboard_server pushea printables)
OK sys_exit conectado al task lifecycle (marca current task DEAD)
OK sys_mkdir / sys_rmdir / sys_unlink / sys_rename (wrappers VFS)
OK sys_getdents (drena vfs_readdir, layout linux_dirent64)
OK sys_open de directorios en O_RDONLY (read directo -> EISDIR; usar getdents)
TODO write con offset real (hoy todo write es append, OK para fopen("w"/"a"))
TODO errno exportado a userland (los syscalls ya devuelven -errno Linux; ring3 lo va a recibir tal cual)

### FASE 4.5 — Memory management — CERRADA (excepto growth + fault handler)
Pre-requisito de FASE 6 (ring3) y FASE 7 (libc).

OK physical memory manager
  - parsea limine_memmap_request + hhdm_request
  - bitmap (1 bit/página de 4KB) en la primera región USABLE grande
  - pmm_alloc_page / pmm_free_page con hint O(1) amortizado
  - /sys/meminfo expone mem total/used/free kB
OK virtual memory manager + paging propio
  - clona el PML4 de Limine en kernel_pml4 + switch CR3 (control de paging)
  - vmm_map / vmm_unmap / vmm_lookup walk de 4 niveles (PML4 -> PDPT -> PD -> PT)
  - intermediate tables P|W|U allocadas on-demand desde PMM
  - invlpg después de cada modificación
  - /sys/meminfo expone pml4 entries used/total
OK heap kernel real
  - 16 páginas iniciales (64 KB) mapeadas en KHEAP_VIRT_BASE = 0xffffc00000000000
  - free-list singly-linked first-fit con split + coalesce con next/prev
  - kmalloc 8-byte aligned; kfree(NULL) y kmalloc(0) edge cases
  - /sys/meminfo expone kheap total/used B
  - TODO growth: hoy si te quedás sin 64KB, kmalloc devuelve NULL
OK address spaces por proceso
  - address_space_create: clona high-half de kernel_pml4, low-half vacío
  - address_space_destroy: walk low half + free user pages + intermediate tables + PML4
  - kernel siempre visible (high half compartido por todas las AS)
  - task_t.pml4 (NULL = usa kernel_pml4); switch CR3 viene en FASE 6 con ring3
OK copy_from_user / copy_to_user (skeleton)
  - validan rango: user pointer debe quedar por debajo de OSNOS_USER_VIRT_MAX = 0x0000800000000000
  - hoy son memcpy directo; falla bonita ante unmapped page necesita fault handler (FASE 6)
TODO reemplazar arrays estáticos en ramfs/ipc/fd con allocations dinámicas
TODO page fault handler con extable (para que copy_*_user no triple-faulte)

(SMAP/SMEP, KPTI, NX everywhere, COW, ASLR -> ROADMAP_APENDICE.md)

### FASE 5 — Userland (mínima, todavía ring0) — CERRADA
OK 24. pseudo-userland: tasks que corren un trampoline + builtin_main
OK 25. builtins built-in en /bin (binfs synthetic RO; hello, echo, true, false, init)
OK 26. exec interno: proc_exec("/bin/PROG", "args") + cmd_exec en shell
OK 27. mini init: builtin /bin/init exposed (kmain sigue siendo el init real)
OK IPC_PROC_EXITED desde la trampoline -> shell defiere prompt hasta que el child termina
OK builtin /bin/cat (open + read loop + write + close — ejercita la syscall ABI completa)

OK ABI de filesystem completa para builtins:
   - sys_mkdir / sys_rmdir / sys_unlink / sys_rename (Linux numbers: 83/84/87/82)
   - sys_getdents (Linux 217) con osnos_dirent_t layout-compatible con linux_dirent64
   - sys_open de directorios en modo RDONLY (read directo -> EISDIR; usar getdents)
   - fd.is_dir flag para distinguir
   - 7 builtins nuevos: /bin/mkdir /bin/rmdir /bin/rm /bin/touch /bin/mv /bin/cp /bin/ls

PENDIENTE (opcional) migrar handlers del shell (cmd_ls/cmd_mkdir/etc) a
  forwarders de proc_exec. Hoy el shell tiene fast-path IPC_FS_* directo;
  FASE 7.5 migró el lado /bin/* a ELF + libc pero el shell sigue usando
  su path nativo. Reemplazar `cmd_ls(args)` por `proc_exec("/bin/ls",args)`
  unifica codepath pero cuesta el spawn de un task por comando. Útil
  cuando llegue FASE 10 (servers a userspace) — antes no se nota.

### FASE 6 — ELF + ring3 — CERRADA
OK 29a. GDT propia: null, kcode (0x08), kdata (0x10), ucode (0x18|3), udata (0x20|3)
       reload con far return; ds/es/ss/fs/gs apuntando a kdata
OK 29b. IDT con 256 entries (4 KiB), gates de interrupción long-mode (type 0x8E)
       handlers en clang __attribute__((interrupt))
       generic catch-all + específicos para #PF (con CR2), #GP, #DF, #UD, etc.
       panic header al framebuffer en rojo, hlt (ya no triple-fault)
       /sys/meminfo expone exceptions count
OK 29c. TSS instalado (16-byte descriptor en GDT slots 5-6, selector 0x28)
       RSP0 apunta a kernel_stack[16384] alineado a 16
       IOPB offset = sizeof(tss) (sin permisos de I/O desde ring 3)
       ltr ejecutado; tr_now == 0x28 verificado
       tss_set_rsp0 listo para per-task switching en FASE 6.3
OK 6.3a primer salto a ring 3 (comando `ring3` one-shot) — VERIFICADO en QEMU
   - address_space_create + 2 user pages (code RX, stack RW)
   - kstack 16KB vía kmalloc -> tss_set_rsp0
   - iretq frame (SS=0x23, CS=0x1b, RFLAGS=0x202) -> CPL=3
   - int3 (0xCC) llena la code page; trap inmediato (rip=0x400001 = RIP+1 porque int3 es trap)
   - exc_3 handler imprime "from USER mode (ring 3)" + rip/cs/rsp -> hcf
   - IDT[3] y IDT[4] con DPL=3 (int3/into callable desde ring 3, convención Linux)
   - Validado: GDT user selectors / TSS.RSP0 / IDT desde user / AS user-kernel split

OK 6.4a syscall ABI desde ring 3 vía `int 0x80` (Linux legacy ABI) — VERIFICADO en QEMU
   - IDT[0x80] DPL=3 → handler `int80_entry` (file-scope asm en src/micro/int80.c)
   - stub pushea syscall_frame_t (rax, rdi, rsi, rdx, r10, r8, r9) en orden del struct
   - alinea stack a 16 (rbp save), llama int80_dispatch_wrapper(frame)
   - wrapper llama syscall_dispatch + console_server_tick (drain stdout antes de iretq)
   - retval del syscall sobreescribe rax saved; restore + iretq → user ve resultado en rax
   - registros caller-saved no salvados (RCX/R11), igual que SYSCALL → user-stub portable
   - User stub `user_hello_start..end`: write(1,"hello from ring3\n",17) + exit(0)
   - Comando `ring3hello` arma el AS, entra a ring 3, imprime desde CPL=3 vía VFS,
     vuelve al shell limpio (gracias a 6.3b.1)

OK 6.3b.1 scheduler resume primitive (setjmp/longjmp ad-hoc) — VERIFICADO en QEMU
   - scheduler_loop() es el for(;;) eterno con resume point guardado adentro
     (savea RIP via lea label, RSP, RBP, RBX, R12-R15). Función no retorna nunca
     para que el frame se mantenga vivo en el stack
   - sched_resume_jump() restaura CR3 al kernel_pml4, resetea RUNNING -> READY,
     restaura callee-saved + RSP + RIP con jmpq *RIP
   - sys_exit_to_scheduler flag: armado por demos ring3, sys_exit lo chequea
     y llama sched_resume_jump() (nunca retorna) si está set
   - cmd_ring3hello round-trip funcional: user mode → int 0x80 sys_write →
     int 0x80 sys_exit → longjmp → shell prompt sin reboot
   - Pieza fundacional: cuando 6.3b lleguen user tasks reales, mismo mecanismo
     se reusa para task lifecycle (task DEAD via fault o sys_exit)

OK 6.3b user tasks integrados al scheduler de primera
   - task_t.kernel_stack_top (16 KiB via kmalloc), pml4 != NULL marca "user task"
   - builtin_t extended: kernel builtin (main fn) o user builtin (code blob)
   - task_create_user en proc/exec.c: AS + code page (PTE_U) + stack page (PTE_W|PTE_U)
     + kstack heap + task slot con user_task_trampoline
   - user_task_trampoline: tss_set_rsp0 + CR3 swap + iretq al user entry (0x400000)
   - proc_exec ahora dispatcha kernel vs user builtin segun los campos del registry
   - sys_exit detecta user task (t->pml4 != NULL): switch CR3 a kernel, address_space_destroy,
     manda IPC_PROC_EXITED al shell, sched_resume_jump → scheduler corre siguiente task
   - /bin/ring3hello como user builtin: exec /bin/ring3hello imprime "hello from ring3"
     y vuelve al shell vía el flujo normal proc_exec / IPC_PROC_EXITED (mismo path que /bin/hello)
   - Memory leak conocido: el kstack del user task no se libera (estamos corriendo sobre él);
     reaper de DEAD tasks lo limpia (TODO de cleanup, no funcional)

OK 6.3c page fault handler con extable — VERIFICADO en QEMU
   - src/micro/extable.{c,h}: tabla {rip_start, rip_end, recovery_rip}
     con extable_register / extable_lookup (linear scan, max 16 entries)
   - src/micro/uaccess.c reescrito: __uaccess_copy_bytes en file-scope asm,
     sin prólogo, RSP intacta — fault → RIP redirect a __uaccess_copy_bytes_fault
     que retorna EFAULT con `ret` normal
   - uaccess_init() registra el span [start, end) → fault en kmain
   - idt.c fault_try_recover(vec, frame, err):
     (a) kernel CPL=0 + RIP en extable → frame->rip = recovery, iretq → caller
         "vuelve" con EFAULT
     (b) CPL=3 + t->pml4 != 0 → print "ring-3 task killed", llama
         proc_exit_current_user(128+11) — never returns
     (c) else → panic + hcf como antes
   - VECTOR_NO_ERROR/VECTOR_WITH_ERROR macros llaman fault_try_recover primero
   - exc_pf también
   - proc_exit_current_user en proc/exec.c: factoreó el user-task cleanup
     de sys_exit; usado tanto por sys_exit como por los fault handlers
   - 2 tests nuevos en cmd_test: copy_from_user / copy_to_user contra
     página unmapped en low half (0x10000) → EFAULT vía extable
   - /bin/ring3fault user builtin: prints announce + deref 0xdead;
     el shell sobrevive y sigue con prompt
OK 6.3d reaper de DEAD tasks
   - src/micro/reaper.{c,h}: cola de 16 kstacks pendientes + task_reap_dead
   - task_t.kernel_stack_base guardado al crear el user task; pasado al
     reaper en proc_exit_current_user (sys_exit y fault path)
   - scheduler_tick llama reaper_drain al inicio: kfree de cada kstack
     pendiente + reset de slots DEAD → UNUSED (ps/ /sys/tasks limpios)
   - /sys/meminfo expone "reaped tasks" y "reaper leaks" counters
   - test: N exec /bin/ring3hello sin crecimiento monotónico de kheap
OK 6.4b SYSCALL/SYSRET MSRs (STAR, LSTAR, FMASK) — verificado en QEMU
   - GDT reorganizado: udata 0x18 antes que ucode 0x20 (SYSRET ordering)
   - src/micro/syscall_msr.{c,h}: habilita EFER.SCE + escribe STAR/LSTAR/FMASK
     - STAR  = (GDT_KDATA << 48) | (GDT_KCODE << 32)
     - LSTAR = syscall_entry (asm en src/micro/syscall_entry.c)
     - FMASK = 0x40700  (clears TF | IF | DF | AC en entry)
   - syscall_entry stub asm: stash user RSP → swap a tss_kernel_rsp0 →
     push syscall_frame_t + rcx/r11 (user RIP/RFLAGS) → call int80_dispatch_wrapper
     → pops + restore + sysretq
   - tss_kernel_rsp0 mirror exportado por tss.c, sincronizado en tss_set_rsp0
   - /bin/ring3hello migrado a syscall; /bin/ring3int80 mantiene legacy int 0x80
     (mismo dispatcher, ambos paths probados)
   - 3 asserts nuevos en cmd_test: EFER.SCE, STAR[47:32], STAR[63:48]

OK 28 ELF64 loader simple — verificado en QEMU
   - src/include/osnos_elf.h: Elf64_Ehdr / Elf64_Phdr + constantes
     (ET_EXEC, EM_X86_64, PT_LOAD, PF_X/W/R) layout Linux-compatible
   - src/proc/elf.c: elf_load(blob, size, *pml4, *entry, *stack_top)
     - valida magic, class=64, le, ET_EXEC, EM_X86_64
     - itera PT_LOAD, aloca + mapea por página, copia p_filesz, zero-fill p_memsz
     - aplica PTE_W según PF_W; PTE_U siempre (user-accessible)
     - aloca stack page en 0x7FFFE000-0x7FFFF000
   - src/proc/exec.c: task_create_user_elf flavor + proc_exec dispatcher
     selecciona kernel/userblob/userelf según campos del builtin_t
   - user_task_trampoline parametrizado via task_t.user_entry/user_stack_top
   - Makefile builds tests/user_hello.c → tests/user_hello.elf →
     embed via objcopy con sección .rodata; expone _binary_user_hello_elf_*
   - /bin/hello_elf = primer ELF de verdad que corre con e_entry = 0x400000
   - 5 asserts nuevos en cmd_test: magic, truncated, hello_elf load + entry + stack

### FASE 7 — libc — CERRADA (mini-libc local)
OK SYS_BRK = 12 (Linux number) en syscall.h + sys_brk en syscall.c
   - task_t.heap_start / heap_brk; USER_HEAP_BASE = 0x10000000
   - grow: pmm_alloc + vmm_map (PTE_W|PTE_U) zero-filled; rollback on OOM
   - shrink: vmm_unmap + pmm_free pages above new_brk
   - brk(0) query; out-of-range refusal returns current break
   - 8 asserts nuevos en cmd_test (query, grow 1 page, grow 2 pages,
     shrink, refused below heap_start, refused into kernel)
OK lib/libc/ — mini-libc local linked into user ELFs
   - include/: stdio.h, stdlib.h, string.h, unistd.h, fcntl.h,
     errno.h, sys/{types,stat}.h
   - crt0.S: _start → main(0, NULL, NULL) → _exit(rc)
   - syscall.h: osnos_syscall0..4 inline helpers
   - unistd.c: read/write/open/close/lseek/isatty/fstat/mkdir/rmdir
     /unlink/rename/brk/sbrk/_exit; errno wrappers
   - string.c: strlen/strcmp/strncmp/strcpy/strncpy/strcat/strncat
     /strchr/strrchr/memcpy/memmove/memset/memcmp/memchr
   - stdlib.c: malloc/free/calloc/realloc (sbrk + first-fit free list,
     no split/merge), exit, abort, atoi, strtol
   - stdio.c: printf/vprintf/fprintf/vfprintf/snprintf/vsnprintf/puts
     /putchar — supports %d %i %u %x %X %o %c %s %p %% + flags
     (-,0,+,space), width, length (l, ll, z)
   - errno.c: global int errno
   - Build: libosnos_c.a (ar) + crt0.S.o separate; linker script
     places .text first then crt0 then main + libc
OK toolchain Makefile
   - USER_ELF_SRCS (bare) y USER_ELF_LIBC_SRCS (con libc) separados
   - pattern rule específico para user_hello.elf (sin libc); pattern
     genérico para *.elf con libc + crt0
OK /bin/hello_libc — primer ELF que usa libc: printf con varargs +
   malloc/strcpy/puts/free + formatos %x %o %d %u %p %s

OK FASE 7.6: /bin/calc + /bin/osh — primeros programas de verdad
   - /bin/calc: evaluador de expresiones aritméticas enteras (signed 64-bit).
     Lexer + recursive-descent parser inline; soporta + - * / % ()
     y unary minus con precedencia correcta. Concatena argv[1..] y
     evalúa: `calc 2 + 3 \* 4` → 14, `calc "(7-3)*(2+8)"` → 40.
   - /bin/osh: mini script interpreter (single-pass eval, sin AST):
     - Variables int64 (max 32, nombres ≤15 chars), `x = expr`
     - Operadores: + - * / %, ==/!=/</>/<=/>=, && || !, parens
     - Statements: print, if/else, while, expression-as-stmt
     - Strings: "...", usadas sólo en print (sin escapes por ahora)
     - Comentarios `#` hasta fin de línea
     - Modos: `osh FILE.osh` o `osh -e "code"`; lee hasta 8 KiB
     - Skipping ramas no tomadas y loops: re-parsea con flag
       exec_enabled para consumir tokens sin side effects
     - Cap de 1M iteraciones en while (anti-loop infinito)
   - bugfix osh.run(): el loop top-level no llamaba skip_stmt_term, así
     que un trailing ';' (parse_stmt early-return) generaba infinite
     loop. Agregado skip_stmt_term + skip_blank_nl al inicio de cada
     iteración (mismo patrón que parse_block).
   - bugfix tokenizer kernel (build_argv_block): ahora honra "..." y
     '...' como token único conservando los espacios internos. Antes
     `exec /bin/osh "x = 5; print x"` se rompía en pedazos y osh
     interpretaba `"x` como nombre de archivo.
   - bugfix shell cmd_echo: strippea comillas exteriores (matching " o
     ') tanto del contenido como del filename. `echo "a b" > foo`
     ahora escribe `a b\n` en `foo`, no `"a b"\n`.

OK FASE 7.5: argv passing + migración de tools a libc ELF
   - build_argv_block en proc/exec.c: tokeniza args y arma el bloque
     System V x86_64 (argc, argv[], NULL, NULL_envp, strings) en el
     top del stack page user, escribiendo via HHDM
   - task.user_stack_top apunta a argc; crt0.S lee argc/argv/envp
     desde (rsp)/8(rsp)/16+8*argc(rsp) y los pasa a main
   - libc additions: dirent.h con opendir/readdir/closedir (envuelven
     SYS_GETDENTS); ctype.h (isdigit/isspace/etc.); assert.h
   - tests/libc.lds — linker script compartido por todos los ELFs
     libc-linked (text+rodata @ 0x400000 R+X, data+bss @ 0x401000 R+W)
   - Migración: 13 tools movidas a tests/<n>.c (cada una ~30 LOC):
     hello, echo, true, false, init, cat, touch, mkdir, rmdir, rm,
     mv, cp, ls
   - src/proc/builtin.c: removido KERN macro y todos los bn_*;
     builtin_t pierde el campo main; sólo USER (asm) + USERELF (ELF)
   - src/proc/exec.c: removido builtin_trampoline y la rama
     "kernel-mode builtin"; task_t pierde priv/args (cleanup)
   - USER_CFLAGS gana -ffunction-sections -fdata-sections; USER_LDFLAGS
     gana --gc-sections (los tools simples como true/false bajan a 13 KB)
   - bugfix: struct raw_dirent en lib/libc/dirent.c marcado __packed__
     (sin packed, sizeof daba 24 por trailing padding mientras el kernel
     emite los records con prefijo de 19 bytes; resultaba en nombres
     cortados en la última letra). Ahora opendir/readdir devuelven los
     d_name completos.

OK FASE 7.7 — libc Tier 1 (pre-networking) — VERIFICADO en QEMU
   42 PASS / 0 FAIL via `exec /bin/libctest`. Pensado para tener la
   superficie de POSIX lista para cuando aterrice networking en FASE
   8.5 (post-FAT) — los slots están reservados pero los syscalls
   correspondientes devuelven ENOSYS hasta entonces.

   FILE * layer (lib/libc/stdio.{c,h})
   - struct __osnos_file con fd, single shared read/write buffer,
     dir state, sticky err/eof, pushback (ungetc).
   - stdin/stdout/stderr pasaron de macros enteras a FILE *const
     globales. Static FILE structs en .data; defbuf de BUFSIZ=512
     interno, sin malloc al boot.
     - stdout/stdin → _IOLBF (line buffered)
     - stderr       → _IONBF (unbuffered, flush per syscall)
   - fopen("r"/"w"/"a"/"+", optional 'b' ignored); freopen; fclose
     (auto-fflush + free + close salvo std{in,out,err})
   - fread / fwrite con direction switch (drop_read con lseek-back
     o drain_write antes del flip)
   - fgetc / getchar / fputc / putchar / fgets / fputs / puts / ungetc
   - fseek / ftell / rewind (ftell ajusta por bufferización + pushback)
   - feof / ferror / clearerr / fileno
   - setvbuf / setbuf con sustitución del buffer (free + adopt)
   - perror, remove (alias de unlink)
   - printf engine reescrito para flowear vía do_write(FILE*) en lugar
     de write(fd) directo → line-buffered stdout se beneficia del
     newline-flush automáticamente. snprintf/vsprintf inalterados
     (sink en memoria). vsprintf agregado (delega a vsnprintf con
     cap=SIZE_MAX/2).

   string.h extensions (lib/libc/string.c)
   - memrchr
   - strnlen, strdup, strndup
   - strstr, strpbrk, strspn, strcspn
   - strtok + strtok_r (static state en strtok)
   - strcasecmp, strncasecmp (ASCII-only)
   - strerror con tabla manual (~20 códigos Linux) + fallback
     "errno=N" para desconocidos

   stdlib.h extensions (lib/libc/stdlib.c)
   - atol, atoll, strtoll, strtoul, strtoull (todos via parser
     core parse_uint compartido con strtol)
   - abs / labs / llabs
   - div / ldiv / lldiv (structs con quot+rem)
   - qsort: insertion sort hasta N=16, Lomuto quicksort más allá
   - bsearch
   - getenv / setenv / unsetenv stubs (sin envp wiring; vuelven
     0 / NULL hasta que kernel pase envp)
   - atexit (32 slots, LIFO); exit() ahora corre handlers +
     fflush(stdout) + fflush(stderr) antes de _exit

   setjmp.h + setjmp.S
   - jmp_buf como uint64_t[8]: rbx, rbp, r12, r13, r14, r15, rsp, rip
   - setjmp/longjmp en asm puro (callee-saved + rsp+rip restore,
     longjmp(env,0) devuelve 1 per spec)
   - sigsetjmp/siglongjmp aliasean a las anteriores (no signal
     mask todavía)

   inttypes.h — PRI*/SCN* macros para int{8,16,32,64,MAX,PTR}_t
   en {d,i,u,o,x,X}. limits.h, stdint.h, stdbool.h ya venían vía
   freestnd-c-hdrs (no necesitamos shadow).

   endian.h
   - LITTLE_ENDIAN / BIG_ENDIAN / PDP_ENDIAN constants
   - BYTE_ORDER = LITTLE_ENDIAN (x86_64)
   - htobe{16,32,64} / be{16,32,64}toh via __builtin_bswap*
   - htole / letoh* como identity casts

   arpa/inet.h + netinet/in.h
   - struct in_addr / sockaddr_in (layout Linux exacto)
   - struct in6_addr / sockaddr_in6 skeleton
   - INADDR_ANY / LOOPBACK / BROADCAST / NONE
   - htons / htonl / ntohs / ntohl (alias de htobe*/be*toh)
   - inet_aton, inet_addr, inet_ntoa
   - inet_pton, inet_ntop (AF_INET; AF_INET6 → -1 + EAFNOSUPPORT)
   - INET_ADDRSTRLEN=16, INET6_ADDRSTRLEN=46

   sys/socket.h — superficie POSIX completa con todas las funciones
   stubeadas a -1 + ENOSYS hasta que aterrice el stack de red:
   - AF_{UNSPEC,UNIX,LOCAL,INET,INET6,PACKET} + PF_* aliases
   - SOCK_{STREAM,DGRAM,RAW,SEQPACKET} + flags (CLOEXEC, NONBLOCK)
   - IPPROTO_{IP,ICMP,TCP,UDP}
   - SOL_SOCKET, SO_* options (REUSEADDR, KEEPALIVE, BROADCAST,
     LINGER, REUSEPORT, etc.)
   - MSG_{OOB,PEEK,DONTROUTE,DONTWAIT,WAITALL,NOSIGNAL}
   - SHUT_{RD,WR,RDWR}
   - struct sockaddr, sockaddr_storage, iovec, msghdr
   - socket, bind, listen, accept, connect, send, recv, sendto,
     recvfrom, sendmsg, recvmsg, shutdown, getsockname,
     getpeername, getsockopt, setsockopt
   - lib/libc/inet.c funnels todas a errno=ENOSYS, return -1

   errno.h extensions
   - ENOSYS=38, ELOOP=40
   - Networking placeholders: ENOTSOCK=88, EPROTONOSUPPORT=93,
     EAFNOSUPPORT=97, EADDRINUSE=98, EADDRNOTAVAIL=99, ENETDOWN=100,
     ECONNRESET=104, ETIMEDOUT=110, ECONNREFUSED=111

   tests/libctest.c — smoke test cubriendo:
   - FILE * round-trip (fopen w+r, fwrite, fread, fgets, ungetc,
     rewind, ftell, fclose)
   - string: strdup, strstr, strtok_r (skipped delimiters),
     strcasecmp, strncasecmp, strnlen, strerror
   - stdlib: qsort 10 ints + bsearch hit/miss, strtoul 0xFF,
     strtoll negativo, abs / labs
   - setjmp / longjmp (longjmp con val=42)
   - byte-swap: htons, htonl, ntohs roundtrip, htobe64
   - inet_aton (válido, junk, octet > 255), inet_ntop loopback,
     inet_pton AF_INET, inet_pton AF_INET6 → ENOSYS-style EAFNOSUPPORT
   - socket(AF_INET, SOCK_STREAM, 0) → -1 + errno=ENOSYS
   42 PASS / 0 FAIL en QEMU.

### FASE 8 — Disco real
35. block driver (ATA PIO o VirtIO sobre el sd.img que ya tenemos)
36. simplefs o FAT
37. persistencia real
38. fsck simple

### FASE 9 — Scheduler real — CERRADA
OK 9.1 timer IRQ infra — VERIFICADO en QEMU
   - src/drivers/pic.{c,h}: 8259 remap (master 0x20-0x27, slave 0x28-0x2F),
     mask all on init, pic_unmask/mask/send_eoi
   - src/drivers/lapic.{c,h}: necesario en chipsets modernos (q35).
     El 8259 INTR no va directo al CPU, pasa por LAPIC.LINT0. Habilita
     LAPIC software-enable (SVR bit 8) y configura LINT0 en modo
     ExtINT (delivery 7) para passthrough del 8259. LINT1=NMI.
     Mapea fisica del LAPIC (0xFEE00000) en kernel_pml4 con PTE_PCD
     (cache disabled, obligatorio para MMIO) — Limine HHDM cubre RAM
     pero no MMIO ranges.
   - src/micro/timer.{c,h}: PIT @ 100 Hz (divisor 11931, modo 3
     square wave), handler clang __attribute__((interrupt)) con
     ticks_counter++ + pic_send_eoi(0). timer_ticks/ms/irqs.
   - idt.c: expone idt_set_handler(vec, handler, dpl) para registrar
     IRQs runtime después de idt_init.
   - kmain agrega pic_init + lapic_init + timer_init + sti final
   - /sys/uptime ahora muestra segundos reales (sec.ms) + timer ticks
     + scheduler ticks
   - /sys/timer nuevo: hz, ticks, ms, irqs
   - 11 asserts nuevos en cmd_test: IF=1, PIC IMR (no fully masked,
     IRQ 0 unmasked), IDT[0x20] offset != 0 + present + interrupt
     gate + kernel CS, int $0x20 invoca handler, ticks > 0, irqs > 0,
     ms > 0, ticks advance during busy-wait
   - Diagnóstico al pie: PIC IRR/ISR/IMR + ticks post-busy-wait
OK 9.2 SYS_NANOSLEEP (kernel-mode hlt-loop) — superado por 9.3
   - quedaba como hlt-loop solo en la rama kernel-mode (tests).
   - el resto migró a suspend real con context save/restore (9.3).

OK 9.3a — Voluntary suspension via saved state — VERIFICADO en QEMU
   - syscall_frame_t extendido a 15 GPRs (rax, rbx, rcx, rdx, rsi, rdi,
     rbp, r8..r15) — los stubs ahora capturan TODO el estado user
   - int80_entry: pushea + popea los 15 en orden de struct; usa
     r12 como saved-rsp para alinear el call de C (rbp ya está
     guardado en el frame, no se puede reusar para alignment)
   - syscall_entry: ahora sintetiza un iret frame ANTES del frame
     GPR (ss=0x1b, rsp=syscall_user_rsp, rflags=r11, cs=0x23,
     rip=rcx) y usa iretq en lugar de sysretq. Unifica la disposición
     de stack con int 0x80 — sys_nanosleep lee iret frame en el
     mismo offset (kstack_top - 40) en ambos paths
   - task_t crece con saved_user state: saved_valid, saved_return,
     wakeup_at_ms, saved_iret_{rip,cs,rflags,rsp,ss}, saved_<15 GPRs>
   - sys_nanosleep ahora hace SUSPEND real cuando lo llama un user task:
     copia iret frame (kstack_top - 40) + syscall_frame (kstack_top -
     160) en saved_*, marca BLOCKED + wakeup_at_ms = timer_ms() + ms,
     llama sched_resume_jump (kernel-mode caller sigue con hlt-loop
     fallback para los tests internos)
   - scheduler_tick llama task_check_wakeups(timer_ms()) al inicio
     de cada iteración; cualquier BLOCKED con wakeup_at_ms <= now_ms
     pasa a READY (wakeup_at_ms = 0 indica "no es sleep, no tocar")
   - user_task_trampoline ahora detecta t->saved_valid y reruta a
     user_task_resume que: tss_set_rsp0 + CR3 swap + empaqueta los 20
     valores en buf[] local + asm que pushea iret frame y restaura
     los 15 GPRs vía %r15 (cargado último para no clobberear) + iretq
   - task_clear (helper nuevo en task.c) limpia el slot byte-a-byte
     en task_init/task_create/task_reap_dead — más simple y resiste
     que task_t crezca

OK 9.3 bugfix CRITICO: sti en sched_resume_jump
   - sched_resume_jump se llama desde sys_exit / sys_nanosleep /
     fault_try_recover, todos contextos con IF=0 (FMASK o interrupt
     gate ya limpiaron IF al entrar al kernel)
   - El longjmp restauraba RSP/RBP/RBX/R12-R15/RIP pero NO RFLAGS,
     así que el scheduler_loop reentraba con IF=0 → IRQs hardware
     (timer incluido) se quedaban en la IRR del PIC sin entregar
   - Síntoma visible: tras un /bin/sleep N, timer_ms apenas avanzaba
     (un tick por varios segundos de wall time) porque el kernel
     bucleaba con IRQs masked
   - Fix: `__asm__ volatile ("sti")` justo antes del longjmp asm

OK 9.3 diagnósticos
   - task_wakeups_fired() counter
   - /sys/timer incluye "wakeups fired" + lista de BLOCKED tasks con
     pid + wakeup_at_ms + saved_valid (útil para debug de sleep)

OK 9.3b — preempción timer-driven (CPL=3 only) — VERIFICADO en QEMU
   - timer_irq handler reescrito: clang __attribute__((interrupt))
     reemplazado por asm stub (timer_entry) + C handler (timer_handle)
     en two-stage (mismo patrón que int80_entry / syscall_entry)
   - asm timer_entry pushea los 15 GPRs igual que int80, calls
     timer_handle(frame), pops + iretq
   - timer_handle decide preempt:
       (a) ticks++, EOI siempre
       (b) lee iret frame en frame + sizeof(*frame)
       (c) si CS != GDT_UCODE → kernel mode, no preempt (reset
           preempt_countdown)
       (d) cuenta hasta PREEMPT_QUANTUM = 5 ticks (50ms slice)
       (e) preempt: snapshot iret + 15 GPRs en task.saved_*,
           marca state=READY (no BLOCKED — preempted=runnable),
           sched_resume_jump → scheduler reentra y picks next ready
   - Kernel tasks NUNCA se preemptean (CS check). Modelo cooperativo
     preservado para servers.
   - Cleanup: saved_return field removida; user_task_resume usa
     t->saved_rax. sys_nanosleep setea saved_rax = 0 (valor de retorno).
   - timer_preempts() counter + /sys/timer expone "preempts: N"
   - Validado:
       - cat /sys/timer durante `exec /bin/osh -e "while 1==1 {x=1}"`
         muestra preempts subiendo (~18 en run de 1M iters)
       - shell sigue respondiendo durante runaway user task
       - sleep + hello_libc + test sin regresiones
OK 9.4 Ctrl+C live + /bin/kill — VERIFICADO en QEMU
   - task_t.kill_pending: flag sticky seteado por shell (Ctrl+C) o
     /bin/kill (vía SYS_KILL). Limpiado al destruir el task.
   - Shell tracking foreground:
     - static uint64_t fg_pid en shell_server.c
     - cmd_exec setea fg_pid = pid después de proc_exec exitoso
     - handler IPC_PROC_EXITED limpia fg_pid si arg1 == fg_pid
   - Ctrl+C handler tiene dos modos:
     - fg_pid != 0 → busca task por pid, setea kill_pending,
       imprime "^C" pero NO redraw prompt (espera IPC_PROC_EXITED)
     - fg_pid == 0 → modo legacy: cancela línea de input + prompt
   - Delivery: int80_dispatch_wrapper y timer_handle chequean
     kill_pending al volver a CPL=3; si set, llaman
     proc_exit_current_user(130) (128 + SIGINT).
     - syscall path: cubre tasks que hacen syscalls regulares
       (la mayoría)
     - timer path: cubre runaway tasks que NO hacen syscalls,
       catch dentro de 10ms del ^C
   - SYS_KILL = 62 (Linux number) en syscall.h
   - sys_kill(pid, sig) ignora sig; setea kill_pending si task
     existe y es ring-3 (pml4 != NULL); ESRCH en otro caso
   - libc kill() en unistd.h
   - /bin/kill PID — tool clásico. argv pid → kill(pid, 9).

OK 9.4 extensión: kill sobre BLOCKED + shell builtin — VERIFICADO en QEMU
   - bugfix crítico: kill_pending sobre task BLOCKED (e.g., dormida en
     nanosleep) nunca disparaba — los dos delivery points originales
     (return de syscall, IRQ de timer en CPL=3) requieren que la task
     esté corriendo o dentro de un syscall. Sleep larga + kill quedaba
     esperando al wakeup natural (50s).
   - Fix 1: sys_kill, si la task está TASK_BLOCKED, le pone
     wakeup_at_ms=0 y state=READY → el scheduler la dispatcha en el
     próximo tick.
   - Fix 2: user_task_trampoline (src/proc/exec.c) chequea kill_pending
     ANTES del iretq → si está set, llama proc_exit_current_user(130)
     y nunca toca ring 3. Cubre tanto resume vía saved_valid como
     fresh dispatch. Latencia total: ≤10ms (un tick del scheduler).
   - cmd_kill (shell builtin nuevo): comparte sys_kill con /bin/kill
     en lugar de tocar t->kill_pending directo. Garantiza el
     force-wake; antes el builtin saltaba ese paso y dejaba BLOCKED.
   - Validado:
     - exec /bin/sleep 50 & ; kill 5 → [5] done casi inmediato
     - exec /bin/sleep 30 ; ^C → vuelve al prompt antes
     - runaway osh loop foreground + ^C → kill ≤10ms
     - exec /bin/kill 3 → ESRCH (kernel tasks rechazadas)

OK 9.4 background jobs (`&`) — VERIFICADO en QEMU
   - cmd_exec parsea `&` final (después de stripear trailing spaces).
     Si presente: lanza vía proc_exec pero NO setea fg_pid; imprime
     "[pid]" y redibuja el prompt inmediatamente.
   - IPC_PROC_EXITED handler diferencia:
     - msg.arg1 == fg_pid → caso foreground original (clear + prompt)
     - msg.arg1 != fg_pid → bg done: imprime "\n[pid] done\n" +
       prompt + re-emite input_len bytes del buffer para que el
       usuario no pierda lo que estaba tipeando.
   - Combina con Ctrl+C: si hay fg corriendo, ^C va al fg; las bg
     siguen vivas y se matan con `kill PID`.
   - Pattern Bash-style básico: 1 fg + N bg simultáneas, hasta el
     límite de tasks (16 slots).

OK 9.4 SYS_GETPID + libc getpid — VERIFICADO en QEMU
   - SYS_GETPID = 39 (Linux number). syscall_dispatch case sin args.
   - sys_getpid retorna task_current()->pid; 0 si no hay task
     (kernel-mode caller, no debería ocurrir desde user).
   - libc: pid_t getpid(void) en unistd.h.
   - Usado por /bin/top para mostrar su propio pid en el header.

OK 9.4 /bin/top — viewer live — VERIFICADO en QEMU
   - tests/top.c: loop infinito que cada segundo emite ANSI clear
     (\033[2J\033[H), header con getpid() + "Ctrl+C to exit", luego
     dump_file de /sys/uptime, /sys/tasks, /sys/mem. sleep(1) entre
     refreshes; Ctrl+C lo termina vía kill_pending durante el sleep.
   - Soporte ANSI mínimo agregado a framebuffer_draw_string:
     parser de ESC [ <param> { J | H } — el resto se consume hasta
     la letra final y se ignora. Solo ESC[2J/ESC[J (clear screen)
     y ESC[H (home cursor) tienen efecto. Suficiente para top y
     para TUIs futuras simples.
   - Demo recomendada:
       exec /bin/osh -e "while 1==1 { x = 1 }" &
       exec /bin/top
     Mostrá `dispatches` del osh subiendo rápido mientras top y
     shell siguen vivos. Es la prueba visual de que preempt CPL=3
     + sleep real + IRQ delivery + bg jobs funcionan en conjunto.

### FASE 10 — Microkernel real
43. mover drivers a userspace
44. mover servers a userspace
45. IPC user/kernel real
46. isolation por address spaces

### FASE 11 — TUI potente
47. mini Norton Commander / mini-mc
48. viewer
49. editor
50. copy/move/delete visual

### FASE 12 — Gráfico
51. window_server
52. terminal window
53. Chip-8 emulator
54. mouse
55. compositor simple
