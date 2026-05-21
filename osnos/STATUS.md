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

### FASE 8 — Disco real — CERRADA

OK 8.1 ATA PIO block driver — VERIFICADO en QEMU
   - src/drivers/block_ata.{c,h}: driver PIO @ Primary IDE (0x1F0/0x3F6),
     LBA28. `block_ata_init()` corre IDENTIFY (cmd 0xEC), parsea sector
     count (words 60..61) + model (words 27..46 byte-swapped). Sin DMA,
     sin IRQs — polling de BSY/DRQ con presupuesto 1M iters.
   - block_ata_read_sector / block_ata_write_sector: 512B PIO; write
     emite FLUSH_CACHE (0xE7) post-data para write-through, sin buffer
     cache en el kernel.
   - block_ata_present / block_ata_sector_count / block_ata_model
     expuestos para el sysfs.
   - kmain: block_ata_init() corre después de timer_init(); falla
     silenciosa (present=false) si no hay disco.
   - sysfs nuevo: /sys/disks (model + sectors + bytes); /sys/mounts ya
     listaba /sd automáticamente vía longest-prefix.

OK 8.1 wiring QEMU + sd.img — VERIFICADO en QEMU
   - GNUmakefile: target `sd.img` que crea imagen de 16 MiB via mtools
     (mformat + mcopy con seeds README.TXT y HELLO.TXT). Sin sudo /
     sin loopback mount.
   - run-bios depende de sd.img; pasa `-drive file=sd.img,format=raw,
     if=ide,index=0,media=disk` y usa **-M pc** (i440FX + PIIX3-IDE)
     en vez de q35.
   - q35 monta el disco vía ich9-ahci (SATA AHCI), NO en puertos
     legacy 0x1F0/0x3F6. -M pc levanta piix3-ide con IDE legacy
     wired. Diagnosticado vía `info qtree` en monitor QEMU al ver
     que `/sys/disks` no detectaba nada con q35.

OK 8.2 FAT16 parser (read-only) — VERIFICADO en QEMU
   - src/fs/fat.{c,h}: parser puro sobre block_ata.
   - fat_init: lee sector 0, valida boot signature 0x55AA + BPB,
     calcula fat_lba / root_dir_lba / data_lba. Rechaza FAT12 y FAT32
     vía cluster-count check ([4085, 65525)).
   - dir_slot_locate / read_dir_slot: dos primitivas separadas. La
     primera no interpreta marker bytes (la usa el alocador de slots);
     la segunda devuelve "end-of-dir" en 0x00 (la usan lookup/readdir).
   - fat_lookup: walk de componentes, conversion 8.3 case-insensitive
     ("readme.txt" → "README  TXT"), navega root + subdirs.
   - fat_read_file: walk cluster chain con offset arbitrario,
     copia sector-a-sector.
   - fat_readdir: cursor lineal opaco; skip de 0xE5 / LFN / volume label.
   - Solo 8.3 short names. LFN se ignora silenciosamente.

OK 8.3 VFS adapter — VERIFICADO en QEMU
   - src/fs/fat_vfs.{c,h}: const vfs_ops_t fat_vfs_ops. Strip de "/sd"
     antes de delegar al parser. Esconde "." y ".." en readdir (convención
     POSIX/VFS).
   - bootstrap_fs: `if (fat_init() == 0) vfs_mount("/sd", &fat_vfs_ops, 0)`.
     Sin disco el mount no aparece; el resto del FS sigue intacto.
   - `cat /sd/README.TXT` imprime "hola desde el disco".
   - `ls /sd` lista README.TXT + HELLO.TXT.

OK 8.4 FAT write support — VERIFICADO en QEMU (round-trip + reboot persistence)
   - fat_set_entry: setea FAT[c] = value en TODAS las copias del FAT
     (mirror). Sin esto cada write rompe consistencia y fsck explota.
   - fat_alloc_cluster: scan lineal desde cluster 2, primer free claimea
     marcando como EOF (0xFFFF).
   - fat_free_chain: walk + set 0 en cada eslabón.
   - fat_alloc_chain(N): aloca + enlaza N clusters; rollback completo
     en cualquier fallo (no deja chain parcial).
   - write_data_into_chain: copia bytes a través del chain, padea última
     sector con zeros (no leak de RAM del kernel a disco).
   - Orden crash-safe en write: alloc-new-chain → write-data →
     update-dirent → free-old-chain. Crash entre fases deja como mucho
     clusters huérfanos (fsck-recoverable), nunca dirent apuntando a
     basura.
   - fat_write_path: maneja overwrite (free old chain al final) y
     create (split parent/base, name_to_83, find_free_dir_slot, build_dirent_raw).
   - fat_append_path: read-existing-into-scratch + concat + write. Cap
     FAT_APPEND_SCRATCH = 8 KiB.
   - fat_unlink_path: free chain + raw[0]=0xE5 en el dirent.
   - fat_mkdir_path: aloca un cluster, zero-llena, escribe . y .. en
     los primeros 64 bytes, registra dirent en parent con ATTR_DIR.
     ".." apunta al first_cluster del parent (0 para root, convención FAT).
   - fat_rmdir_path: rechaza si encuentra cualquier valid entry que no
     sea . / ..; free chain + 0xE5 en dirent.
   - find_free_dir_slot: reusa primer 0xE5 (deleted), o claimea primer
     0x00 (end-of-dir). En post-mformat-zero el slot inmediatamente
     siguiente sigue siendo 0x00, así que la marca de fin se preserva
     sin escribir un terminator explícito.
   - Sin LFN (los nombres se almacenan SIEMPRE como 8.3 uppercase).
   - Sin rename in-place: vfs_move cae al fallback copy+unlink.
   - fat_vfs_ops migrado: write/append/mkdir/rmdir/unlink ya no son
     EROFS. .rename = NULL.

OK 8.5 fsck minimal — VERIFICADO en QEMU (read-only audit)
   - src/fs/fat.c: fat_fsck_report(out, out_size). Audit pasivo, nunca
     escribe a disco.
   - Cluster leak detection: bitmap estático de 8 KiB en BSS
     (FSCK_BITMAP_BYTES = 65525/8 + 1). Walk DFS iterativo del directory
     tree (stack explícito FSCK_MAX_DEPTH=16, sin recursión); cada
     dirent.first_cluster dispara fsck_walk_chain que marca cada
     cluster visitado. Después se recorre el FAT entero; cualquier
     entry no-zero / no-bad sin bit set es leak.
   - Cross-link / cycle detection: la misma fsck_walk_chain detecta
     revisitas (cluster ya marcado) → crosslinks++ + break, lo que
     también blinda contra FATs corruptos con ciclos auto-referentes.
     Hop cap FSCK_MAX_CHAIN_HOPS = 65525 como belt-and-suspenders.
   - FAT mirror divergence: fsck_check_mirror lee FAT[0] sector a
     sector y compara contra FAT[1..N-1]; suma de sectores que
     difieren se reporta.
   - Size consistency: para cada file regular, walk del chain devuelve
     `len`. Se valida (a) size==0 ⇒ first_cluster==0, (b) size>0 ⇒
     len>=1, (c) size <= len*cluster_size, (d) size > (len-1)*cluster_size
     (no clusters trailing inutilizados).
   - Diagnósticos extra: bad refs (chain → cluster fuera de rango / EIO
     leyendo FAT) y deep_skip (subdirs descartados por exceder MAX_DEPTH).
   - Skip explícito de "." y ".." en el walk para no contar las
     auto-referencias como crosslinks.
   - Expuesto vía /sys/fat_fsck (sysfs synthetic). `cat /sys/fat_fsck`
     corre el audit completo (~250 PIO reads en 16MB / FAT mirror
     incluido) e imprime: files / dirs / clusters free|used|bad /
     mirror / cross-links / leaks / size mismatches / bad refs.

TODO opcional fsck repair-mode — pendiente
   - Hoy es read-only audit. Una pasada que (a) libere clusters leaked,
     (b) trunque chains con cross-links, (c) escriba FAT[0] sobre los
     espejos divergentes sería el siguiente paso natural.

OK 8.6 rename in-place — VERIFICADO en QEMU
   - fat_rename_path(src, dst): expuesto vía fat_vfs_ops.rename, así
     que `mv` dentro de /sd ya no cae al fallback copy+unlink del VFS
     (que truncaba archivos > VFS_COPY_BUF_SIZE = 1024 B sin avisar).
   - Fast path (mismo parent dir): una sola RMW del sector del dirent
     existente — sobrescribe bytes 0..10 (8.3 name). O(1) en bytes
     escritos, independiente del tamaño del archivo.
   - Cross-directory: reserva slot libre en dst-parent (find_free_dir_slot,
     falla rápido con ENOSPC), marca src dirent 0xE5, escribe el nuevo
     dirent (32 bytes copiados del src + name reemplazado) en el dst slot.
   - Orden crash-safe deliberado: src-deleted FIRST, dst-write segundo.
     Crash entre ambos = orphan chain (fsck-recoverable), NUNCA cross-link.
   - Rechaza overwrite de dst existente (EEXIST). Rename a sí mismo
     (mismo dirent_lba+offset resuelto desde dst path) es no-op.
   - Comparación same-parent vía src_parent.first_cluster ==
     dst_parent.first_cluster (root sentinel = 0 ambos lados).

OK 8.7 LFN read-side — VERIFICADO en QEMU
   - LFN constants (LFN_SEQ_LAST=0x40, LFN_SEQ_MASK=0x1F,
     LFN_CHARS_PER_SLOT=13, LFN_MAX_SEQ=5) y helpers nuevos:
     lfn_checksum_11 (rotate-right-add sobre el 8.3), lfn_extract_chars
     (UCS-2 → ASCII, '?' para chars no-ASCII, stop en 0x0000/0xFFFF).
   - lfn_accum_t: state machine para acumular slots LFN mientras se
     scanea el dir hacia adelante. Maneja restart al ver otro 0x40,
     reseteo en cksum/seq mismatch, orphan tail.
   - fat_readdir rewriten: en cada slot decide entre LFN slot
     (accumulate), volume label (reset), deleted (reset), 8.3 dirent
     (parse + finalize LFN si está completo y checksum matchea, else
     fallback al short name). Cursor sigue siendo lineal sobre slots
     raw, así que múltiples readdir calls retoman después del grupo.
   - dir_find_by_83 reemplazada por dir_find_by_name: itera con
     fat_readdir (que ya colapsa LFN+8.3) y compara case-insensitive
     contra el nombre devuelto. Match contra long name OR 8.3 alias.
   - fat_lookup component buffer crecido a FAT_NAME_MAX (64), name_to_83
     ya no se llama en lookup. Permite paths con espacios.
   - fat_dirent_t.name: 13 → FAT_NAME_MAX (64 bytes) en fat.h.
   - Seed sd.img incluye archivo "My Long Filename.txt" (alias on-disk
     "MYLONG~1.TXT" + LFN slots) para validación inmediata.
   - Verificado: `ls /sd` muestra "My Long Filename.txt", `cat` con
     ese nombre (case-insensitive) andó, y `cat /sd/MYLONG~1.TXT`
     también (8.3 alias sigue funcionando).

OK 8.7.1 bugfix shell: strip de outer quotes en make_absolute_path
   - Hoy `cat "/sd/file with spaces"` fallaba porque cmd_cat (y todos
     los handlers que toman un path) pasan args literal a
     make_absolute_path. Las comillas se trataban como prefijo no-/,
     resultando en /home/"... relativo a cwd.
   - Fix: make_absolute_path ahora strippea un par de outer quotes
     matched (single o double) + trailing whitespace antes del check
     absoluto/relativo. Comillas internas y sin match pasan tal cual.
   - Beneficia: cat, rm, touch, mkdir, rmdir, tree, ls, cd y cualquier
     comando que usa make_absolute_path. cmd_echo ya tenía su propio
     strip_outer_quotes para el filename de redirección, sin conflicto.

OK 8.8 LFN write-side — VERIFICADO en QEMU (258 PASS / 0 FAIL en cmd_test)
   - name_to_83 ahora rechaza explícitamente espacios y un segundo '.'
     en el componente, así que esos nombres caen automáticamente al
     path LFN en vez de generar dirent inválidos. Mayúsculas se
     normalizan sin LFN (case loss aceptado).
   - gen_short_alias(parent, long_name, alias11): genera BASE~N.EXT
     con collision check vía dir_has_8_3, hasta N=99. Filtra a
     uppercase alfanumérico + '_' para chars no soportados en 8.3.
   - build_lfn_slot(out, name, total_len, seq, is_last, cksum):
     empaqueta los 13 UCS-2 chars en los offsets 1/3/5/7/9, 14/16/.../24,
     28/30; NUL después del último char del long name, 0xFFFF padding,
     attr 0x0F, cksum compartido en todos los slots de la secuencia.
   - find_free_dir_slots_run(dir, count, lba_arr, off_arr): scan
     lineal aceptando 0xE5 (deleted) o 0x00 (end-of-storage zone); el
     run se reinicia si ve un slot ocupado. ENOSPC si no encuentra
     ventana del tamaño pedido antes del fin del dir storage.
   - entry_naming_t + compute_entry_naming + write_entry_with_naming:
     trío de helpers que unifica el path "instalar entrada nueva" para
     fat_write_path y fat_mkdir_path. Detecta automáticamente 8.3 vs
     LFN, reserva los slots, escribe LFN slots en reverse seq (highest
     con 0x40 va primero on-disk) y el 8.3 al final. Rollback automático
     en cualquier fallo intermedio.
   - locate_target_and_lfn(parent, target_lba, target_off,
     &target_idx, &lfn_count): walk del parent dir contando slots LFN
     consecutivos con checksum matching antes del 8.3 target. Usado
     por unlink/rmdir/rename para borrar la secuencia completa.
   - delete_slot_by_idx(dir, idx): RMW para marcar slot 0xE5 sin
     romper la sector entera.
   - fat_unlink_path / fat_rmdir_path: ahora borran 8.3 + todos los
     LFN slots precedentes. Orden: free chain → delete 8.3 → delete LFN
     slots. Crash entre el 8.3 y los LFN deja orphan slots (cosmético,
     fsck-recoverable), nunca cross-link.
   - fat_rename_path:
     * Fast path (same-parent + new name fits 8.3 + src sin LFN): un
       sector RMW reescribiendo los 11 bytes del name. O(1).
     * Path general (cualquier otro caso, incluyendo cross-dir,
       LFN→8.3, 8.3→LFN, LFN→LFN): write_entry_with_naming en dst
       parent → delete src 8.3 → delete src LFN slots. Ventana de
       cross-link breve y fsck-recoverable.
   - fat_dirent_t agrandado con campo short_name[13] que siempre
     guarda el 8.3 alias on-disk (incluso cuando name contiene el long
     LFN). dir_find_by_name compara case-insensitive contra los dos,
     así que `cat /sd/MYLONG~1.TXT` funciona tanto como
     `cat "/sd/My Long Filename.txt"`.

OK 8.8 shell quote stripping
   - make_absolute_path strippea pair de outer quotes (single o double)
     + trailing whitespace antes del check absoluto/relativo. Permite
     `cat "/sd/My Long Filename.txt"` desde el shell. Beneficia todos
     los handlers que pasan args a make_absolute_path (cat/rm/touch/
     mkdir/rmdir/tree/ls/cd).

OK 8.9 self-test extendido — VERIFICADO en QEMU
   - cmd_test extendido con ~38 asserts FAT en su propio sandbox
     /sd/__fattest. Pre-clean + post-clean para re-ejecución limpia.
   - Cubre: read seed 8.3, read seed LFN (case-insensitive + alias),
     create 8.3, create LFN (write-side), append, overwrite truncate,
     rename 8.3↔8.3 (fast path), LFN→8.3 (borra slots), 8.3→LFN
     (escribe slots), cross-dir LFN→LFN, EEXIST en collision, ENOTEMPTY
     en rmdir no-vacío, empty file (size=0), unlink + rmdir nested,
     y fsck audit final: leaks/cross-links/size mismatch/bad refs
     todos none, mirror OK.
   - Total con la suite anterior + FAT: 258 PASS / 0 FAIL.
   - SKIP automático del bloque FAT si /sd no está montado (boot sin
     sd.img).

### FASE 8.5 — Networking (post-FAT) — EN PROGRESO

OK 8.5.1 PCI scan + RTL8139 driver — VERIFICADO en QEMU
   - src/micro/pmm.c: pmm_alloc_pages_contig(n_pages). Linear scan
     del bitmap buscando run de N free bits. O(total_pages*n_pages) en
     worst case; aceptable porque sólo se usa para buffers DMA al boot.
   - src/drivers/pci.{c,h}: PCI config-space sobre ports 0xCF8/0xCFC.
     - pci_find_device(vendor, device, *out): scan bus 0..255 dev 0..31
       fn=0, primer match retorna pci_addr_t.
     - pci_read_bar / pci_read_irq: ofsets 0x10+4*N y 0x3C.
     - pci_enable_bus_master: set bits 0 (I/O space) + 2 (bus master)
       en el config register 0x04.
   - src/drivers/rtl8139.{c,h}: driver minimal.
     - Boot path: pci_find_device(0x10EC, 0x8139) → enable bus master
       → read BAR0 (I/O port range) y IRQ line del PCI config →
       wake CONFIG1=0 → soft-reset (write 0x10 a CMD, poll hasta 0)
       → read MAC (IDR0..5).
     - RX ring: 3 páginas contiguas (8KB+16+1500 ≈ 9.5KB) vía
       pmm_alloc_pages_contig. RBSTART = phys, RCR =
       APM|AB|WRAP (no AAP por ahora), RBLEN 8KB+16. CAPR inicializado
       a -0x10 (convención on-chip). Reader offset trackeado en
       dev.rx_offset, advancea por header(2)+length(2)+payload con
       4-byte alignment, wrap dentro de 8192.
     - TX ring: 4 buffers de 1 página cada uno (pmm_alloc_page,
       single-page = contig por definición). TSAD0..3 = phys cada uno.
       rtl8139_tx() round-robins entre los 4 slots; chequea OWN bit
       del slot antes de reescribir para no pisar DMA en flight.
       Disparo: outl(TSD, len), chip padea cortos a 60.
     - IRQ: asm stub rtl8139_irq_entry (save GPRs, call C, restore,
       iretq) instalado vía idt_set_handler en vector
       PIC_VECTOR_BASE_{MASTER,SLAVE} + irq_line%8.
       rtl8139_irq_handle drena ISR (write-1-to-clear), procesa
       RX ring si ROK, cuenta TX si TOK, errors si RER/TER/RXOVW.
       pic_send_eoi al final.
   - Diagnósticos: /sys/net (sysfs) muestra driver, mac, io_base,
     irq_line, irqs, rx_packets/bytes, tx_packets/bytes, errors.
   - kmain: rtl8139_init() después de timer_init; silent-fail si no
     hay chip (kernel sigue booteando sin red).
   - GNUmakefile run-bios: agrega `-netdev user,id=net0,hostfwd=tcp::
     8080-:80 -device rtl8139,netdev=net0`. QEMU slirp NAT con
     guest IP 10.0.2.15, gateway 10.0.2.2, port-forward host:8080 →
     guest:80 para futuro httpd.
   - Verificado: /sys/net muestra mac 52:54:00:12:34:56 (default
     QEMU), io_base 0xc000, irq_line 11. Sin tráfico al boot,
     irqs=0 esperado (se van a mover en 8.5.2 cuando enviemos ARP).

OK 8.5.2 Ethernet + ARP — VERIFICADO en QEMU
   - src/drivers/rtl8139: extendido con rtl8139_set_rx_callback. El
     drain_rx llama el callback registrado pasando el frame ya sin
     el trailer CRC. Callback corre en IRQ context con IRQs enabled.
   - src/net/eth.{c,h}: Ethernet II framing.
     - ETH_HEADER_SIZE=14, ETHERTYPE_IPV4=0x0800, ETHERTYPE_ARP=0x0806.
     - eth_broadcast_mac = FF:FF:FF:FF:FF:FF constante.
     - net_init: registra net_rx como callback del driver, llama
       arp_init. Silent-fail si no hay NIC.
     - net_rx(frame, len): parsea ethertype del header (offset 12..13,
       big-endian), dispatcha a arp_handle por ahora; ETHERTYPE_IPV4
       cae en default (handled en 8.5.3).
     - eth_send(dst_mac, ethertype, payload, len): copia 14 bytes
       de header (dst + nuestra src MAC vía rtl8139_mac + ethertype
       BE) + payload, manda vía rtl8139_tx. Stack-local buffer
       hasta ETH_MAX_PAYLOAD=1500.
     - Config estática: LOCAL_IP 10.0.2.15, GATEWAY 10.0.2.2,
       NETMASK 255.255.255.0 (defaults slirp).
   - src/net/arp.{c,h}:
     - ARP wire layout (28 bytes): htype/ptype/hlen/plen/oper +
       sha/spa/tha/tpa. Todos los multi-byte fields BE.
     - Cache 8 entries: { valid, ip (host order), mac[6], last_seen_ms }.
       Insert con LRU eviction sobre last_seen_ms. Lookups cli/sti-
       guarded porque writes vienen del IRQ.
     - arp_send_request(ip): broadcast con oper=1, tha=0.
     - arp_send_reply(target_mac, target_ip): unicast con oper=2.
     - arp_handle: valida htype/ptype/hlen/plen, popula cache con
       sha/spa (todo request o reply nos enseña algo nuevo), responde
       si oper=REQUEST y tpa == nuestra IP.
     - arp_resolve(ip, mac_out, timeout_ms): cache check primero;
       si miss, envía request y busy-polls timer_ms() hasta hit o
       deadline. Funciona porque el IRQ del NIC corre asíncrono
       y actualiza la cache.
     - arp_dump(out): texto human-readable para /sys/arp.
   - Sysfs nuevo: /sys/arp (cache + IPs locales).
   - Shell nuevo: comando `arp [IP]` (default = gateway). Parser
     dotted-quad propio (parse_ipv4), imprime MAC en formato
     `xx:xx:xx:xx:xx:xx`.
   - kmain: net_init() después de rtl8139_init.
   - Verificado: `arp` desde shell → /sys/net muestra
     tx_packets/rx_packets/irqs incrementados, /sys/arp lista
     10.0.2.2 con su MAC.
OK 8.5.3 IPv4 + ICMP echo — VERIFICADO en QEMU
   - src/net/ip.{c,h}: header de 20 bytes (sin opciones, IHL=5), TTL
     fijo en 64. Routing trivial: si dst está en el mismo /24 que
     local_ip → ARP del dst; si no → ARP del gateway. ip_send
     llama arp_resolve(next_hop, 500ms) y arma el frame Ethernet
     vía eth_send.
   - ip_checksum: RFC 1071 — sum de 16-bit words BE + carry fold +
     ones complement. Mismo helper sirve para generar (con field=0)
     y para validar (sum sobre header completo debe dar 0).
   - ip_handle: valida version=4, IHL>=5, total_len<=frame, checksum.
     Dropea si el dst no es nuestra IP ni broadcast (255.255.255.255).
     Dispatcha por byte protocol: 1=ICMP (8.5.3), 6=TCP (8.5.5),
     17=UDP (8.5.4).
   - src/net/icmp.{c,h}: echo request/reply (type 8/0, code 0).
     icmp_handle responde automáticamente a requests dirigidos a
     nosotros (copia payload, type=0, recalcula checksum). Para
     replies, hace match contra el `pending` slot (src_ip + id + seq)
     y setea replied=true para despertar a icmp_ping.
   - icmp_ping(dst, id, seq, timeout_ms, *rtt_out): synchronous —
     send + busy-poll de timer_ms() hasta hit o deadline. Payload
     fijo de 56 bytes (ping clásico, total 64 incluyendo header).
   - Shell nuevo: comando `ping IP` con parser dotted-quad reutilizado
     de cmd_arp. Imprime "reply: time=N ms" o "timeout".
   - /sys/net extendido: counters de ip rx/tx/drop e icmp rx/tx.
   - Verificado: ping al gateway 10.0.2.2 (10ms primera vez con ARP
     resolve, 0ms con cache) y ping a 10.0.2.3 (DNS slirp) andando.
     ping a 10.0.2.15 (self) hace timeout — esperado: el chip no
     devuelve sus propios frames y slirp tampoco bouncea localhost.
OK 8.5.4a UDP layer + kernel socket API — VERIFICADO en QEMU
   - src/net/udp.{c,h}: header 8 bytes (sports, dport, length, cs BE).
     udp_compute_checksum arma pseudo-header (src/dst IP + 0 + proto17
     + udp length) seguido del segmento UDP, reusa ip_checksum. Sender
     transmite 0xFFFF cuando el cálculo da 0 (RFC 768). Receiver
     respeta cs=0 como "skip verify".
   - src/net/socket.{c,h}: socket table 8 slots, espacio de
     descriptores propio (independiente del fd table — 8.5.4b lo
     integra). RX ring de 8 datagrams × 1024B cada uno. sock_create/
     bind/close/recvfrom/sendto. sock_bind admite port=0 (ephemeral
     en 40000..60000) e ip=0 (INADDR_ANY).
   - sock_recvfrom: busy-poll cli/sti contra rx_count, timer_ms
     deadline. Lock cli/sti porque sock_deliver_udp corre en IRQ
     context y modifica el ring.
   - sock_sendto: auto-bind si no había puerto local (para ephemeral
     send). Llama udp_send → ip_send → arp_resolve → eth_send →
     rtl8139_tx.
   - sock_deliver_udp: invocado desde udp_handle. Busca el socket
     bindeado al dst_port (cualquier local_ip), enqueue. Si queue
     full → drop silencioso.
   - Hook en ip.c: IP_PROTO_UDP → udp_handle. Hook en udp_handle →
     sock_deliver_udp.
   - Shell nuevo: `udptest [PORT]` (default 1234) — bind, recibe 10s,
     imprime "rx IP:PORT [contenido]" y echo back al sender.
   - QEMU: agregado hostfwd=udp::1234-:1234 al -netdev user.
   - /sys/net: counters udp rx/tx/drop.
   - **Bugfix crítico**: el asm stub rtl8139_irq_entry usaba r12 como
     scratch para alinear rsp sin guardarlo. r12 es callee-saved en
     SystemV x86-64; el código interrumpido lo veía corrupto post-iretq.
     Disparaba GPF aleatorios durante udptest. Fix: pushq %r12 al
     entrar, popq %r12 al salir.
   - Verificado: desde Mac `echo hola | nc -u -w1 127.0.0.1 1234`,
     OSnOS muestra "rx 10.0.2.2:NNNNN [hola.]" — 3 datagrams seguidos
     con source ports random vía slirp NAT.

OK 8.5.4b Linux socket syscalls + libc + udptest ELF — VERIFICADO en QEMU
   - osnos_status_t extendido con códigos Linux errno de networking:
     ENOTSOCK=88, EPROTONOSUPPORT=93, EAFNOSUPPORT=97, EADDRINUSE=98,
     EADDRNOTAVAIL=99, ENETDOWN=100, ECONNRESET=104, ETIMEDOUT=110,
     ECONNREFUSED=111. Idénticos a Linux x86_64.
   - osnos_fd_t (src/micro/fd.{c,h}) crece con is_socket + sock_idx.
     fd_alloc y fd_free los inicializan / limpian. sys_close ahora
     llama sock_close cuando el fd es socket.
   - OSNOS_SOCK_* renumerado para matchear Linux exacto: SOCK_STREAM=1,
     SOCK_DGRAM=2. (Antes tenía el orden invertido.)
   - 4 syscall numbers nuevos (Linux x86_64): SYS_SOCKET=41, SYS_BIND=49,
     SYS_SENDTO=44, SYS_RECVFROM=45. Declarados en src/micro/syscall.h
     y src/lib/libc/syscall.h en lockstep.
   - lib/libc/syscall.h: osnos_syscall5 + osnos_syscall6 nuevos
     (sendto/recvfrom toman 6 args). System V x86_64 convention con
     r10/r8/r9 como 4°/5°/6° argument.
   - Handlers en src/micro/syscall.c:
     - unpack_sockaddr_in / pack_sockaddr_in: parsean los 16 bytes
       de la struct Linux (family LE en bytes 0..1, port BE en 2..3,
       IP BE en 4..7, sin_zero en 8..15) byte-por-byte para no
       depender de alineación user-side.
     - sys_socket: solo AF_INET + SOCK_DGRAM por ahora; reserva slot
       en sock_create + fd_alloc, conecta ambos. EAFNOSUPPORT para
       SOCK_STREAM hasta 8.5.5.
     - sys_bind / sys_sendto: validan family, delegan a sock_*.
     - sys_recvfrom: busy-wait 0x7FFFFFFF ms (~25 días, "infinito")
       hasta que llegue. Devuelve el src en el sockaddr_in opcional,
       actualiza addrlen a 16.
   - lib/libc/inet.c: stubs ENOSYS reemplazados por osnos_syscall*
     reales para socket/bind/sendto/recvfrom. -errno → errno + return
     -1 al estilo Linux.
   - tests/udptest.c: ELF user-mode con socket(AF_INET, SOCK_DGRAM, 0)
     + bind(htons(port), htonl(INADDR_ANY)) + recvfrom/sendto loop
     de 5 datagrams. Demostración end-to-end del path:
     ring-3 ELF → libc → SYSCALL ABI → sys_* → sock_* → UDP → IP →
     ARP → eth → RTL8139. Registrado en builtin.c.
   - Verificado: `exec /bin/udptest` + `echo hola | nc -u -w1
     127.0.0.1 1234` desde Mac → llega + echo back con caller IP/port
     correcto. Linux ABI invariant preservado: los números de syscall
     y la struct sockaddr_in son idénticos a Linux x86_64.

OK 8.5.5a TCP handshake (passive) — VERIFICADO en QEMU
   - src/net/tcp.{c,h}: header 20 bytes + flags FIN/SYN/RST/PSH/ACK/URG,
     tcp_state_t enum con los 11 estados de RFC 793.
   - tcp_compute_checksum: pseudo-header (src/dst IP + 0 + proto=6 +
     tcp length) + segmento; reusa ip_checksum.
   - tcp_send(dst_ip, dst_port, src_port, seq, ack, flags, window,
     payload, len): arma header fijo (data offset 5) + payload,
     calcula checksum, manda vía ip_send (que hace ARP + eth).
   - tcp_handle: valida checksum + data offset, delega state machine
     a sock_tcp_handle_segment.
   - sock_t extendido con tcp_state, remote_ip/port, snd_nxt/snd_una/
     rcv_nxt. sock_create acepta SOCK_STREAM (1).
   - sock_listen(sd): pasa el socket a TCP_LISTEN (requiere bind previo).
   - sock_tcp_handle_segment: state machine pasiva.
     - LISTEN + SYN → SYN_RCVD, snd_una/snd_nxt = ISN (timer_ms+i*1000,
       cheap pero suficiente sin SYN-flood defense), rcv_nxt = peer_seq+1,
       envía SYN-ACK, snd_nxt++.
     - SYN_RCVD + ACK válido → ESTABLISHED.
     - ESTABLISHED + FIN/PSH → RST de vuelta (8.5.5a no hace data
       transfer ni close gracioso, esos llegan en 8.5.5b).
     - RST en cualquier estado → vuelve a LISTEN.
     - Segmento sin socket bindeado → respondemos con RST+ACK.
   - sock_tcp_get_peer(sd, &ip, &port): poll para ver si está
     ESTABLISHED. sock_tcp_reset(sd): manda RST y vuelve a LISTEN.
   - /sys/net: tcp rx/tx/drop counters.
   - Shell nuevo: tcptest [PORT] (default 80; matches el hostfwd
     existente tcp::8080-:80). Bind+listen, espera 15s, imprime peer,
     RST, close.
   - Verificado: desde Mac `nc -v -w1 127.0.0.1 8080` se conecta
     correctamente ("Connection succeeded!"), OSnOS muestra
     "connection from 10.0.2.2:NNNNN — handshake OK, sending RST"
     y nc ve el RST inmediato.

OK 8.5.5b TCP data transfer + graceful close — VERIFICADO en QEMU
   - sock_t extendido con byte ring `tcp_rx[4096]` + zombie/peer_fin
     flags. SOCK_TCP_RX_BUF = 4096, TCP_MSS = 1400.
   - sock_tcp_handle_segment recibe ahora `(payload, payload_len)` y
     procesa data en ESTABLISHED / FIN_WAIT_1 / FIN_WAIT_2: si seq
     matchea rcv_nxt, enqueue al ring (drop si lleno, peer retransmite),
     avanza rcv_nxt, emite ACK inmediato. Acept window =
     SOCK_TCP_RX_BUF - rx_count.
   - State machine completa para close:
     - active close: sock_close_tcp en ESTABLISHED → FIN+ACK out →
       FIN_WAIT_1 → ACK in → FIN_WAIT_2 → FIN in → CLOSED.
     - passive close: peer FIN in ESTABLISHED → CLOSE_WAIT, peer_fin
       flag → recv devuelve 0 (EOF). User llama close →
       FIN+ACK out → LAST_ACK → ACK in → CLOSED.
     - simultaneous close: FIN_WAIT_1 + FIN in → CLOSED.
   - Zombie flag: cuando sock_close_tcp se llama pero la conexión sigue
     en FIN_WAIT_*/LAST_ACK, el slot se marca zombie y se libera
     automáticamente cuando llega CLOSED (desde el IRQ handler).
   - sock_recv(sd, buf, len, timeout_ms): busy-poll cli/sti contra el
     ring. Returns n>0 bytes, 0 en EOF (peer_fin + ring vacío),
     -2 timeout, -1 error. Linux semántica.
   - sock_send(sd, buf, len): splittea en TCP_MSS-sized segments con
     PSH|ACK flags, avanza snd_nxt. No retransmite (asumimos QEMU
     localhost reliable — TODO 8.5.5d para retransmisión real).
   - sock_close: para STREAM dispatcha a sock_close_tcp (Linux POSIX
     semantics — orderly close por defecto, no RST).
   - sock_tcp_reset: extension osnos para RST inmediato (sigue
     disponible para tests).
   - Shell tcptest extendido: tras handshake, lee una línea con
     sock_recv 5s timeout, hace echo back con sock_send, close.
   - Verificado: `echo "hola tcp" | nc -v -w2 127.0.0.1 8080` desde
     macOS BSD nc → OSnOS muestra "rx 9B: hola tcp." + "echoed back,
     closing" + tcptest done. nc recibe el echo y termina sin error.
     FIN exchange limpio.

   Gotcha documentado: nc BSD (macOS) no tiene `-N`; usar `-w2` para
   que cierre por timeout o `( ... ; sleep 1 ) | nc ...` para EOF
   programado.
OK 8.5.5c TCP listen+accept + child sockets + /bin/echotcp — VERIFICADO en QEMU
   - sock_t extiende con parent_sd (-1 normal, índice al LISTEN si es
     child), accept_q[4] (ring de child sds en ESTABLISHED pendientes
     de accept), backlog cap.
   - sock_tcp_handle_segment ahora hace 2-pass lookup: primero match
     4-tuple en sockets no-LISTEN; si no encuentra, busca LISTEN. Sin
     esto la LISTEN se comía los paquetes de sus propias conexiones.
   - LISTEN + SYN → alloc_child_for_syn(): nuevo slot inicializado en
     SYN_RCVD con remote pin, parent_sd = listen idx. SYN-ACK desde la
     ISN del child. La LISTEN sigue intacta esperando más SYNs. Si
     accept_q lleno → drop del SYN (peer retransmite).
   - SYN_RCVD + ACK válido en child → ESTABLISHED + push al accept_q
     del parent (idx del child al ring tail).
   - sock_listen(sd, backlog) clamp a SOCK_ACCEPT_QUEUE_DEPTH.
   - sock_accept(sd, &peer_ip, &peer_port, timeout_ms): busy-poll
     cli/sti contra accept_q_count. Dequeue → devuelve child sd.
   - Linux syscalls nuevos: SYS_LISTEN=50, SYS_ACCEPT=43. sys_accept
     hace fd_alloc + asocia con el child sd recibido + pack del peer
     en sockaddr_in.
   - sys_sendto / sys_recvfrom ahora dispatchean al path TCP cuando el
     socket es STREAM. send(fd,buf,n,flags) en libc se traduce a
     sendto(...,NULL,0); recv idem. Linux convention.
   - libc inet.c: listen / accept / send / recv real (no más stubs
     ENOSYS).
   - Bugfix: sys_socket aceptaba sólo SOCK_DGRAM, rechazaba SOCK_STREAM
     con errno=97 (EAFNOSUPPORT). Agregado el check para ambos types.
   - tests/echotcp.c: bind→listen(4)→accept loop x5→recv+send+close.
     Registrado en builtin.c como /bin/echotcp.
   - Verificado: `exec /bin/echotcp` + 5x `echo X | nc -v -w2
     127.0.0.1 8080` desde Mac → OSnOS imprime cada conexión con peer
     IP:port y el mensaje recibido; cada nc del Mac ve el echo y cierra
     limpio. accept se reusa con cada conexión, sin leaks de slots.
OK 8.5.6 select() + setsockopt + cooperative yield — VERIFICADO en QEMU
   - Status nuevo: OSNOS_EINTR=4. Para syscalls interrumpibles.
   - Syscall numbers: SYS_SELECT=23, SYS_SETSOCKOPT=54 (Linux x86_64).
   - sys_select (kernel) es **non-blocking single-pass**: hace un
     polling cycle sobre cada fd en nfds, marca readable/writable/
     except en out bitmaps, devuelve count. La libc loopea con
     nanosleep entre polls. Sin este split, el busy-poll en CPL=0
     monopolizaba el CPU y los servers cooperativos (keyboard, shell)
     nunca corrían → no se podía ni Ctrl+C ni tipear nada.
   - sys_setsockopt: stub que acepta SOL_SOCKET (level=1) + SO_REUSEADDR
     (optname=2) como no-op success. Otros options → EINVAL.
   - fd_readable check soporta:
     - stdin (fd 0) via stdin_readable() — chequea ring buffer.
     - sockets via sock_readable: UDP (rx_count>0), TCP LISTEN
       (accept_q_count>0), TCP ESTABLISHED/CLOSE_WAIT/FIN_WAIT_*
       (tcp_rx_count>0 OR peer_fin).
     - regular fds: always readable.
   - sock_readable usa `volatile sock_t *` para que clang -O2 no
     cachee las cuentas mutadas por el NIC IRQ.
   - libc: sys/select.h (fd_set 1024 bits = uint64_t[16], FD_ZERO/SET/
     CLR/ISSET macros), sys/time.h (struct timeval Linux x86_64 layout).
   - libc inet.c: select wrapper hace busy-loop con nanosleep(20ms)
     entre polls. NULL timeout = forever (loop infinito); timeout
     {0,0} = single poll; sino respeta deadline.
   - kill_pending check en busy-polls de sock_recv/recvfrom/accept
     (devuelve -1) para que un Ctrl+C entregado por el shell corte.
   - Bugfix stdin pre-buffer: keyboard_server pushea CADA keystroke a
     stdin_buf, incluyendo los chars que tipean el comando exec. Sin
     limpiar el buffer al exec, un select() sobre stdin disparaba
     inmediatamente con el '\n' del Enter ya bufereado. Fix:
     stdin_clear() llamado al inicio de proc_exec.
   - Bugfix port_in_use: las conexiones TCP en estados de cierre
     (FIN_WAIT_*, CLOSE_WAIT, LAST_ACK, etc.) ya no bloquean un
     re-bind a su mismo puerto. Estilo SO_REUSEADDR de Linux para
     TIME_WAIT. Sin esto, re-correr selecttest tras una conexión
     daba EADDRINUSE (errno 98).
   - tests/selecttest.c: ELF user-mode demo. Bind+listen TCP, select
     sobre srv + stdin. Cada SYN procesa la conexión via accept/recv/
     send; cualquier keystroke termina. Re-ejecutable sin problemas.
   - Verificado: nc desde Mac llega, OSnOS imprime el peer y echoea;
     tipear una tecla en QEMU termina selecttest limpio; re-correr
     `exec /bin/selecttest` rebindea sin EADDRINUSE.
OK 8.5.7 getaddrinfo no-DNS + /bin/selectserver — VERIFICADO en QEMU
   - lib/libc/include/netdb.h: struct addrinfo (Linux layout), flags
     AI_PASSIVE / AI_NUMERICHOST / AI_CANONNAME, EAI_* error codes,
     getaddrinfo / freeaddrinfo / gai_strerror prototypes.
   - lib/libc/netdb.c: scope minimal pero suficiente para Beej:
     - AF_UNSPEC → AF_INET (default). AF_INET6 → EAI_FAMILY.
     - node==NULL + AI_PASSIVE → INADDR_ANY; sin AI_PASSIVE → loopback.
     - node!=NULL → parse vía inet_aton (numeric IPv4). Nombres
       reales devuelven EAI_NONAME (no hay DNS resolver todavía).
     - service: decimal port 1..65535. /etc/services no soportado.
     - Aloca con malloc; freeaddrinfo libera struct + sockaddr_in.
     - Una sola entrada por call (no enumera IPv4+IPv6 separadas).
   - sys/socket.h ahora hace #include <sys/select.h> transitivamente
     (matchea glibc — necesario para compilar código que asume
     `sys/socket.h` trae `fd_set` consigo).
   - QEMU: hostfwd=tcp::9034-:9034 agregado al -netdev user.
   - tests/selectserver.c: **VERBATIM de Beej**
     (https://beej.us/guide/bgnet/source/examples/selectserver.c).
     Solo cambia el comentario header. Usa getaddrinfo, setsockopt
     SO_REUSEADDR, socket/bind/listen/select/accept/recv/send/close,
     inet_ntop, fprintf, perror, exit, FD_*. Todo de la libc + kernel
     osnos.
   - Verificado: `exec /bin/selectserver` en OSnOS, dos `nc -v
     127.0.0.1 9034` desde Mac. Cada uno se ve "new connection from
     ... on socket N". Tipear en uno aparece en el otro (chat multi-
     cliente broadcast). nc cierra con Ctrl+D → "socket N hung up".
   - **MILESTONE**: networking osnos completa el surface mínimo para
     correr aplicaciones BSD-sockets unmodified. La invariante de
     Linux ABI compat (números syscall, struct sockaddr_in,
     errno values, fd_set layout) se mantuvo desde FASE 4 sin grietas.

OK 8.5.8 TCP connect() / SYN_SENT / outbound — VERIFICADO en QEMU
   - State machine extiende con TCP_SYN_SENT:
     - Recibe SYN-ACK válido (ack == snd_nxt) → ACK back, ESTABLISHED.
     - Recibe SYN sin ACK (simultaneous open) → SYN_RCVD, SYN-ACK back.
     - Recibe RST → CLOSED (connection refused).
     - Bad ACK → RST + CLOSED.
   - sock_connect(sd, dst_ip, dst_port, timeout_ms):
     - Auto-bind a ephemeral port si no estaba bindeado.
     - Setea remote, ISN = timer_ms + sd*1000.
     - Manda SYN, marca SYN_SENT, snd_nxt++.
     - Busy-poll con volatile read del tcp_state hasta ESTABLISHED
       o CLOSED o deadline. poll_interrupted() respeta Ctrl+C.
   - Syscall SYS_CONNECT=42 (Linux x86_64). sys_connect unpacks
     sockaddr_in, llama sock_connect con timeout 5s, mapea a
     ECONNREFUSED en fallo.
   - libc inet.c: connect() pasa de stub ENOSYS a syscall real.
   - tests/tcpclient.c (/bin/tcpclient HOST PORT): parsea dotted-quad,
     socket(AF_INET, SOCK_STREAM), connect, send greeting, recv reply,
     close. Demo de extremo a extremo del path activo.
   - Verificado: Mac `nc -l 9050` + OSnOS `exec /bin/tcpclient
     10.0.2.2 9050` → handshake limpio, Mac ve "hello from osnos",
     reply de Mac vuelve a OSnOS. Funciona también contra IPs externas
     vía slirp (probado conexiones outbound).
   - **MILESTONE**: el surface socket completo (passive + active open
     + data + close + multiplex) está cubierto. Cualquier programa
     BSD-sockets que use IP literal compila y corre.
OK 8.5.9 DNS resolver + getaddrinfo con nombres — VERIFICADO en QEMU
   - lib/libc/resolver.{c,h} (privado, no se exporta en includes/):
     - dns_encode_name: "google.com" → "\7google\3com\0".
     - dns_skip_name: walk de labels + soporte de compression
       pointers (0xC0..) para parsear respuestas con name compression.
     - dns_parse_response: valida ID match + RCODE=0; iterate
       questions skip; en answers, primer TYPE=1 (A) record →
       extrae IPv4 en network byte order.
     - dns_resolve_a: UDP socket, query con QTYPE=A QCLASS=IN a
       slirp DNS 10.0.2.3:53. Recv con select(timeout=3s) para no
       colgar si no hay respuesta. 512 bytes max response, TC ignorado.
     - dns_next_id: counter seeded with getpid para no colisionar
       entre llamadas.
   - lib/libc/netdb.c (getaddrinfo): si inet_aton falla y
     AI_NUMERICHOST no está set, fallback a dns_resolve_a. Hostnames
     no resueltos devuelven EAI_NONAME como antes.
   - tests/tcpclient.c migrado a getaddrinfo (acepta IPs literales O
     hostnames). Mejora: si port==80, manda HTTP/1.0 GET real con
     Host header y Connection: close, así el server (Google etc.)
     responde. recv bounded con select(timeout=5s) para no colgar
     en peers que no respondan inmediato. Cap de 4KB en output.
   - Verificado: `exec /bin/tcpclient google.com 80` →
     "connecting to 172.217.18.110:80 (resolved from google.com)" →
     "connected!" → "sent 92 bytes" → HTML real de Google → "(peer
     closed, 773 bytes total)". DNS + TCP + HTTP funcionando
     end-to-end desde un hobby OS escrito en C desde cero.
   - **MILESTONE**: tu OS puede visitar sitios web. Cualquier
     programa BSD-sockets que use getaddrinfo/connect/recv compila
     y resuelve nombres reales vía DNS.

OK 8.5.10 /bin/httpd HTTP/1.0 server — VERIFICADO en QEMU
   - tests/httpd.c: bind+listen TCP port 80 (default), accept loop
     hasta MAX_CONNS=50 conexiones, parse de "GET /path HTTP/1.0\r\n",
     map a /sd/<path> (default /sd/index.html), abre el archivo
     vía open(O_RDONLY), fstat para Content-Length, stream con
     send() en chunks de 2 KiB.
   - Buffers grandes (g_req[1024] / g_chunk[2048] / g_hdr[256] /
     g_abs_path[256]) en BSS estático en vez de stack, porque la
     user stack en osnos es 1 página (4 KiB) y el call chain
     main→handle_conn→serve_file con buffers locales overflowea.
   - Bugfix: cli/sti ahora con clobber "memory" para que el compiler
     no cachee accept_q_count / tcp_rx_count / rx_count a través de
     los busy-polls. sock_accept y sock_recv y sock_recvfrom usan
     volatile sock_t * sobre el socket en sus loops por la misma
     razón. Sin esto, la SEGUNDA accept después de servir una
     conexión nunca veía el flag actualizado por el NIC IRQ.
   - Content-Type guess por extensión: html/htm/txt/css/js/json/png/
     jpg/gif. Default application/octet-stream.
   - Manejo de path traversal: rechaza ".." con 403. Limpia query
     string "?..." y fragment "#..." antes del mapping.
   - 404 cuando open falla, 405 si method != GET, 400 si malformed.
   - QEMU forwarding existente (`hostfwd=tcp::8080-:80`) sirve esto
     directamente; `curl http://localhost:8080/` desde Mac baja el
     HTML seed que mformat instaló al compilar sd.img.
   - sd.img seed: index.html (~690 bytes) con HTML decorado en
     español + CSS inline + listado del data path osnos.
   - Verificado: curl baja el index.html con HTTP/1.0 200 OK +
     headers correctos. Browsers (Safari/Chrome) renderizan la
     página con CSS. Multi-cliente (varias curls en sucesión) anda
     gracias al fix volatile/memory-barrier — sin el fix, accept
     se quedaba pegado a partir de la segunda conexión.

TODO opcional 8.5.11 retransmisión + RTT + congestion control real
TODO opcional 8.5.12 IPv6 (struct sockaddr_in6 + state machine)
TODO opcional 8.5.13 select() / accept blocking real via task suspend
                       (hoy es busy-poll en kernel; las syscalls que
                       blockean por mucho tiempo no dejan correr otros
                       servers cooperativos sin libc wrapping con
                       nanosleep como hicimos en select)

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
