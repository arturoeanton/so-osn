# OSnOS — Arquitectura

Sistema operativo microkernel x86_64 minimalista. Boot por Limine.
**Post-FASE 10**: console + keyboard + shell viven como ELFs ring-3
(`consrv`, `kbdsrv`, `shellsrv`); el kernel solo arranca drivers,
spawnea los 3 servers y entra al scheduler. fs_server fue eliminado
(la shell habla VFS directo). User tasks corren en ring 3 con su
propio address space y entran al kernel vía `syscall` (preferido) o
`int 0x80` (legacy compat).

Scheduler preemptivo timer-driven (50 ms quantum, sólo CPL=3) sobre
un loop cooperativo en ring 0. **ABI POSIX core 100% COMPLETO**:
`fork(2)` (SYS_FORK=57) + `execve(2)` (SYS_EXECVE=59) + `wait(2)/
waitpid(2)` (SYS_WAIT4=61, con nuevo estado TASK_ZOMBIE para
preservar exit_code) + `sigaction(2)` (SYS_RT_SIGACTION=13,
sa_handler-only model con sigframe en user stack +
SYS_RT_SIGRETURN=15 epilogue via libc __sigtramp) + EINTR estándar
en blocking syscalls. También `osn_spawn` (SYS_SPAWN=266, fork+exec
atómico estilo posix_spawn con fd inheritance MOVE-semantics) para
casos donde fork+exec sería overkill.

**Disk-resident** (Fase 2): sd.img se popula al build con todos
los ELFs (~64) via mtools. El kernel solo embebe consrv/kbdsrv/
shellsrv/banner como ROM recovery. Kernel binary: 7.6 MB → 1.1 MB.

## Capas (post-FASE 10)

```
+----------------------------------------------------------------+
| ring-3 servers (FASE 10) — ELFs en elfs/osn-server/            |
|   consrv   — IPC_CONSOLE_WRITE / CLEAR → /dev/fb0              |
|   kbdsrv   — /dev/input0 → sys_tty_input (POSIX termios)       |
|   shellsrv — line editor + history + pipes/redirects + jobs   |
|              (registra SERVER_SHELL, ES EL shell del OS)       |
+----------------------------------------------------------------+
| ring-3 user tasks ~60 ELFs en /bin (coreutils + net + tests)   |
|   ls cat cp mv rm mkdir touch echo head tail wc grep sort      |
|   uniq cut tr seq yes tee env pwd which printf date uname      |
|   basename dirname clear tree banner calc top kill sleep ovi   |
|   hello hello_libc tcc — httpd selectserver echotcp tcpclient  |
+----------------------------------------------------------------+
|                 lib/libc — osnos mini-libc                      |
|   stdio (FILE*, printf, fopen, fread/fwrite, fgets, snprintf), |
|   stdlib (malloc/free, qsort, atoi/strtol, atexit, setjmp),    |
|   string (mem*, str*, strdup, strstr, strtok_r), unistd (read, |
|   write, open, close, pipe, dup, dup2, fcntl, mmap, ...),      |
|   sys/socket (TCP/UDP/select), arpa/inet, time, errno, crt0    |
|              ↓                                                   |
|             syscall (via inline asm in syscall.h)               |
+----------------------------------------------------------------+
| Syscall ABI Linux x86_64 + osnos-specific (≥ 250):             |
|   read/write/open/close/lseek/fstat/mmap/munmap/brk/pipe/      |
|   dup/dup2/nanosleep/getpid/socket/bind/listen/accept/connect/ |
|   send/recv/select/fcntl/getcwd/chdir/mkdir/rmdir/rename/      |
|   unlink/ioctl/getdents64/gettimeofday/time/kill/exit/         |
|   fork (#57) execve (#59) wait4 (#61) — POSIX core              |
|   rt_sigaction (#13) rt_sigprocmask (#14) rt_sigreturn (#15)   |
|   setpgid (#109) getppid (#110) getpgrp (#111) setsid (#112)   |
|   getpgid (#121) getsid (#124) — job control                   |
|                                                                 |
|   osnos: IPC_SEND (260) IPC_RECV (261) SERVICE_REGISTER (262)  |
|   SERVICE_LOOKUP (263) TTY_INPUT (264) TASKINFO (265)          |
|   SPAWN (266) SET_FG (267) RESUME (268)                        |
|                                                                 |
|       int 0x80 (IDT[0x80] DPL=3)      syscall  (LSTAR=entry)   |
|              int80_entry asm                    syscall_entry  |
|                       \                          /              |
|                        syscall_dispatch(frame)                  |
+----------------------------------------------------------------+
| VFS layer:                                                      |
|   ramfs (/)  sysfs (/sys)  devfs (/dev: null/zero/fb0/input0)  |
|   aliasfs (/home → /sd/home  AND  /bin → /sd/bin)              |
|   binfs (/bin fallback diskless)                               |
|   fat16 (/sd, sd.img — read/write con dir-chain extension)     |
+----------------------------------------------------------------+
| IPC layer:                                                      |
|   ipc_send / ipc_recv (cola compartida 64 × 1 KB)              |
|   ipc_send rewrite msg.to: SID → pid (receivers filtran        |
|     por su propio pid via sys_ipc_recv → ipc_recv(t->pid))     |
|   service_register / service_lookup (SERVER_* SIDs)            |
+----------------------------------------------------------------+
| net/ stack (FASE 8.5):                                          |
|   socket (UDP + full TCP state machine, accept queue, retx RTO)|
|   tcp → ip → eth (rtl8139)  +  arp (cache + ARP_TIMEOUT poll)  |
|   icmp + udp delivered through socket layer to ring-3          |
|   DNS resolver + getaddrinfo (slirp 10.0.2.3)                  |
+----------------------------------------------------------------+
| micro/ core:                                                    |
|   task (16 slots; per-task fds[16] thin slots → OFD pool,      |
|     FPU state, mmap regions, cwd, kill/stop_pending,           |
|     stdin/stdout_redir, saved iret, parent_pid + wait_status_  |
|     ptr + wait_change (WUNTRACED/WCONTINUED tracking),         |
|     sa_handler[32] + sig_pending (sigaction), pgid + sid       |
|     (job-control), TASK_ZOMBIE state)                           |
|   fd (per-task slot {used, ofd_idx, fd_flags=CLOEXEC}) +       |
|     global ofd_pool[128] shared open file descriptions —       |
|     refcounted; dup/dup2/fork share offset via OFD ref bump    |
|   pty (pool de 8 pty_pair_t — m2s/s2m ring buffers 4 KiB +     |
|     termios per-pair + canon line accumulator; /dev/ptmx +     |
|     /dev/pts/N via OFD with is_pty + pty_side)                  |
|   scheduler (preempt CPL=3 + cooperative + resume_jump)        |
|   reaper (kstack free queue + DEAD→UNUSED reaping)             |
|   ipc, service, fd, pipe, fpu, extable, uaccess (fault-recov)  |
|   syscall, syscall_msr, syscall_entry, int80, tss, gdt, idt    |
|   pmm, vmm, kmalloc — paging, heap, per-task address spaces    |
+----------------------------------------------------------------+
| proc/ layer:                                                    |
|   builtin (registry de ELFs embedded como ROM fallback)         |
|   exec (proc_execve, proc_exit_current_user)                   |
|   elf (Elf64 loader; PT_LOAD → page-by-page map + zero-fill)   |
+----------------------------------------------------------------+
| drivers/:                                                       |
|   keyboard (PS/2; scancodes + Shift + Ctrl + ext + arrows)     |
|   framebuffer (linear FB + 8x8 font + VT100 CSI parser:        |
|     ESC[2J/H/r;cH/K/m/7m + SGR truecolor 38;2;R;G;B)           |
|   rtl8139 (PCI; PIO+DMA; 4 TX slots, RX ring; IRQ stub)        |
|   block_ata (ATA PIO over IDE primary master, LBA28)           |
|   pic / lapic / timer (PIT @ 100 Hz for preempt)                |
+----------------------------------------------------------------+
| Kernel-side cooperative tasks (NO servers, dos helpers):       |
|   keyboard feeder — keyboard_poll → devfs_input_push           |
|     (no toca políticas TTY — eso vive en ring 3 kbdsrv)         |
|   init-respawn — cada ~100ms verifica que consrv/kbdsrv/       |
|     shellsrv sigan vivos; respawnea si murieron (cubre el      |
|     post-exec gap). Sleep via state=BLOCKED + wakeup_at_ms.    |
+----------------------------------------------------------------+
|                       Limine bootloader                         |
+----------------------------------------------------------------+
```

## Flujo de una tecla típica (post-FASE 10)

```
   user types 'l'
        |
        v
   PS/2 controller
        |  scancode 0x26 in port 0x60
        v
   [ring 0]  keyboard_server_tick() — kernel cooperative task
        |  reads scancode, applies shift/ctrl/extended state
        |  builds keyboard_event_t { ascii='l', keycode=0 }
        |  devfs_input_push(ev) — append to /dev/input0 ring buffer
        |  (NO IPC anymore — kbdsrv polls /dev/input0)
        v
   scheduler dispatches next task
        |
        v
   [ring 3]  kbdsrv (ELF en elfs/osn-server/kbdsrv.c)
        |  read("/dev/input0", &ev, sizeof(ev))   ← syscall
        |  sys_tty_input('l') — feeds the kernel-side TTY's input ring
        |  (sys_tty_input #264; only the SERVER_KEYBOARD pid is allowed)
        |  (no más IPC_KEY_EVENT a SERVER_SHELL — shellsrv lee del TTY)
        v
   [ring 3]  shellsrv (ELF en elfs/osn-server/shellsrv.c)
        |  read(0, &c, 1)   ← blocking syscall on stdin (raw mode termios)
        |  sys_read → TTY input ring → returns 'l'
        |  shellsrv stores 'l' in line buffer, renders prompt+cursor
        v
   printf-equivalent (libc) → write(1, "l", 1)   ← syscall
        |  sys_write fd=1 (default) → write_to_console (kernel)
        v
   ipc_send(to=SERVER_CONSOLE, type=IPC_CONSOLE_WRITE,
            arg0=color, data="l", arg1=1)
        |  ipc_send rewrite msg.to: SERVER_CONSOLE (SID) → consrv_pid
        |  task_unblock(consrv_pid)
        v
   [ring 3]  consrv (ELF en elfs/osn-server/consrv.c)
        |  sys_ipc_recv → ipc_recv(t->pid, &msg) matches the queued msg
        |  if color != white: write(fb_fd, "\x1b[38;2;R;G;Bm", ...)
        |  write(fb_fd, msg.data, msg.arg1)        ← syscall on /dev/fb0
        |  write(fb_fd, "\x1b[39m", 5)
        v
   [ring 0]  devfs fb0_write → framebuffer_write_bytes(buf, n, color)
        |  framebuffer_draw_string per chunk (with safe-split for CSI)
        |  pixel writes to linear framebuffer
```

**Cambios clave vs pre-FASE 10**:
- Política TTY (canonical/raw/echo/ISIG) ahora vive en kernel TTY layer
  pero la **entrega** la hace kbdsrv ring-3 vía SYS_TTY_INPUT.
- shellsrv lee bytes con `read(0)` igual que cualquier programa POSIX
  en modo raw — no recibe IPC_KEY_EVENT.
- consrv ring-3 wrappea cada write con SGR truecolor si arg0 != white,
  y hace el syscall a `/dev/fb0` (devfs char device).
- Toda la cadena pasa ~4 veces ring 3 ↔ ring 0; el cost extra vale por
  la modularidad (los servers son ELFs reemplazables en /sd/bin/).

## Flujo de un comando: `ls /home` (post-FASE 10)

`ls` ahora es un ELF independiente en `/bin/ls` (no un builtin del
shell). shellsrv lo spawnea con `osn_spawn` y le inherita stdin/stdout.

```
[ring 3]  shellsrv recibe '\n' del read_line raw-mode
   |
   |  history_save("ls /home")
   |  dispatch("ls /home")
   |  expand_vars (sin $VAR aquí) + line_split_stages + stage_parse
   |  argv = ["ls", "/home"]
   |
   v
[ring 3]  shellsrv: cmd not in COMMANDS[] (builtins) → es ELF externo
   |  resolve_cmd_path("ls", ...) → busca por $PATH:
   |     stat("/bin/ls") → OK → path = "/bin/ls"
   |  pack_envp(envbuf, ...) → "PATH=/bin\0HOME=/home\0SHELL=...\0\0"
   |  osn_spawn("/bin/ls", "/home", envp, stdin_fd, stdout_fd)
   v
[ring 0]  sys_spawn (#266)
   |  proc_execve("/bin/ls", "/home", envp_array)
   |  /bin/ls → aliasfs → /sd/bin/ls (FAT16)
   |  vfs_stat(/sd/bin/ls) → exists → vfs_read(blob) → elf_load(blob)
   |  fd inheritance: child fds[0]=src_stdin, fds[1]=src_stdout (MOVED)
   |  task_create_user_elf, returns child_pid
   v
[ring 3]  shellsrv: wait_pid_or_stop(child_pid)
   |  loop with sys_taskinfo(265) hasta state == TASK_DEAD or STOPPED
   |  (yield via nanosleep entre iteraciones)
   v
[ring 3]  /bin/ls main(argc=2, argv=["ls","/home"], envp=...)
   |  opendir("/home") → sys_open (DIR)
   |  loop readdir → sys_getdents64
   |     ramfs (or aliasfs → fat16 if /home on FAT) drives the listing
   |  for each entry: printf("%s\n", name)   ← stdio buffered
   |  exit(0) → atexit + fflush(stdout) → _exit syscall #60
   v
printf chain: stdio buffer → write(1, buf, n)
   |  sys_write fd=1 → write_to_console
   |  ipc_send(SERVER_CONSOLE→consrv_pid, IPC_CONSOLE_WRITE, ...)
   v
[ring 3]  consrv processes IPC, writes to /dev/fb0 (framebuffer)
   v
[ring 0]  proc_exit_current_user(0) on /bin/ls
   |  destroy AS, mark task DEAD, sched_resume_jump
   |  reaper queue picks up kstack on next tick
   v
[ring 3]  shellsrv sees TASK_DEAD, prints next prompt
```

**Notas**:
- fs_server (ring-0 ipc wrapper que existió hasta FASE 10.3) está
  **borrado**. Los ELFs invocan VFS directamente vía syscalls (open,
  read, write, getdents64, stat, etc).
- Builtins del shellsrv (`cd`, `pwd`, `help`, `exit`, `export`, `jobs`,
  `fg`, `bg`, `kill`, `history`, `test`) no spawnean ELFs — corren
  in-process en shellsrv ring-3.
- Cualquier comando NO en COMMANDS[] es tratado como nombre de ELF y
  resuelto via $PATH (default `/bin`).

## Flujo de un GET HTTP entrante (`curl http://localhost:8080/`)

```
host curl: TCP SYN -> 10.0.2.2:8080 (slirp gateway from guest's POV)
   |
   |  slirp NATs and bridges packet to guest @ 10.0.2.15:80
   v
RTL8139 chip receives frame
   |  asserts IRQ 11
   v
rtl8139_irq_entry asm stub -> rtl8139_irq_handle (C)
   |  reads ISR, drain_rx() walks RX ring
   |  passes each frame to net_rx() with ethertype
   v
net_rx -> ip_handle (validates header, checksum) -> tcp_handle
   |
   v
sock_tcp_handle_segment(src, dst, seq, ack, flags, payload)
   |
   |  2-pass lookup: matched LISTEN socket (TCP/80)
   |  state machine: LISTEN + SYN -> alloc_child_for_syn
   |  child socket initialized: SYN_RCVD, ISN, parent_sd
   |  tcp_send(SYN-ACK)  via ip_send -> eth_send -> rtl8139_tx
   |
   |  ...host ACKs the SYN-ACK...
   |  state machine: SYN_RCVD + ACK -> ESTABLISHED
   |  push child idx into parent's accept_q
   |
   |  ...host sends "GET / HTTP/1.0\r\n..."...
   |  state machine: ESTABLISHED + data -> enqueue in tcp_rx, ACK back
   v
[ring 3] httpd's accept() is busy-looping in libc:
   |  syscall(SYS_ACCEPT) -> sock_accept -> dequeue -> returns child fd
   |  syscall(SYS_RECVFROM) -> sock_recv -> drains tcp_rx
   |
   |  parses "GET /sd/index.html"
   |  open("/sd/index.html") -> VFS -> fat_vfs_read -> block_ata_read_sector
   |
   |  send(fd, header) + stream chunks via send(fd, chunk)
   v
syscall(SYS_SENDTO) -> sock_send
   |  tcp_send for each MSS-sized chunk
   |  saves last segment in retx_buf for RTO retransmission
   v
ip_send -> eth_send -> rtl8139_tx
   |  rtl8139_tx hunts free slot (avoids tx_cur saturation)
   |  writes packet to TX_BUF, fires TSD with size
   |  chip transmits + TOK IRQ when done
   v
host curl receives the response, renders HTML.
   |  half-close (FIN) from curl -> we transition ESTABLISHED -> CLOSE_WAIT
   |  httpd close(fd) -> sock_close_tcp -> emit FIN+ACK, LAST_ACK
   |  host's ACK of our FIN -> CLOSED, slot freed via zombie path
```

## Contratos clave

### IPC (`src/include/osnos_ipc_abi.h`, kernel impl `src/micro/ipc.{c,h}`)

- Mensajes de tamaño fijo (`ipc_msg_t`: 1024B data + arg0/arg1/from/to/
  type), copiados al `ipc_send`. Sender puede descartar su buffer en
  el momento que retorna.
- Opcodes en rangos numéricos (ABI compartida kernel ↔ ring-3):
  - `0x00–0x0F` sistema (KEY_EVENT, COMMAND_RUN)
  - `0x10–0x1F` consola (CONSOLE_WRITE, CONSOLE_CLEAR)
  - `0x20–0x3F` fs / vfs (legacy — fs_server eliminado en FASE 10.3)
  - `0x40–0x5F` process lifecycle (PROC_EXITED, PROC_EXITED_USER)
  - `0x60+` reservado para nuevos
- SID enum compartido: `SERVER_KEYBOARD=1`, `SERVER_SHELL=2`,
  `SERVER_CONSOLE=3`, `SERVER_FS=4` (libre desde 10.3).
- **SID→pid rewrite en ipc_send** (FASE 10): `msg->to` originalmente
  es un SID; `ipc_send` resuelve a pid vía `service_get_pid` y
  reescribe la copia queued. Los ring-3 receivers (sys_ipc_recv)
  filtran por `t->pid`, NO por SID. Sin este rewrite, los consumers
  ring-3 nunca matcheaban sus propios mensajes.
- Convención de respuesta:
  - `arg0` = `osnos_status_t` (0 = OK, >0 = errno-like).
  - `arg1` = tamaño del payload o conteo cuando aplica.
  - `data` = payload textual o binario (null-terminated cuando es
    texto; explicit `arg1` cuando es binario o color-prefijado).
- `ipc_send` retorna `OSNOS_OK` / `OSNOS_EAGAIN` (queue full) /
  `OSNOS_ESRCH` (target no registrado). Callers DEBEN chequear.
- Ring-3 receivers (consrv/kbdsrv/shellsrv) usan `ipc_recv_block`
  libc wrapper que loopea sobre SYS_IPC_RECV (#261) con nanosleep
  entre EAGAIN — no se bloquean kernel-side.

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

### Shell (`elfs/osn-server/shellsrv.c` — ring 3 post-FASE 10.4)

- Es un ELF normal cargado por proc_execve en kmain. Registra
  SERVER_SHELL via SYS_SERVICE_REGISTER.
- **raw-mode TTY**: tcgetattr/tcsetattr para deshabilitar ICANON+ECHO
  y leer un byte a la vez con read(0). Procesa CSI manualmente
  (`ESC[A/B/C/D` para flechas, `0x7F`/`\b` para backspace).
- Comandos vía tabla `COMMANDS[]` con structs `{ name, fn, help }`.
  El `help` se genera iterando la tabla. Incluye: `help`, `exit`,
  `pwd`, `cd`, `ls`, `cat`, `echo`, `history`, `test`, `jobs`,
  `fg`, `bg`, `kill`, `export`, `unset`, `setenv`, `exec`.
- **`exec CMD [args]`** builtin — calls `execve(2)` after
  `osn_set_fg(getpid())` so Ctrl+C delivers to the new image
  post-swap. If exec fails (path not found etc), shellsrv stays
  alive and reports errno.
- **$VAR / ${VAR} expansion** (post-FASE 10.4): `expand_vars` walk
  + substitución antes del parsing del pipeline. Soporta `\$` escape.
- **Pipeline parser** (`line_split_stages` + `stage_parse` +
  `run_pipeline`): split en `|`, cada stage soporta `< file`,
  `> file`, `>> file`. Background con trailing `&`.
- **Operadores de secuencia**: `dispatch` top-level splittea la
  línea en `;`, `&&`, `||` antes del pipeline parsing. Cada
  segmento corre via `dispatch_segment`. Decisiones:
  - `;`  → siempre corre el siguiente
  - `&&` → corre solo si `last_status == 0`
  - `||` → corre solo si `last_status != 0`
  - `last_status` se actualiza después de cada ejecución (builtin
    return value, o `wait_pid_capture` para externos).
- **`$?` substitution** en expand_vars — formatea `last_status`
  como decimal.
- **Glob `*`** en stage_parse (`expand_glob_into`): tokens con `*`
  se machean contra entradas del dir implícito (dir prefix o "."
  si no hay slash). Matcher recursivo (`glob_match`) soporta `*`
  con captura greedy. Sin matches → token literal (bash default).
  Storage en `glob_buf[4096]` estático.
- **`do_ls` POSIX multi-arg**: pase 1 imprime files (sin header),
  pase 2 lista dirs (con header `PATH:` si hay >1 path).
- **History**: ring buffer de 16 entradas con dedup consecutivo,
  navegación con flechas up/down, persistencia en `/home/.history`.
- **.oshrc autoload**: shellsrv ejecuta `/home/.oshrc` línea por
  línea al startup. Default seedea `export PATH=/bin / HOME=/home /
  SHELL=/bin/shellsrv / OSNAME=osnos`.
- **Path resolution**: `path_normalize` para `cd ..` / `cd .` /
  paths relativos; `resolve_cmd_path` para PATH search.
- **fd inheritance**: cuando spawnea un child, copia stdin/stdout
  fds del shellsrv al child via SYS_SPAWN (MOVES, no COPY — el
  caller pierde los slots para pipes).
- **Job control**: bg_jobs[] track pids + cmd labels. Ctrl+C envía
  SIGINT al fg task (kernel_fg_pid set via SYS_SET_FG #267); Ctrl+Z
  hace stop pending → TASK_STOPPED. `fg <pid>` hace SYS_RESUME #268
  + osn_set_fg(pid) + wait. `kill <pid>` hace sys_kill (también
  despierta STOPPED tasks).

## Boot sequence (post-FASE 10)

`kmain` en `src/kernel/main.c`:

1. Validar Limine base revision y framebuffer; `framebuffer_init`.
2. Memoria: `pmm_init` → `vmm_init` → `kheap_init` (slab + dynamic
   growth hasta 4 MiB).
3. CPU: `gdt_init` → `tss_init` → `idt_init` → `uaccess_init`
   (registra el span de copy_*_user en la extable) → `syscall_msr_init`
   (habilita EFER.SCE + programa STAR/LSTAR/FMASK).
4. Interrupciones: `pic_init` → `lapic_init` → `timer_init` (PIT @ 100
   Hz). `block_ata_init` corre IDENTIFY contra primary IDE; si hay
   disco, se monta luego FAT16 en `/sd`.
5. Microkernel: `ipc_init` → `pipe_init` → `task_init` → `reaper_init`
   → `scheduler_init` → `syscall_init` → `ramfs_init` (slots vacíos)
   → `bootstrap_fs`.
6. `bootstrap_fs`:
   - Monta `/`, `/sys`, `/dev` (synthetic backends).
   - Si hay FAT mounted: monta `/sd`. **Fase 2 disk-resident**:
     `sd.img` ya viene con `/bin/*` poblado por el build script
     (GNUmakefile + mtools), y `/home/{README.TXT,HELLO.TXT,
     .oshrc}` también pre-cargado. `bootstrap_fs` solo hace `mkdir
     /sd/bin` (idempotente) y dumpea los 4 ROM ELFs si faltan
     (recovery path). Monta aliasfs `/bin → /sd/bin` y
     `/home → /sd/home`.
   - Diskless: monta binfs sintético en `/bin` (read-only sobre el
     ROM set: consrv/kbdsrv/shellsrv/banner + user_hello) y crea
     `/home` ramfs.
7. `task_create("keyboard", keyboard_server_tick)` — kernel-side
   feeder (poll PS/2 → /dev/input0 ring buffer).
8. Spawn los 3 servers ring-3 vía `proc_execve` + `service_register`:
   ```c
   int64_t consrv_pid   = proc_execve("/bin/consrv",   "", 0);
   service_register(SERVER_CONSOLE,  (uint64_t)consrv_pid);
   int64_t kbdsrv_pid   = proc_execve("/bin/kbdsrv",   "", 0);
   service_register(SERVER_KEYBOARD, (uint64_t)kbdsrv_pid);
   int64_t shellsrv_pid = proc_execve("/bin/shellsrv", "", 0);
   service_register(SERVER_SHELL,    (uint64_t)shellsrv_pid);
   ```
   Pre-registro evita la race: si shellsrv envía IPC antes de
   auto-registrarse, ya tiene el SID resuelto en service_pid[].
9. `task_create("init-respawn", server_respawn_tick)` — watchdog
   kernel task que cada ~100ms verifica los 3 servers y los
   respawnea si murieron. Sleep usando `state=BLOCKED +
   wakeup_at_ms = timer_ms()+100`. Resuelve el cuelgue post
   `exec /bin/foo` (foo termina → shellsrv slot UNUSED → no shell
   → sin watchdog quedaba colgado).
10. `keyboard_server_init()` (PS/2 hardware init).
11. `__asm__("sti")` — habilita IRQs. Timer empieza a contar.
12. `scheduler_loop`: guarda resume point (longjmp host) y entra a
    `for(;;) scheduler_tick()`. Cada tick:
    - `reaper_drain` (libera kstacks de tareas muertas, reapja
      DEAD → UNUSED)
    - `task_check_wakeups` (BLOCKED + wakeup_at_ms reached → READY)
    - dispatchea la próxima READY (round-robin).

    Las tasks ring-3 son **preempt cada 50 ms** vía timer IRQ
    (FASE 9 — sólo cuando CPL=3 entrando al handler; tasks kernel-
    side siguen siendo cooperativas).

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
  - syscall_dispatch(frame) → sys_write
  - sys_write → write_to_console (kernel helper)
  - ipc_send(SERVER_CONSOLE → consrv_pid, IPC_CONSOLE_WRITE, ...)
  - return retval en rax
  - (ring-3 consrv eventualmente recibe el IPC y escribe a /dev/fb0)
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

## libc

`lib/libc/` es una mini-libc local (~700 LOC). Se compila aparte del
kernel con `USER_CFLAGS` (sin `-mcmodel=kernel`), se empaqueta en
`libosnos_c.a` + un `crt0.S.o` standalone, y se linka contra cada ELF
user que la quiera.

```
elfs/tests/hello_libc.c
    │
    │ clang USER_CFLAGS -I lib/libc/include
    ▼
hello_libc.o
    │
    │ ld.lld -T elfs/libc.lds
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
shellsrv:/$ hello_libc
   │  (shellsrv resolve_cmd_path via PATH → /bin/hello_libc)
   │  (osn_spawn → SYS_SPAWN → proc_execve)
   ▼
proc_execve → task_create_user_elf → elf_load(blob,...)
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
        ↓ crt0 calls exit(0) (NO _exit directo — pasa por exit
        ↓   para que atexit() + fflush(stdout) corran)
        ↓ exit → _exit → syscall #60
        ↓ proc_exit_current_user → AS destroy + IPC_PROC_EXITED + sched_resume_jump
   ↓
shellsrv (wait_pid_or_stop) ve TASK_DEAD, imprime prompt nuevo
```

## ELF loader

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

## Disk-resident /bin (Fase 1 + Fase 2 FINAL)

La canonical store de los ELFs es **FAT16** en `/sd/bin/`. El kernel
solo embebe un **ROM recovery set** mínimo (4 ELFs críticos +
bare user_hello). El resto vive exclusivamente en disco. Kernel
binary pasó de **7.6 MB → 1.1 MB** (-85%).

```
build (host):
   GNUmakefile target $(SD_IMG) depende de USER_ELF_LIST.
   ↓
   mformat -i sd.img ::
   mmd     -i sd.img ::/bin ::/home
   for elf in $(USER_ELF_LIST):
       name = basename $$elf .elf
       mcopy -i sd.img $$elf ::/bin/$$name      # 64 ELFs, no .elf
   mcopy /home/{README.TXT,HELLO.TXT,.oshrc}    # seed user files
   ↓
kernel link:
   solo objcopy → .elf.o los 4 ROM:
     consrv kbdsrv shellsrv banner + user_hello (bare)
   builtin.c registry refleja ese set mínimo
   ↓
boot:
   bootstrap_fs detecta FAT16 en /sd, monta /sd via fat_vfs_ops.
   `mkdir /sd/bin` idempotente (ya existe del build).
   `seed_disk_bin()` itera builtins[] (4 entradas) — vfs_stat ve
   que ya existen en /sd/bin/* → skip. Solo si el disco está vacío
   o corrupto se re-escriben los 4 ROMs.
   aliasfs /bin → /sd/bin   (todo el set de 64 ELFs accesible)
   aliasfs /home → /sd/home
   ↓
runtime:
   exec.c proc_execve("/bin/hello"):
     1. vfs_stat("/bin/hello") → aliasfs → /sd/bin/hello en FAT → OK
     2. vfs_read(blob), elf_load(blob), task_create_user_elf
     3. Si vfs_stat falla (archivo no en disco):
        fallback a builtin_find("hello") → solo funciona para los
        4 ROM. Para los otros 60 ELFs no hay fallback (by design).
```

**FAT16 NT case-bits**: los SFNs en FAT16 se guardan uppercase pero
el byte 0x0C del dirent tiene bits 0x08 (base lowercase) y 0x10
(ext lowercase) que Windows 95+ y mtools setean. Nuestro
`name_from_83` los honra → `hello` no vuelve como `HELLO` y los
matchers case-sensitive (glob, strcmp) funcionan.

**FAT16 dir-chain extension**: `extend_dir_chain` alloca un cluster
nuevo y lo encadena al dir cuando find_free_dir_slots_run pega
ENOSPC. Permite subdirs grandes (los 64 ELFs viven en `/sd/bin/`
sin problema).

## Limitaciones conscientes (post-FASE 10)

- **Preempción solo en CPL=3**: tasks ring-0 (keyboard feeder)
  siguen cooperativas. Un loop infinito en kernel-side cuelga.
  Pero el feeder es simple: lee scancodes y empuja a ring buffer,
  no hay paths peligrosos. Fix futuro = scheduler en lockless
  IRQ-driven sin loops cooperativos.
- **Cola IPC única compartida** (64 slots × 1 KB). Un server
  ruidoso puede llenarla y bloquear a los demás con EAGAIN.
  Mitigaciones aplicadas: kbdsrv ya no envía IPC_KEY_EVENT
  (shellsrv lee del TTY directo); ovi bufferea su render en 16 KB
  + single-write. Fix real = cola por servidor o backpressure
  explícito por sender.
- **VFS sin permisos**: no hay uid/gid, no atime/mtime, no chmod.
  FAT16 tiene attrs limitadas y aliasfs los pasa transparente.
- **ABI POSIX core 100% completo**: `fork(2)` + `execve(2)` +
  `wait/waitpid(2)` (con TASK_ZOMBIE) + `sigaction(2)` (sa_handler-
  only, sigframe-based delivery) + EINTR en blocking syscalls.
  Fork sigue siendo full page copy (no COW todavía). osn_spawn
  (#266) coexiste para el caso atómico optimizado.
- **Signals user-mode**: `sigaction(2)` con sa_handler-only funciona
  (`SYS_RT_SIGACTION=13`, sigframe en user stack, libc `__sigtramp`
  + `SYS_RT_SIGRETURN=15`). Default disposition correctly applies
  (SIG_DFL → `proc_exit_current_user(128+sig)`). SIGKILL/SIGSTOP
  uncatchable. **Pendiente**: `sa_mask` / `sigprocmask` reales
  (hoy stub no-op); `SA_SIGINFO`+`siginfo_t`; `SIGCHLD` automático
  al child exit; signals para faults (SEGV/FPE — hoy proc_exit duro).
- **Sin TLS / FS register / thread-local storage**.
- **Sin loader dinámico**. Solo `ET_EXEC` estático.
- **Solo 16 tasks** simultáneas (MAX_TASKS = 16).
- **No SMP**: 1 core, 1 LAPIC.
- **Solo IDE primary master** (ATA PIO LBA28, 1 disco).

## Flujo de un pipeline: `ls /home | grep TXT | sort`

```
[ring 3]  shellsrv.run_pipeline(stages=[ls, grep, sort])
   |
   |  Crea 2 pipes (uno entre cada par de stages):
   |    pipe(p01) → p01[0]=read, p01[1]=write (ls→grep)
   |    pipe(p12) → p12[0]=read, p12[1]=write (grep→sort)
   |
   |  Spawn stage 0: "ls /home"
   |     osn_spawn("/bin/ls", "/home", envp,
   |               stdin_fd=-1 (default),   ← terminal stdin
   |               stdout_fd=p01[1])         ← write a pipe 0→1
   |     close(p01[1]) en shellsrv (el child se lo lleva)
   |
   |  Spawn stage 1: "grep TXT"
   |     osn_spawn("/bin/grep", "TXT", envp,
   |               stdin_fd=p01[0],          ← read pipe 0→1
   |               stdout_fd=p12[1])         ← write pipe 1→2
   |     close(p01[0]); close(p12[1])
   |
   |  Spawn stage 2: "sort"
   |     osn_spawn("/bin/sort", "", envp,
   |               stdin_fd=p12[0],          ← read pipe 1→2
   |               stdout_fd=-1)             ← default stdout
   |     close(p12[0])
   |
   |  Wait the LAST pid (sort's). Cuando sort cierra, los anteriores
   |  ya cerraron porque sus stdout pipes vieron EOF.
   v
[ring 0]  sys_spawn implementation (en src/micro/syscall.c):
   |  proc_execve crea la child task. Después:
   |  if (stdin_fd >= 0)  child.fds[0] = caller.fds[stdin_fd];   MOVE
   |                       caller.fds[stdin_fd].used = false;
   |  (mismo para stdout). MOVES, no COPIES — el caller pierde el
   |  slot para que la cuenta de referencias del pipe sea correcta.
   v
ring-3 stages corren en paralelo, scheduler los preempta cada 50 ms.
Cuando uno termina con exit(0), proc_exit_current_user libera fds
del child (decrementa pipe refcounts; al llegar a 0, los siguientes
read() en el read-end devuelven 0 = EOF).
```

## Flujo de un osn_spawn directo (sin pipes)

```
[ring 3]  shellsrv: dispatch("hello")
   |  resolve_cmd_path("hello") via PATH → "/bin/hello"
   |  pack_envp(envbuf) → "PATH=/bin\0HOME=/home\0...\0\0"
   |  osn_spawn("/bin/hello", "", envp, -1, -1)
   v
[lib/libc/include/osnos_ipc.h] inline:
   osnos_syscall5(SYS_SPAWN, path, args, envp, -1, -1)
   v
[ring 0]  sys_spawn:
   |  caller = task_current() (= shellsrv)
   |  copy_from_user para path / args / envp_flat (todo a kernel scratch)
   |  unpack envp_flat ("k=v\0k=v\0\0") → envp_array[MAX]
   |  child_pid = proc_execve(path_kbuf, args_kbuf, envp_array)
   |  if (stdin_fd  >= 0) move caller.fds[stdin_fd]  → child.fds[0]
   |  if (stdout_fd >= 0) move caller.fds[stdout_fd] → child.fds[1]
   |  return child_pid (positive) o -errno
   v
[ring 3]  shellsrv ve pid > 0:
   |  if background ("&"):  bg_jobs_remember(pid, cmd_label); return
   |  else:                  osn_set_fg(pid); wait_pid_or_stop(pid)
```

## Flujo de un fork (SYS_FORK = #57 Linux)

`fork` crea un task nuevo idéntico al actual (mismo memory image,
mismas fds, mismo cwd), con la única diferencia de que ambos
retornan distinto: parent ve el child pid, child ve 0.

```
[ring 3]  pid_t r = fork();
   |
   |  libc: osnos_syscall0(SYS_FORK=57)
   v
[ring 0]  sys_fork (src/micro/syscall.c):
   |
   |  parent = task_current()
   |
   |  --- alocaciones (todo o nada) ---
   |  child_pml4   = address_space_clone(parent->pml4)
   |     ↓ walk pml4[0..255] → pdpt → pd → pt → leaf
   |     ↓ for each present leaf: pmm_alloc_page() + memcpy via HHDM
   |     ↓ vmm_map(child_pml4, virt, new_phys, flags)
   |  child_kstack = kmalloc(USER_KSTACK_BYTES)
   |  child_pid    = task_create(parent->name, parent->entry)
   |     (cualquiera de los 3 falla → free los anteriores + return -ENOMEM)
   |
   |  --- estado per-task ---
   |  child->pml4 / kernel_stack / heap / mmap / fpu = parent's
   |  child->cwd / redirs / fds[0..15] = parent's
   |     para cada fd is_pipe: pipe_dup_reader/writer (++ ref_w/r)
   |  child->kill_pending / stop_pending = 0  (clean slate)
   |
   |  --- snapshot del syscall context ---
   |  iret = parent->kernel_stack_top - 40
   |  child->saved_iret_{rip,cs,rflags,rsp,ss} = iret[0..4]
   |  sf = iret - sizeof(syscall_frame_t)
   |  child->saved_{rbx,rcx,rdx,...,r15} = sf->{rbx,...}
   |  child->saved_rax = 0                   ← child sees fork()=0
   |  child->saved_valid = 1
   |  child->state = TASK_READY
   |
   |  return child->pid                       ← parent sees fork()=child_pid
   v
[scheduler] eventually dispatches the child:
   |  user_task_trampoline: saved_valid=1 → "replay" path
   |     - load CR3 = child_pml4 (kernel high-half intact, user low
   |       half is the cloned copy)
   |     - push saved_iret_{ss,rsp,rflags,cs,rip}
   |     - mov saved_rax → %rax  (0 in child!)
   |     - restore saved_rbx..r15
   |     - iretq
   v
[ring 3 — CHILD]
   |  arrives at the instruction RIGHT AFTER the syscall, with
   |  rax=0 (so libc fork() returns 0).
   |  All memory writes the child does from here on go to ITS pml4
   |  (the cloned copy), not the parent's.
```

**Invariants críticos**:
- Atómico: si `address_space_clone` o `kmalloc(kstack)` o
  `task_create` fallan, NO se modifica el parent. fork retorna
  -errno y el parent continúa normal.
- No COW: cada fork inmediatamente duplica TODAS las páginas user
  del parent. Para un proceso con 1 MiB de heap, fork cuesta 1 MiB
  de RAM física al instante. Suficiente para nuestros programas
  (~ 64 KiB típicos); optimizable a COW más adelante.
- fds compartidas (con refcount): pipe abierto por el parent → el
  child también lo ve. Cuando uno de los dos cierra, sólo decrementa
  el contador; el pipe vive hasta que ambos cierren.
- Sin SIGCHLD automático: el parent no recibe SIGCHLD cuando el
  child muere. Usa `wait(2)` / `waitpid(2)` (bloqueantes vía
  TASK_ZOMBIE + parent_pid + wake-up async desde
  `proc_exit_current_user`). forktest y waittest usan esta API
  POSIX-real. shellsrv todavía polea con `sys_taskinfo` para
  soportar `WUNTRACED` (Ctrl+Z), pero las apps user-mode usan
  `waitpid` directo. Cuando llegue SIGCHLD se podrá hacer el
  classic `signal(SIGCHLD, reaper); fork(); wait();` pattern.

## Flujo de un execve (SYS_EXECVE = #59 Linux)

A diferencia de `osn_spawn` que crea una task NUEVA, `execve` mata
el user-mode image actual y carga otro en su lugar, **manteniendo
el mismo pid**. Es el bloque "exec" de fork+exec.

```
[ring 3]  shellsrv usa `exec` builtin (do_exec):
   |  resolve_cmd_path(name) → /bin/foo via $PATH
   |  leave_raw()                ← cooked TTY para el nuevo image
   |  osn_set_fg(getpid())       ← claim foreground (pid preserved)
   |  execve("/bin/foo", argv, environ)
   v
[ring 3 libc] execve wrapper:
   |  osnos_syscall3(SYS_EXECVE=59, path, argv, envp)
   v
[ring 0]  sys_execve (src/micro/syscall.c):
   |  copy_from_user de path / argv[] (incluido strings) / envp[]
   |  args_kbuf = join argv[1..] con espacios
   |  envp_arr  = kernel array NULL-terminated
   |  proc_execve_replace(path, args_kbuf, envp_arr)
   v
[ring 0]  proc_execve_replace (src/proc/exec.c):
   |  resolve_executable(path) → blob (VFS first, ROM fallback)
   |  elf_load(blob) → new_pml4 / new_entry / new_stack_top
   |  build_argv_block(new_pml4, new_stack_top, name, args, envp)
   |     → init_rsp en el nuevo stack
   |
   |  --- todo OK, swap atómico ---
   |  old_pml4 = t->pml4
   |  t->pml4 = new_pml4
   |  t->user_entry / user_stack_top / heap_* / mmap_* / redirs reset
   |  t->saved_valid = 0          ← fresh start, no resume from prev
   |  task name = basename(path)
   |  fpu_state_init(t->fpu_state)
   |  address_space_destroy(old_pml4)   ← libera el AS viejo
   |
   |  sched_resume_jump()                ← never returns
   v
[scheduler] next dispatch entra a user_task_trampoline
   |  saved_valid=0 → "first-time" path → iretq con t->user_entry
   |  y t->user_stack_top (= init_rsp del nuevo image).
   v
[ring 3]  /bin/foo arranca en su _start, lee argv/envp del stack,
          llama a su main(). Mismo pid, mismos fds heredados, mismo
          cwd. shellsrv ya no existe — la ejecución continúa con
          el nuevo binario.
```

**Key invariants**:
- Sólo el user-mode portion del task cambia. PID, kstack, fds[16],
  cwd, slot index, kernel_fg_pid (si era ours) — todo igual.
- Es ATÓMICO: si elf_load falla, devolvemos -errno y el viejo image
  sigue vivo. Nada se destruye antes de que el nuevo esté listo.
- POST EXIT: el watchdog `init-respawn` detecta cuando el nuevo
  image (e.g. `/bin/top`) muere y trae shellsrv de vuelta. Sin él
  un `exec /bin/top` interactivo dejaría el sistema sin shell tras
  el Ctrl+C.

## Flujo de un wait (SYS_WAIT4 = #61) + TASK_ZOMBIE

`wait/waitpid` blocks the parent until a child enters TASK_ZOMBIE.
Lifecycle:

```
parent: pid_t r = waitpid(child, &status, 0);
   |  libc → osnos_syscall4(SYS_WAIT4, child, &status, 0, 0)
   v
[ring 0] sys_wait4:
   |  1st sweep: walk task table for tasks with parent_pid == self.
   |  - Si alguno está ZOMBIE: status = encode_wait_status(exit_code)
   |    (WIFEXITED << 8 ó WTERMSIG & 0x7f); copy_to_user; transición
   |    ZOMBIE → DEAD; return child pid. ✓
   |  - Si no hay ZOMBIE pero hay children vivos: WNOHANG → 0; sino
   |    snapshot iret+GPRs (saved_rax=0), state=BLOCKED,
   |    waiting_for_pid=child, wait_status_ptr=u_status,
   |    sched_resume_jump.
   |  - Si no hay children al all: -ECHILD.
   v
[meanwhile]  child eventualmente llama _exit(N):
   v
[ring 0] proc_exit_current_user(N):
   |  parent_t = task_by_pid(t->parent_pid).
   |  state = parent vivo ? TASK_ZOMBIE : TASK_DEAD.
   |  Si parent está BLOCKED esperándonos (-1 o == nuestro pid):
   |    - status_value = encode_wait_status(N).
   |    - vmm_lookup(parent->pml4, parent->wait_status_ptr) →
   |      HHDM-mapped phys + offset → escribe int.
   |    - parent->saved_rax = our pid (wait4 return value).
   |    - parent->state = READY.
   |  address_space_destroy(our pml4).
   |  reaper_add_kstack(our kstack).
   |  sched_resume_jump.
   v
[scheduler] dispatches parent → user_task_trampoline → saved_valid
   path → iretq con rax = child pid.
   v
[ring 3] wait4 retorna child pid. *status escrito vía pml4.
   |  Si llegó signal antes que child wake → sig_pending != 0 ó
   |  saved_rax = -EINTR (sys_kill lo escribió). user_task_resume
   |  delivers signal o syscall retorna -EINTR.
```

**Decisión de diseño**: ZOMBIE en vez de "extend reap grace" — un
slot ZOMBIE se queda explícitamente hasta que wait() lo consume
(transition ZOMBIE → DEAD). El reaper solo reapea DEAD. Esto
preserva el exit_code sin races y permite que `ps` futuro muestre
"defunct" como Linux.

**Orphan handling**: si parent_pid es 0 (kernel task) ó parent ya
está DEAD/UNUSED, child al exit pasa direct a DEAD (sin pasar por
ZOMBIE) — el reaper lo recoge. Sin "init" pid 1 todavía.

## Flujo de un signal (sigaction + delivery + sigreturn)

POSIX sa_handler-only. El kernel mantiene 32 handlers per task,
delivery en `user_task_resume` justo antes del iretq.

```
[ring 3] sigaction(SIGINT, &act, NULL):
   |  libc wrap: si !act.sa_restorer, fill with __sigtramp.
   v
[ring 0] sys_rt_sigaction(SIGINT, &act, NULL, _):
   |  Validación: SIGKILL/SIGSTOP → EINVAL.
   |  copy_from_user(&kact, &act, sizeof(kuser_sigaction_t)).
   |  t->sa_handler [1] = kact.sa_handler.
   |  t->sa_restorer[1] = kact.sa_restorer.
   v
... más tarde, signal delivery:

[ring 3] otra task hace kill(t->pid, SIGINT) o user toca Ctrl+C:
   v
[ring 0] sys_kill / tty_signal:
   |  t->sig_pending |= 1 << (SIGINT-1) ;
   |  Si t->state == BLOCKED:
   |    Si t->saved_valid: t->saved_rax = -EINTR;
   |    t->state = READY;
   |  return.
   v
[scheduler] dispatches t → user_task_trampoline → user_task_resume:
   |  Build buf[] del iret+GPRs como siempre.
   |  Loop: lowest_signal(t->sig_pending) = SIGINT.
   |        handler = t->sa_handler[SIGINT-1].
   |        Si SIG_IGN (1): clear bit, continue.
   |        Si SIG_DFL (0): SIGINT/TERM/etc → proc_exit_current_user(128+sig);
   |          SIGCHLD/URG/WINCH → clear+continue.
   |        Si fn ptr: build sigframe_t en user stack via
   |          write_other_user(t->pml4, sigframe_base, ...). 160 B
   |          aligned a 16. Escribe restorer en sigframe_base - 8.
   |          buf[4] = handler. buf[1] = restorer_slot.
   |          buf[10] = signum (rdi). Clear bit. break.
   |  iretq con buf modificado.
   v
[ring 3] entra al handler con rdi=signum, rsp apuntando a restorer.
   handler corre user code; cuando hace `ret`, pop restorer.
   v
[ring 3] __sigtramp (sigtramp.S):
   |  movq $15, %rax       ; SYS_RT_SIGRETURN
   |  syscall
   v
[ring 0] sys_rt_sigreturn:
   |  user_rsp = iret[3] (current iret frame's RSP — top of sigframe).
   |  copy_from_user(&f, user_rsp, sizeof(sigframe_t)).
   |  t->saved_iret_* = f.{rip,cs,rflags,rsp,ss}.
   |  t->saved_* = f.{rax,...,r15}.
   |  t->saved_valid = 1; t->state = READY.
   |  sched_resume_jump.
   v
[scheduler] re-dispatches t → user_task_resume restaura saved_* →
   iretq al RIP/RSP justo después del syscall original (antes del
   signal). RAX = lo que era antes del signal (e.g. -EINTR si
   estábamos en wait4 interrumpido).
```

**Invariantes**:
- `sa_handler == 0` = SIG_DFL, `== 1` = SIG_IGN, otros = user ptr.
- Solo se entrega UN signal por dispatch (loop break tras delivery
  exitosa). Otros pendientes esperan al próximo iretq.
- Stack overflow: si user stack no llega a sigframe_base (~200 B
  abajo del orig RSP), `write_other_user` devuelve 0 y dropeamos
  el signal silenciosamente. Mejor que kernel panic.
- POSIX exec-on-handler: NO implementado (al fork las handlers se
  heredan, al execve se resetean en proc_execve_replace — TODO).

## VFS layered backend dispatch

```
vfs_stat("/bin/hello")
   |  check_path  → valida path, comienza con '/'
   |  find_mount(longest-prefix):
   |    /bin → aliasfs (post-FASE 10 + FAT mounted)
   |  → aliasfs_ops->stat(priv, "/bin/hello"):
   |       priv = { mountpoint="/bin", target="/sd/bin" }
   |       rewrites path: "/bin/hello" → "/sd/bin/hello"
   |       recursively calls vfs_stat("/sd/bin/hello")
   |  → find_mount("/sd/bin/hello") = fat16
   |  → fat_vfs_ops->stat(priv, "/sd/bin/hello"):
   |       strip_mount: "/sd/bin/hello" → "/bin/hello" (interno a FAT)
   |       fat_lookup walks dir entries (con LFN si hace falta)
   |       returns vfs_stat_t { type=REG, size=44384 }
```

Aliasfs es trivial (rewrite + delegate); fat es el único backend
con state real (BPB, FAT, ATA reads). Sysfs/devfs/binfs son
synthetic — generan respuestas on-demand sin storage.
