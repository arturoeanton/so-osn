# OSnOS — Architecture

Minimalist x86_64 microkernel operating system. Booted by Limine.
**Post-FASE 10**: console + keyboard + shell live as ring-3 ELFs
(`consrv`, `kbdsrv`, `shellsrv`); the kernel only starts drivers,
spawns the three servers, and enters the scheduler. fs_server was
eliminated (the shell talks VFS directly). User tasks run in ring 3
with their own address space and enter the kernel via `syscall`
(preferred) or `int 0x80` (legacy compat).

**Post-FASE 12**: in addition to the 3 core servers, an **Ox window
system server** (`/bin/oxsrv`) can be launched on demand — it is
opt-in (`oxsrv &` from the shell), not autostart. When running it
takes exclusive ownership of the framebuffer (via new ioctls
`FBIOGET_VSCREENINFO` + `FBIO_BLIT`) and of the keyboard ring (via
new opcodes `IPC_CONSOLE_SUSPEND` + `IPC_KEYBOARD_SUSPEND`). GUI
apps talk to oxsrv via IPC opcodes in the `0x60-0x7F` range.

**Post-FASE 13**: musl 1.2.5 available as **opt-in second libc**
(`USER_ELF_MUSL_SRCS` in the GNUmakefile). Coexists with mini-libc:
simple programs keep using it (small footprint), apps that need
full stdio/`%f`/locale/pthread use musl. Three new bootstrap
syscalls: `SYS_WRITEV=20` (musl stdio), `SYS_ARCH_PRCTL=158`
(`wrmsr MSR_FS_BASE` for TLS), `SYS_SET_TID_ADDRESS=218` (stub).

Timer-driven preemptive scheduler (50 ms quantum, CPL=3 only) on
top of a cooperative ring-0 loop. **Full POSIX core ABI 100%
COMPLETE**: `fork(2)` (SYS_FORK=57) + `execve(2)` (SYS_EXECVE=59)
+ `wait(2)/waitpid(2)` (SYS_WAIT4=61, with new TASK_ZOMBIE state
to preserve exit_code) + `sigaction(2)` (SYS_RT_SIGACTION=13,
sa_handler-only model with sigframe on user stack +
SYS_RT_SIGRETURN=15 epilogue via libc __sigtramp) + standard EINTR
on blocking syscalls. Also `osn_spawn` (SYS_SPAWN=266, atomic
fork+exec posix_spawn-style with MOVE-semantics fd inheritance)
for cases where fork+exec would be overkill.

**Disk-resident** (Phase 2): sd.img (32 MiB FAT16) is populated at
build time with all the ELFs (~95) + wallpapers + libc headers +
TCC sysroot, via mtools. The kernel only embeds
consrv/kbdsrv/shellsrv/banner + **oxsrv** as ROM recovery.
Kernel binary ~1.5 MB.

Spanish counterpart: [`ARCH.es.md`](ARCH.es.md).

## Layers (post-FASE 13)

```
+----------------------------------------------------------------+
| ring-3 GUI apps (FASE 12 — Ox) — clients of oxsrv via IPC     |
|   oxnotepad  — text editor (Ctrl+S, opens argv[1] path)        |
|   oxcalc     — 4-function calculator (4x5 grid)                |
|   oxterm     — PTY + uxsh sub-shell, full ANSI parser          |
|   oxfiles    — file browser (.ppm -> wallpaper, others -> notepad)|
|   oxsettings — wallpaper picker -> /home/.oxrc + RELOAD IPC    |
+----------------------------------------------------------------+
| ring-3 Ox window server (FASE 12) — ELF in elfs/gui/oxsrv.c   |
|   - owns cursor + z-order + dirty flag                         |
|   - reads /dev/mouse0 + /dev/input0 (O_NONBLOCK)               |
|   - dispatches events to focused client (IPC 0x60-0x7F)        |
|   - composites wallpaper + windows + cursor -> FBIO_BLIT ioctl |
|   - Openbox-style root menu (right-click on wallpaper)         |
|   - SUSPEND consrv + kbdsrv on start; RESUME on exit           |
|     (signal handler + watchdog in consrv/kbdsrv for safety)    |
+----------------------------------------------------------------+
| ring-3 servers (FASE 10) — ELFs in elfs/osn-server/            |
|   consrv   — IPC_CONSOLE_WRITE / CLEAR -> /dev/fb0             |
|              (FASE 12: + IPC_CONSOLE_SUSPEND/RESUME +          |
|               watchdog auto-resume if SERVER_OX disappears)    |
|   kbdsrv   — /dev/input0 -> sys_tty_input (POSIX termios)      |
|              (FASE 12: + IPC_KEYBOARD_SUSPEND/RESUME +         |
|               O_NONBLOCK read + watchdog auto-resume)          |
|   shellsrv — line editor + history + pipes/redirects + jobs   |
|              (registers SERVER_SHELL, IS THE OS shell)         |
+----------------------------------------------------------------+
| ring-3 user tasks ~95 ELFs in /bin (coreutils + net + tests +  |
|   GUI + uxsh + hello_musl):                                    |
|   ls cat cp mv rm mkdir touch echo head tail wc grep sort      |
|   uniq cut tr seq yes tee env pwd which printf date uname      |
|   basename dirname clear tree banner calc top kill sleep ovi   |
|   less readelf poweroff reboot hello hello_libc hello_musl —   |
|   httpd selectserver echotcp tcpclient — term minishell uxsh   |
|   tcc (TinyCC 0.9.27 — self-hosting C, FASE 11.0)             |
|   lua (Lua 5.4.7    — second self-host language, FASE 11.2)   |
|   jq  (jq 1.7.1     — JSON filter, FASE 11.3)                  |
+----------------------------------------------------------------+
|        lib/libc — osnos mini-libc (DEFAULT)                     |
|   stdio (FILE*, printf, fopen, fread/fwrite, fgets, snprintf), |
|   stdlib (malloc/free, qsort, atoi/strtol, atexit, setjmp),    |
|   string (mem*, str*, strdup, strstr, strtok_r), unistd (read, |
|   write, open, close, pipe, dup, dup2, fcntl, mmap, ...),      |
|   sys/socket (TCP/UDP/select), arpa/inet, time, errno, crt0    |
|   ox.h + ox.c (FASE 12 — Ox client wire protocol)              |
+----------------------------------------------------------------+
|        vendor/musl 1.2.5 — opt-in second libc (FASE 13)        |
|   USER_ELF_MUSL_SRCS in GNUmakefile. crt1.o + libc.a + crti +  |
|   crtn linked against musl.lds (preserves PT_TLS + init_array).|
|   Access to real printf %f, full snprintf, locale, pthread shim|
|              v                                                  |
|             syscall (via inline asm in syscall.h)               |
+----------------------------------------------------------------+
| Linux x86_64 syscall ABI + osnos-specific (>= 250):            |
|   read/write/open/close/lseek/fstat/mmap/munmap/brk/pipe/      |
|   dup/dup2/nanosleep/getpid/socket/bind/listen/accept/connect/ |
|   send/recv/select/fcntl/getcwd/chdir/mkdir/rmdir/rename/      |
|   unlink/ioctl/getdents64/gettimeofday/time/kill/exit/         |
|   fork (#57) execve (#59) wait4 (#61) — POSIX core              |
|   rt_sigaction (#13) rt_sigprocmask (#14) rt_sigreturn (#15)   |
|   writev (#20) — musl stdio (FASE 13)                          |
|   setpgid (#109) getppid (#110) getpgrp (#111) setsid (#112)   |
|   getpgid (#121) getsid (#124) — job control                   |
|   arch_prctl (#158) — ARCH_SET_FS = wrmsr MSR_FS_BASE = TLS    |
|     (FASE 13 — musl bootstrap)                                 |
|   reboot (#169) — ACPI S5 + 8042 reset (poweroff/restart)      |
|   set_tid_address (#218) — musl bootstrap stub                 |
|                                                                 |
|   osnos: IPC_SEND (260) IPC_RECV (261) SERVICE_REGISTER (262)  |
|   SERVICE_LOOKUP (263) TTY_INPUT (264) TASKINFO (265)          |
|   SPAWN (266) SET_FG (267) RESUME (268)                        |
|                                                                 |
| Framebuffer ioctls (FASE 12 — Ox needs raw pixel access):      |
|   FBIOGET_VSCREENINFO = 0x4600 (Linux-compat) -> fb_var_       |
|     screeninfo { xres, yres, bits_per_pixel, line_length,      |
|     RGBA offsets }                                              |
|   FBIO_BLIT = 0x4680 (osnos-specific) -> fb_blit_req { x, y,   |
|     w, h, src, src_pitch } — copy_from_user row-by-row +        |
|     framebuffer_blit_kernel. Path detected by "/dev/fb0".      |
|                                                                 |
|       int 0x80 (IDT[0x80] DPL=3)      syscall  (LSTAR=entry)   |
|              int80_entry asm                    syscall_entry  |
|                       \                          /              |
|                        syscall_dispatch(frame)                  |
+----------------------------------------------------------------+
| VFS layer:                                                      |
|   ramfs (/)  sysfs (/sys)  devfs (/dev: null/zero/fb0/input0/  |
|                                       ttyS0/tty/ptmx + pts/N)  |
|   aliasfs (/home -> /sd/home  AND  /bin -> /sd/bin)            |
|   binfs (/bin diskless fallback)                               |
|   fat16 (/sd, sd.img — read/write with dir-chain extension)    |
+----------------------------------------------------------------+
| IPC layer:                                                      |
|   ipc_send / ipc_recv (64 x 1 KB shared queue)                 |
|   ipc_send two-step routing (FASE 12 — Ox needed direct-pid):  |
|     1) try service_get_pid(to)  — covers SERVER_* clients     |
|     2) if not, treat `to` as literal pid, check task_by_pid()  |
|     -> lets oxsrv send events back to client tasks             |
|       without each client having to register its own SID       |
|   service_register / service_lookup (SERVER_* SIDs:            |
|     KEYBOARD=1 SHELL=2 CONSOLE=3 FS=4 OX=5 — FASE 12 added OX) |
|   IPC opcode ranges:                                            |
|     0x00-0x0F system (incl CONSOLE/KEYBOARD SUSPEND/RESUME)    |
|     0x10-0x1F console, 0x20-0x3F fs/vfs                         |
|     0x40-0x5F process lifecycle (PROC_EXITED/STOPPED/CONTINUED)|
|     0x60-0x7F Ox window system (FASE 12 — CONNECT/WINDOW_*/   |
|                                  DRAW_*/EVENT_*/RELOAD_SETTINGS)|
+----------------------------------------------------------------+
| net/ stack (FASE 8.5):                                          |
|   socket (UDP + full TCP state machine, accept queue, retx RTO)|
|   tcp -> ip -> eth (rtl8139)  +  arp (cache + ARP_TIMEOUT poll)|
|   icmp + udp delivered through socket layer to ring-3          |
|   DNS resolver + getaddrinfo (slirp 10.0.2.3)                  |
+----------------------------------------------------------------+
| micro/ core:                                                    |
|   task (16 slots; per-task fds[16] thin slots -> OFD pool,     |
|     FPU state, mmap regions, cwd, kill/stop_pending,           |
|     stdin/stdout_redir, saved iret, parent_pid + wait_status_  |
|     ptr + wait_change (WUNTRACED/WCONTINUED tracking),         |
|     sa_handler[32] + sig_pending (sigaction), pgid + sid       |
|     (job-control), TASK_ZOMBIE state)                           |
|   fd (per-task slot {used, ofd_idx, fd_flags=CLOEXEC}) +       |
|     global ofd_pool[128] shared open file descriptions —       |
|     refcounted; dup/dup2/fork share offset via OFD ref bump    |
|   pty (pool of 8 pty_pair_t — m2s/s2m 4 KiB ring buffers +     |
|     per-pair termios + canon line accumulator; /dev/ptmx +     |
|     /dev/pts/N via OFD with is_pty + pty_side; slave_was_      |
|     opened latch prevents EOF race on master post-openpt;      |
|     showcased via /bin/term spawning /bin/minishell)            |
|   scheduler (preempt CPL=3 + cooperative + resume_jump)        |
|   reaper (kstack free queue + DEAD -> UNUSED reaping)          |
|   ipc, service, fd, pipe, fpu, extable, uaccess (fault-recov)  |
|   syscall, syscall_msr, syscall_entry, int80, tss, gdt, idt    |
|   pmm, vmm, kmalloc — paging, heap, per-task address spaces    |
+----------------------------------------------------------------+
| proc/ layer:                                                    |
|   builtin (registry of ELFs embedded as ROM fallback)           |
|   exec (proc_execve, proc_exit_current_user)                   |
|   elf (Elf64 loader; PT_LOAD -> page-by-page map + zero-fill)  |
+----------------------------------------------------------------+
| drivers/:                                                       |
|   keyboard (PS/2; scancodes + Shift + Ctrl + ext + arrows)     |
|     **FASE 12 fix**: check STAT_AUX_DATA, skip mouse bytes     |
|     (without this, AUX bytes were read as random scancodes)    |
|   mouse (PS/2 AUX poll, 3-byte packets, sign extension,        |
|     sync recovery, dy inverted) — FASE 11.4                    |
|   framebuffer (linear FB + 8x8 font + VT100 CSI parser:        |
|     ESC[2J/H/r;cH/K/m/7m + SGR truecolor 38;2;R;G;B; tees      |
|     every write to serial_puts -> dual-console always on;      |
|     FASE 12: + framebuffer_get_info + framebuffer_blit_kernel  |
|     for the new ioctls FBIOGET_VSCREENINFO + FBIO_BLIT)        |
|   serial (UART 16550 COM1 0x3F8, polling-only, 38400 8N1 +      |
|     FIFO; serial_putc spin LSR THRE + CRLF expand, try_getc     |
|     non-blocking RX poll)                                       |
|   rtl8139 (PCI; PIO+DMA; 4 TX slots, RX ring; IRQ stub)         |
|   block_ata (ATA PIO over IDE primary master, LBA28)           |
|   pic / lapic / timer (PIT @ 100 Hz for preempt)                |
+----------------------------------------------------------------+
| Kernel-side cooperative tasks (NOT servers, helpers):          |
|   keyboard feeder — keyboard_poll -> devfs_input_push          |
|     (doesn't touch TTY policies — that lives in ring-3 kbdsrv) |
|   mouse feeder    — mouse_poll -> devfs_mouse_push             |
|     (feeds /dev/mouse0, read by oxsrv and/or mousetest)        |
|   serial-in feeder (FASE 10.7) — serial_try_getc loop x64/tick |
|     -> tty_input(b) direct. \\r -> \\n for host Enter.          |
|     Result: COM1 RX bytes arrive at shellsrv fd 0 like any      |
|     PS/2 keystroke. Enables headless boot.                      |
|   init-respawn — every ~100ms checks consrv/kbdsrv/shellsrv    |
|     are still alive; respawns if dead (covers post-exec gap).  |
|     Does NOT respawn oxsrv (opt-in: if it died, it stays       |
|     dead). Sleep via state=BLOCKED + wakeup_at_ms.             |
+----------------------------------------------------------------+
|                       Limine bootloader                         |
+----------------------------------------------------------------+
```

## Flow of a typical keystroke (post-FASE 10)

```
   user types 'l'
        |
        v
   PS/2 controller
        |  scancode 0x26 on port 0x60
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
   [ring 3]  kbdsrv (ELF in elfs/osn-server/kbdsrv.c)
        |  read("/dev/input0", &ev, sizeof(ev))   <- syscall
        |  sys_tty_input('l') — feeds the kernel-side TTY input ring
        |  (sys_tty_input #264; only the SERVER_KEYBOARD pid is allowed)
        |  (no more IPC_KEY_EVENT to SERVER_SHELL — shellsrv reads from TTY)
        v
   [ring 3]  shellsrv (ELF in elfs/osn-server/shellsrv.c)
        |  read(0, &c, 1)   <- blocking syscall on stdin (raw mode termios)
        |  sys_read -> TTY input ring -> returns 'l'
        |  shellsrv stores 'l' in line buffer, renders prompt+cursor
        v
   printf-equivalent (libc) -> write(1, "l", 1)   <- syscall
        |  sys_write fd=1 (default) -> write_to_console (kernel)
        v
   ipc_send(to=SERVER_CONSOLE, type=IPC_CONSOLE_WRITE,
            arg0=color, data="l", arg1=1)
        |  ipc_send rewrites msg.to: SERVER_CONSOLE (SID) -> consrv_pid
        |  task_unblock(consrv_pid)
        v
   [ring 3]  consrv (ELF in elfs/osn-server/consrv.c)
        |  sys_ipc_recv -> ipc_recv(t->pid, &msg) matches the queued msg
        |  if color != white: write(fb_fd, "\x1b[38;2;R;G;Bm", ...)
        |  write(fb_fd, msg.data, msg.arg1)        <- syscall on /dev/fb0
        |  write(fb_fd, "\x1b[39m", 5)
        v
   [ring 0]  devfs fb0_write -> framebuffer_write_bytes(buf, n, color)
        |  framebuffer_draw_string per chunk (with safe-split for CSI)
        |  pixel writes to the linear framebuffer
```

**Key changes vs pre-FASE 10**:
- TTY policy (canonical/raw/echo/ISIG) now lives in the kernel TTY
  layer but **delivery** is done by ring-3 kbdsrv via SYS_TTY_INPUT.
- shellsrv reads bytes with `read(0)` like any POSIX program in raw
  mode — does not receive IPC_KEY_EVENT.
- ring-3 consrv wraps every write with SGR truecolor if arg0 !=
  white, and does the syscall to `/dev/fb0` (devfs char device).
- The whole chain crosses ring 3 <-> ring 0 ~4 times; the extra cost
  is worth it for modularity (the servers are replaceable ELFs in
  /sd/bin/).

## Flow of a command: `ls /home` (post-FASE 10)

`ls` is now a standalone ELF at `/bin/ls` (not a shell builtin).
shellsrv spawns it via `osn_spawn` and inherits stdin/stdout to it.

```
[ring 3]  shellsrv receives '\n' from raw-mode read_line
   |
   |  history_save("ls /home")
   |  dispatch("ls /home")
   |  expand_vars (no $VAR here) + line_split_stages + stage_parse
   |  argv = ["ls", "/home"]
   |
   v
[ring 3]  shellsrv: cmd not in COMMANDS[] (builtins) -> external ELF
   |  resolve_cmd_path("ls", ...) -> search by $PATH:
   |     stat("/bin/ls") -> OK -> path = "/bin/ls"
   |  pack_envp(envbuf, ...) -> "PATH=/bin\0HOME=/home\0SHELL=...\0\0"
   |  osn_spawn("/bin/ls", "/home", envp, stdin_fd, stdout_fd)
   v
[ring 0]  sys_spawn (#266)
   |  proc_execve("/bin/ls", "/home", envp_array)
   |  /bin/ls -> aliasfs -> /sd/bin/ls (FAT16)
   |  vfs_stat(/sd/bin/ls) -> exists -> vfs_read(blob) -> elf_load(blob)
   |  fd inheritance: child fds[0]=src_stdin, fds[1]=src_stdout (MOVED)
   |  task_create_user_elf, returns child_pid
   v
[ring 3]  shellsrv: wait_pid_or_stop(child_pid)
   |  loop with sys_taskinfo(265) until state == TASK_DEAD or STOPPED
   |  (yield via nanosleep between iterations)
   v
[ring 3]  /bin/ls main(argc=2, argv=["ls","/home"], envp=...)
   |  opendir("/home") -> sys_open (DIR)
   |  loop readdir -> sys_getdents64
   |     ramfs (or aliasfs -> fat16 if /home on FAT) drives the listing
   |  for each entry: printf("%s\n", name)   <- buffered stdio
   |  exit(0) -> atexit + fflush(stdout) -> _exit syscall #60
   v
printf chain: stdio buffer -> write(1, buf, n)
   |  sys_write fd=1 -> write_to_console
   |  ipc_send(SERVER_CONSOLE -> consrv_pid, IPC_CONSOLE_WRITE, ...)
   v
[ring 3]  consrv processes IPC, writes to /dev/fb0 (framebuffer)
   v
[ring 0]  proc_exit_current_user(0) on /bin/ls
   |  destroy AS, mark task DEAD, sched_resume_jump
   |  reaper queue picks up kstack on next tick
   v
[ring 3]  shellsrv sees TASK_DEAD, prints next prompt
```

**Notes**:
- fs_server (the ring-0 ipc wrapper that existed until FASE 10.3)
  is **deleted**. ELFs invoke VFS directly via syscalls (open,
  read, write, getdents64, stat, etc.).
- shellsrv builtins (`cd`, `pwd`, `help`, `exit`, `export`, `jobs`,
  `fg`, `bg`, `kill`, `history`, `test`) do not spawn ELFs — they
  run in-process in ring-3 shellsrv.
- Any command NOT in COMMANDS[] is treated as an ELF name and
  resolved via $PATH (default `/bin`).

## Flow of an incoming HTTP GET (`curl http://localhost:8088/`)

```
host curl: TCP SYN -> 10.0.2.2:8088 (slirp gateway from guest's POV)
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
   |  rtl8139_tx hunts for free slot (avoids tx_cur saturation)
   |  writes packet to TX_BUF, fires TSD with size
   |  chip transmits + TOK IRQ when done
   v
host curl receives the response, renders HTML.
   |  half-close (FIN) from curl -> we transition ESTABLISHED -> CLOSE_WAIT
   |  httpd close(fd) -> sock_close_tcp -> emit FIN+ACK, LAST_ACK
   |  host's ACK of our FIN -> CLOSED, slot freed via zombie path
```

## Key contracts

### IPC (`src/include/osnos_ipc_abi.h`, kernel impl `src/micro/ipc.{c,h}`)

- Fixed-size messages (`ipc_msg_t`: 1024 B data + arg0/arg1/from/to/
  type), copied at `ipc_send`. Sender can discard its buffer the
  moment send returns.
- Opcodes in numeric ranges (shared kernel <-> ring-3 ABI):
  - `0x00-0x0F` system (KEY_EVENT, COMMAND_RUN)
  - `0x10-0x1F` console (CONSOLE_WRITE, CONSOLE_CLEAR)
  - `0x20-0x3F` fs / vfs (legacy — fs_server eliminated in FASE 10.3)
  - `0x40-0x5F` process lifecycle (PROC_EXITED, PROC_EXITED_USER)
  - `0x60+` reserved for new ones (FASE 12 Ox: `0x60-0x7F`)
- Shared SID enum: `SERVER_KEYBOARD=1`, `SERVER_SHELL=2`,
  `SERVER_CONSOLE=3`, `SERVER_FS=4` (free since 10.3), `SERVER_OX=5`.
- **SID -> pid rewrite in ipc_send** (FASE 10): `msg->to` is
  originally a SID; `ipc_send` resolves to pid via `service_get_pid`
  and rewrites the queued copy. Ring-3 receivers (sys_ipc_recv)
  filter by `t->pid`, NOT by SID. Without this rewrite, ring-3
  consumers never matched their own messages.
- Response convention:
  - `arg0` = `osnos_status_t` (0 = OK, >0 = errno-like).
  - `arg1` = payload size or count when applicable.
  - `data` = textual or binary payload (null-terminated when text;
    explicit `arg1` when binary or color-prefixed).
- `ipc_send` returns `OSNOS_OK` / `OSNOS_EAGAIN` (queue full) /
  `OSNOS_ESRCH` (target not registered). Callers MUST check.
- Ring-3 receivers (consrv/kbdsrv/shellsrv) use `ipc_recv_block`
  libc wrapper that loops over SYS_IPC_RECV (#261) with nanosleep
  between EAGAIN — they don't block kernel-side.

### Status codes (`src/include/osnos_status.h`)

Numeric values **identical** to Linux x86_64 errno
(`asm-generic/errno-base.h`, `errno.h`). Project-wide invariant:
anything that may cross the user/kernel boundary in the future uses
these values as-is. Do not invent numbers in the Linux-occupied
range; if we need a code without equivalent, reserve from 200.

### Key codes (`src/include/osnos_keys.h`)

Subset of Linux `input-event-codes.h`: `KEY_UP=103`, `KEY_DOWN=108`,
`KEY_LEFT=105`, `KEY_RIGHT=106`. For future `/dev/input/*`
forwarding without translation.

### Ramfs (`src/fs/ramfs.h`)

- Flat array of `RAMFS_MAX_FILES` slots; each slot stores full path
  + data.
- **Slot ownership**: a slot's index does not change during its
  lifetime. A `const ramfs_file_t *` returned by `ramfs_find`
  remains valid until that same slot is deleted. Critical for the
  future VFS FD layer.
- Delete = `slot.used = false`. The array is never compacted.
- `ramfs_move` of a directory atomically renames the children or
  aborts.

### Shell (`elfs/osn-server/shellsrv.c` — ring 3 post-FASE 10.4)

- It is a normal ELF loaded by proc_execve in kmain. Registers
  SERVER_SHELL via SYS_SERVICE_REGISTER.
- **raw-mode TTY**: tcgetattr/tcsetattr to disable ICANON+ECHO and
  read one byte at a time with read(0). Processes CSI manually
  (`ESC[A/B/C/D` for arrows, `0x7F`/`\b` for backspace).
- Commands via `COMMANDS[]` table with `{ name, fn, help }`
  structs. `help` generated by iterating the table. Includes:
  `help`, `exit`, `pwd`, `cd`, `ls`, `cat`, `echo`, `history`,
  `test`, `jobs`, `fg`, `bg`, `kill`, `export`, `unset`,
  `setenv`, `exec`.
- **`exec CMD [args]`** builtin — calls `execve(2)` after
  `osn_set_fg(getpid())` so Ctrl+C delivers to the new image
  post-swap. If exec fails (path not found etc.), shellsrv stays
  alive and reports errno.
- **$VAR / ${VAR} expansion** (post-FASE 10.4): `expand_vars` walks
  + substitutes before pipeline parsing. Supports `\$` escape.
- **Pipeline parser** (`line_split_stages` + `stage_parse` +
  `run_pipeline`): split on `|`, each stage supports `< file`,
  `> file`, `>> file`. Background with trailing `&`.
- **Sequence operators**: `dispatch` top-level splits the line on
  `;`, `&&`, `||` before pipeline parsing. Each segment runs via
  `dispatch_segment`. Decisions:
  - `;`  -> always runs the next
  - `&&` -> runs only if `last_status == 0`
  - `||` -> runs only if `last_status != 0`
  - `last_status` updates after each execution (builtin return
    value, or `wait_pid_capture` for externals).
- **`$?` substitution** in expand_vars — formats `last_status` as
  decimal.
- **Glob `*`** in stage_parse (`expand_glob_into`): tokens with
  `*` match against entries of the implicit dir (dir prefix or
  "." if no slash). Recursive matcher (`glob_match`) supports
  `*` with greedy capture. No matches -> literal token (bash
  default). Storage in static `glob_buf[4096]`.
- **`do_ls` POSIX multi-arg**: pass 1 prints files (no header),
  pass 2 lists dirs (with `PATH:` header if >1 path).
- **History**: 16-entry ring buffer with consecutive dedup,
  navigation with up/down arrows, persistence at `/home/.history`.
- **.oshrc autoload**: shellsrv executes `/home/.oshrc` line by
  line at startup. Default seeds `export PATH=/bin / HOME=/home /
  SHELL=/bin/shellsrv / OSNAME=osnos`.
- **Path resolution**: `path_normalize` for `cd ..` / `cd .` /
  relative paths; `resolve_cmd_path` for PATH search.
- **fd inheritance**: when spawning a child, copies stdin/stdout
  fds from shellsrv to the child via SYS_SPAWN (MOVES, not COPY —
  the caller loses the slots so pipe refcounting is correct).
- **Job control**: `bg_jobs[]` tracks pids + cmd labels. Ctrl+C
  sends SIGINT to the fg task (`kernel_fg_pid` set via
  SYS_SET_FG #267); Ctrl+Z does stop pending -> TASK_STOPPED.
  `fg <pid>` does SYS_RESUME #268 + osn_set_fg(pid) + wait.
  `kill <pid>` does sys_kill (also wakes STOPPED tasks).

## Boot sequence (post-FASE 10)

`kmain` in `src/kernel/main.c`:

1. Validate Limine base revision and framebuffer; `framebuffer_init`.
2. Memory: `pmm_init` -> `vmm_init` -> `kheap_init` (slab + dynamic
   growth up to 4 MiB).
3. CPU: `gdt_init` -> `tss_init` -> `idt_init` -> `uaccess_init`
   (registers the copy_*_user span in the extable) ->
   `syscall_msr_init` (enables EFER.SCE + programs STAR/LSTAR/FMASK).
4. Interrupts: `pic_init` -> `lapic_init` -> `timer_init` (PIT @ 100
   Hz). `block_ata_init` runs IDENTIFY against primary IDE; if a
   disk is present, FAT16 is mounted later at `/sd`.
5. Microkernel: `ipc_init` -> `pipe_init` -> `task_init` ->
   `reaper_init` -> `scheduler_init` -> `syscall_init` ->
   `ramfs_init` (empty slots) -> `bootstrap_fs`.
6. `bootstrap_fs`:
   - Mounts `/`, `/sys`, `/dev` (synthetic backends).
   - If FAT mounted: mounts `/sd`. **Phase 2 disk-resident**:
     `sd.img` already has `/bin/*` populated by the build script
     (GNUmakefile + mtools), and `/home/{README.TXT, HELLO.TXT,
     .oshrc}` also pre-loaded. `bootstrap_fs` only does `mkdir
     /sd/bin` (idempotent) and dumps the 4 ROM ELFs if missing
     (recovery path). Mounts aliasfs `/bin -> /sd/bin` and
     `/home -> /sd/home`.
   - Diskless: mounts synthetic binfs at `/bin` (read-only on the
     ROM set: consrv/kbdsrv/shellsrv/banner + user_hello) and
     creates `/home` ramfs.
7. `task_create("keyboard", keyboard_server_tick)` — kernel-side
   feeder (poll PS/2 -> /dev/input0 ring buffer).
8. Spawn the 3 ring-3 servers via `proc_execve` +
   `service_register`:
   ```c
   int64_t consrv_pid   = proc_execve("/bin/consrv",   "", 0);
   service_register(SERVER_CONSOLE,  (uint64_t)consrv_pid);
   int64_t kbdsrv_pid   = proc_execve("/bin/kbdsrv",   "", 0);
   service_register(SERVER_KEYBOARD, (uint64_t)kbdsrv_pid);
   int64_t shellsrv_pid = proc_execve("/bin/shellsrv", "", 0);
   service_register(SERVER_SHELL,    (uint64_t)shellsrv_pid);
   ```
   Pre-registration avoids the race: if shellsrv sends IPC before
   auto-registering, it already has the SID resolved in
   `service_pid[]`.
9. `task_create("init-respawn", server_respawn_tick)` — watchdog
   kernel task that every ~100ms checks the 3 servers and respawns
   them if dead. Sleep using `state=BLOCKED + wakeup_at_ms =
   timer_ms()+100`. Resolves the post `exec /bin/foo` hang (foo
   ends -> shellsrv slot UNUSED -> no shell -> without watchdog the
   system hung).
10. `keyboard_server_init()` (PS/2 hardware init).
11. `__asm__("sti")` — enables IRQs. Timer starts ticking.
12. `scheduler_loop`: saves the resume point (longjmp host) and
    enters `for(;;) scheduler_tick()`. Each tick:
    - `reaper_drain` (frees kstacks of dead tasks, reaps DEAD ->
      UNUSED)
    - `task_check_wakeups` (BLOCKED + wakeup_at_ms reached ->
      READY)
    - dispatches the next READY (round-robin).

    ring-3 tasks are **preempted every 50 ms** via timer IRQ (FASE
    9 — only when CPL=3 entering the handler; kernel-side tasks
    remain cooperative).

## Flow of a syscall from ring 3 (`exec /bin/ring3hello`)

```
ring 3:  mov $1,%rax; mov $1,%rdi; lea msg,%rsi; mov $17,%rdx; syscall
            |
            v
CPU sets   CS=GDT_KCODE, SS=GDT_KDATA  (from STAR[47:32])
           RCX=user RIP, R11=user RFLAGS, RFLAGS &= ~FMASK
           RIP=LSTAR=syscall_entry
            |
            v
syscall_entry asm:
  - movq %rsp, syscall_user_rsp     ; CPU didn't change RSP, RSP=user stack
  - movq tss_kernel_rsp0, %rsp      ; now on per-task kernel stack
  - push r11 / rcx                   ; preserve user RIP/RFLAGS
  - push frame (rax,rdi,rsi,rdx,r10,r8,r9)
  - andq $-16, %rsp; call int80_dispatch_wrapper
            |
            v
int80_dispatch_wrapper:
  - syscall_dispatch(frame) -> sys_write
  - sys_write -> write_to_console (kernel helper)
  - ipc_send(SERVER_CONSOLE -> consrv_pid, IPC_CONSOLE_WRITE, ...)
  - return retval in rax
  - (ring-3 consrv eventually receives the IPC and writes to /dev/fb0)
            |
            v
syscall_entry asm (cont):
  - pop frame, pop rcx/r11
  - movq syscall_user_rsp, %rsp
  - sysretq                          ; CPU: CS=GDT_UCODE, SS=GDT_UDATA
                                     ; RIP=RCX, RFLAGS=R11
            |
            v
ring 3:  next instruction after the syscall
```

The `int 0x80` path is equivalent except: (a) the CPU does push an
iret frame (SS, RSP, RFLAGS, CS, RIP) onto the kernel stack, so
`int80_entry` doesn't need to save RSP manually; (b) it returns
with `iretq`, not `sysretq`.

## Fault recovery (ring 0 and ring 3)

Any exception vector first goes through `fault_try_recover` in
`src/micro/idt.c`:

1. **Kernel-mode with RIP in extable** -> rewrites `frame->rip` to
   the recovery label and returns from the handler. The callee
   "returns" with `OSNOS_EFAULT` (e.g.: `copy_from_user` on an
   unmapped page).
2. **User-mode (CPL=3) with a live user task** -> prints "ring-3
   task killed: <vec>" and calls `proc_exit_current_user(139)`
   which destroys the AS, sends IPC_PROC_EXITED, and does
   `sched_resume_jump`. The shell stays alive.
3. **Else** -> classic panic (`hcf`).

## User process heap (sys_brk)

Each user task has its own "break": low virtual address of the
heap. libc `malloc` sits on `sbrk` which sits on the `brk` syscall
(Linux #12):

```
                user pml4
  USER_CODE_VIRT (0x400000)  +--------------+
                             |   PT_LOAD    |
                             |   text+rodata|
                             +--------------+
                             |   PT_LOAD    |
                             |   data+bss   |
                             +--------------+

  USER_HEAP_BASE (0x10000000)
    heap_start -----> +--------------+  <- initial heap_brk == heap_start
                     |  (empty)     |
                     |              |  sys_brk(addr > heap_brk) -> pmm_alloc + vmm_map
                     |  heap        |  sys_brk(addr < heap_brk) -> vmm_unmap + pmm_free
                     |              |
    heap_brk ------> +--------------+

  USER_STACK_VIRT  +--------------+  <- 4 KiB page, single
    (0x7FFFE000)   |     stack    |  user RSP = 0x7FFFF000
                   +--------------+
```

`sys_brk(new)` with `new == 0` reports the current break;
otherwise zero-fills + maps (or unmaps) the corresponding pages
and updates `task.heap_brk`. If requested is out of range (<
heap_start or >= USER_VIRT_MAX) it returns the current break
without touching anything — libc detects it and sets `errno =
ENOMEM`.

## libc

`lib/libc/` is a local mini-libc (~700 LOC). Compiled separately
from the kernel with `USER_CFLAGS` (no `-mcmodel=kernel`), packaged
in `libosnos_c.a` + a standalone `crt0.S.o`, and linked against
every user ELF that wants it.

```
elfs/tests/hello_libc.c
    |
    | clang USER_CFLAGS -I lib/libc/include
    v
hello_libc.o
    |
    | ld.lld -T elfs/libc.lds
    |      crt0.S.o (provides _start) +
    |      hello_libc.o +
    |      libosnos_c.a (printf, malloc, strlen, ...)
    v
hello_libc.elf        (ELF64 ET_EXEC, two PT_LOAD)
    |
    | objcopy -B i386:x86-64 -I binary -O elf64-x86-64
    v
hello_libc.elf.o      (symbols _binary_hello_libc_elf_start/end)
    |
    | kernel link
    v
build/kernel          (embeds the bytes)
```

At runtime:

```
shellsrv:/$ hello_libc
   |  (shellsrv resolve_cmd_path via PATH -> /bin/hello_libc)
   |  (osn_spawn -> SYS_SPAWN -> proc_execve)
   v
proc_execve -> task_create_user_elf -> elf_load(blob,...)
   |  <- maps PT_LOADs, allocates stack page,
   |     sets task.user_entry = 0x400000 (= _start),
   |     task.heap_start = task.heap_brk = 0x10000000
   v
scheduler dispatch -> user_task_trampoline -> iretq -> CPL=3
   |
   v
_start (crt0.S):
   andq $-16, %rsp
   call main(0, NULL, NULL)
   |
   v
main:
   printf("hi %s, %d!\n", "world", 7);
        | vfprintf -> sink_flush -> write(1, buf, n) -> syscall #1
   buf = malloc(64);
        | first-fit walks NULL list, sbrk(80) -> syscall #12 (brk)
        | kernel maps a heap page at 0x10000000
   strcpy(buf, ...); puts(buf);
   free(buf);
   return 0;
        | crt0 calls exit(0) (NOT _exit directly — goes through
        |   exit so that atexit() + fflush(stdout) run)
        | exit -> _exit -> syscall #60
        | proc_exit_current_user -> AS destroy + IPC_PROC_EXITED + sched_resume_jump
   |
   v
shellsrv (wait_pid_or_stop) sees TASK_DEAD, prints new prompt
```

## ELF loader

`elf_load(blob, size, *pml4, *entry, *stack_top)`:

1. Validates ELF magic / class=64 / little-endian / ET_EXEC /
   EM_X86_64.
2. Creates an address space (`address_space_create`).
3. For each `PT_LOAD`:
   - allocates + maps pages at `[p_vaddr, p_vaddr + p_memsz)` with
     `PTE_U` (and `PTE_W` if `PF_W`)
   - copies `p_filesz` bytes from the blob, zero-fills the rest
4. Allocates a user stack page at `0x7FFFE000-0x7FFFF000`.
5. Returns PML4, entry (`e_entry`) and stack top.

`task_create_user_elf` takes the result, adds kstack + task slot,
and sets `t->user_entry / user_stack_top` so
`user_task_trampoline` builds the correct iretq frame.

## Disk-resident /bin (Phase 1 + Phase 2 FINAL)

The canonical store of ELFs is **FAT16** at `/sd/bin/`. The kernel
only embeds a minimal **ROM recovery set** (4 critical ELFs + bare
user_hello). The rest live exclusively on disk. Kernel binary went
from **7.6 MB -> 1.1 MB** (-85%).

```
build (host):
   GNUmakefile target $(SD_IMG) depends on USER_ELF_LIST.
   v
   mformat -i sd.img ::
   mmd     -i sd.img ::/bin ::/home
   for elf in $(USER_ELF_LIST):
       name = basename $$elf .elf
       mcopy -i sd.img $$elf ::/bin/$$name      # 64 ELFs, no .elf
   mcopy /home/{README.TXT,HELLO.TXT,.oshrc}    # seed user files
   v
kernel link:
   only objcopy -> .elf.o the 4 ROM:
     consrv kbdsrv shellsrv banner + user_hello (bare)
   builtin.c registry reflects that minimal set
   v
boot:
   bootstrap_fs detects FAT16 on /sd, mounts /sd via fat_vfs_ops.
   `mkdir /sd/bin` idempotent (already exists from build).
   `seed_disk_bin()` iterates builtins[] (4 entries) — vfs_stat
   sees they already exist at /sd/bin/* -> skip. Only if disk is
   empty or corrupt are the 4 ROMs re-written.
   aliasfs /bin -> /sd/bin   (whole set of 64 ELFs accessible)
   aliasfs /home -> /sd/home
   v
runtime:
   exec.c proc_execve("/bin/hello"):
     1. vfs_stat("/bin/hello") -> aliasfs -> /sd/bin/hello in FAT -> OK
     2. vfs_read(blob), elf_load(blob), task_create_user_elf
     3. If vfs_stat fails (file not on disk):
        fallback to builtin_find("hello") -> only works for the
        4 ROM. For the other 60 ELFs there is no fallback (by design).
```

**FAT16 NT case bits**: SFNs in FAT16 are stored uppercase but
byte 0x0C of the dirent has bits 0x08 (base lowercase) and 0x10
(ext lowercase) that Windows 95+ and mtools set. Our
`name_from_83` honors them -> `hello` doesn't come back as
`HELLO` and case-sensitive matchers (glob, strcmp) work.

**FAT16 dir-chain extension**: `extend_dir_chain` allocates a new
cluster and chains it to the dir when `find_free_dir_slots_run`
hits ENOSPC. Allows large subdirs (the 64 ELFs live in `/sd/bin/`
without issue).

## TinyCC self-hosting (FASE 11.0)

osnos ships a vendored TinyCC 0.9.27 at `vendor/tinycc/`
cross-compiled into `/bin/tcc` (~1 MB ELF). A program written
inside the guest can be edited with `ovi`, compiled with `tcc`,
and run — no host involvement after the initial build of the
osnos image.

```
[ring 3]  user types: tcc /home/foo.c -o /home/foo
   v
[ring 3]  shellsrv -> osn_spawn("/bin/tcc", "/home/foo.c -o /home/foo")
   v
[ring 3]  tcc.elf (TinyCC, 0.9.27 with osnos patches) starts
   |
   |  1. parse argv, default_static_link=1 (CONFIG_TCCBOOT path)
   |  2. open /home/foo.c via libc -> sys_open -> fat_vfs -> FAT16
   |  3. preprocess: chase #include <stdio.h>
   |      |  search path: /lib/tcc/include (TCC's own headers) then
   |      |               /usr/include (osnos libc + freestnd-c-hdrs)
   |      |  -> /lib/tcc/include/stdarg.h (TCC-friendly va_list)
   |      |  -> /usr/include/stdio.h (osnos libc)
   |      |  -> /lib/tcc/include/stdint.h (LP64 explicit types)
   |  4. parse + codegen: tccgen.c builds in-memory ELF sections
   |      | .text per-function (.text.printf, .text.fopen, ...)
   |      | .rela.text relocations for cross-section calls
   |  5. link: pull /lib/libc.a, /lib/tcc/libtcc1.a, /lib/crt[1in].o
   |      | resolve undefined refs against libc archive members
   |      | OSnOS PATCH: when static_link && output_type==EXE,
   |      |   rewrite R_X86_64_PLT32 -> R_X86_64_PC32 direct (no PLT)
   |  6. emit ELF: tcc_output_file writes ELF header + 2 LOAD
   |      | program headers (text RE + data RW) — NO .dynamic,
   |      | NO .interp, NO .plt. Entry = _start (crt1.o = osnos
   |      | crt0.S.o).
   |  7. fopen("/home/foo", "w") -> sys_write -> fat_vfs_append
   |      | (FASE 11.0 kmalloc heap scratch, was static 8KB cap)
   v
[ring 3]  user runs: /home/foo
[ring 0]  sys_execve -> proc_execve_replace -> elf_load(blob, ...)
   |  validates ehdr (ET_EXEC, X86_64), walks PT_LOAD entries,
   |  allocates pages, copies file content + zero-fills BSS,
   |  maps user stack at USER_STACK_TOP, returns entry+stack.
   v
[ring 3]  /home/foo starts at its _start, prints "hello from tcc!"
```

**Two VFS bugs the TCC port flushed out** (fixed in same FASE):
- **sys_read** had `char tmp[1024]` stack scratch + read the whole
  file -> files >1 KiB silently truncated. New `vfs_read_at(off)`
  API + per-backend offset support drops slurp-then-slice and
  reads exactly `count` bytes from `off`. ~10x faster for
  incremental reads (TCC reads headers in 8 KiB chunks).
- **fat_append_path** had `static char scratch[8192]` -> files
  capped at 8 KiB on disk. TCC's 50 KB ELF output got truncated
  mid-header. New: `kmalloc(existing+len)` heap scratch with 4 MiB
  cap.

**Two TCC behaviors that needed patches**:
- TCC's PLT/GOT emission persists in static-link mode (no dynamic
  loader can resolve them) -> `R_X86_64_PLT32` rewritten to direct
  `R_X86_64_PC32` when `static_link && output_type==EXE && symbol
  resolved`. Eliminates `.plt`+`.got` entirely from output ELFs.
- TCC's stock `stdarg.h` uses anonymous unions that its own
  preprocessor mis-parses (yields "missing #endif" cascades).
  Trimmed to a flat struct + no line-continuations in
  `vendor/tinycc/include/stdarg.h`.

## Lua self-hosting (FASE 11.2)

osnos ships Lua 5.4.7 at `vendor/lua/` cross-compiled into
`/bin/lua` (~1.2 MB ELF). Both programs and the interactive REPL
work — `ovi script.lua` -> `lua script.lua` is the canonical
workflow inside the guest.

REPL mode (no script arg) loops:
- `fputs("> ", stdout); fflush(stdout)` — prompt
- `fgets(buf, MAX, stdin)` — read line (shellsrv pre-spawn did
  `leave_raw()` so the kernel TTY is canonical+ECHO; user sees
  what they type)
- `loadbuffer + lua_pcall` — compile + run
- Loop until fgets returns NULL (EOF via Ctrl+D or `os.exit()`)

**No POSIX integration**: no signal handlers (SIGINT inside Lua
falls to default = task killed, suffices for hobby use), no
readline (line editing relies on shellsrv's, not Lua's), no
`os.execute` (returns -1), no `io.popen` (errors), no `require
"native_pkg"` for C extensions (loadlib needs dlopen). Lua-only
modules via `require "pure_lua_pkg"` work fine since they just
parse/exec Lua source.

**libc gap-fill needed for Lua build**: `<locale.h>` (setlocale
stub + struct lconv), `sig_atomic_t` in signal.h, math.h with
`asin/acos/sinh/cosh/tanh/frexp/modf` (real impls via Taylor +
IEEE-754 pun), time.h with `clock()` + `mktime()` + `strftime()`
+ `difftime()` (subset Y/m/d/H/M/S/etc that `os.date()` uses),
stdlib `system()` no-op stub, stdio `tmpnam()` + `remove()`,
string `strcoll/strxfrm` (delegate to strcmp in C-locale).

## Shutdown / reboot (FASE 10.7 polish)

`sys_reboot` (#169 Linux ABI) in `src/micro/syscall.c` gives
platform control to userland:

```
[ring 3] poweroff main -> reboot(RB_POWER_OFF=0x4321FEDC)
  -> osnos_syscall1(SYS_REBOOT=169, cmd)
  v
[ring 0] sys_reboot(cmd):
  RB_POWER_OFF:    outw(0xB004, 0x2000)   ; QEMU -M pc ACPI S5
                   outw(0x0604, 0x2000)   ; QEMU -M q35 ACPI S5
                   outw(0x4004, 0x3400)   ; VirtualBox shutdown
                   outb(0x0501, 0x00)     ; QEMU isa-debug-exit
                   cli; hlt (forever)     ; bare metal fallback

  RB_AUTOBOOT:     outb(0x64, 0xFE)       ; 8042 kbd reset line
                                          ; universal (real HW too)
                   cli; hlt

  RB_HALT_SYSTEM:  cli; hlt
```

No magic1/magic2 cookies from Linux — osnos is trusted, simpler.
Enables CI scripts: `./build_and_run.sh headless <<<"alltest;
poweroff"` runs the test battery + clean exit + propagates exit
code to the host.

## Conscious limitations (post-FASE 10)

- **Preemption only in CPL=3**: ring-0 tasks (keyboard feeder)
  remain cooperative. An infinite kernel-side loop hangs. But the
  feeder is simple: reads scancodes and pushes to ring buffer,
  no dangerous paths. Future fix = lockless IRQ-driven scheduler
  without cooperative loops.
- **Single shared IPC queue** (64 slots x 1 KB). A noisy server
  can fill it and block the others with EAGAIN. Applied
  mitigations: kbdsrv no longer sends IPC_KEY_EVENT (shellsrv
  reads from TTY directly); ovi buffers its render in 16 KB +
  single-write. Real fix = per-server queue or explicit
  backpressure per sender.
- **VFS without permissions**: no uid/gid, no atime/mtime, no
  chmod. FAT16 has limited attrs and aliasfs passes them through.
- **POSIX core ABI 100% complete**: `fork(2)` + `execve(2)` +
  `wait/waitpid(2)` (with TASK_ZOMBIE) + `sigaction(2)`
  (sa_handler-only, sigframe-based delivery) + EINTR on blocking
  syscalls. Fork is still full page copy (no COW yet). osn_spawn
  (#266) coexists for the optimized atomic case.
- **User-mode signals**: `sigaction(2)` with sa_handler-only works
  (`SYS_RT_SIGACTION=13`, sigframe on user stack, libc
  `__sigtramp` + `SYS_RT_SIGRETURN=15`). Default disposition
  correctly applies (SIG_DFL -> `proc_exit_current_user(128+sig)`).
  SIGKILL/SIGSTOP uncatchable. **Pending**: real `sa_mask` /
  `sigprocmask` (today no-op stub); `SA_SIGINFO`+`siginfo_t`;
  automatic `SIGCHLD` on child exit; signals for faults
  (SEGV/FPE — today proc_exit hard).
- **No TLS / FS register / thread-local storage**.
- **No dynamic loader**. Only static `ET_EXEC`.
- **Only 16 simultaneous tasks** (MAX_TASKS = 16).
- **No SMP**: 1 core, 1 LAPIC.
- **Only IDE primary master** (ATA PIO LBA28, 1 disk).

## Flow of a pipeline: `ls /home | grep TXT | sort`

```
[ring 3]  shellsrv.run_pipeline(stages=[ls, grep, sort])
   |
   |  Creates 2 pipes (one between each pair of stages):
   |    pipe(p01) -> p01[0]=read, p01[1]=write (ls->grep)
   |    pipe(p12) -> p12[0]=read, p12[1]=write (grep->sort)
   |
   |  Spawn stage 0: "ls /home"
   |     osn_spawn("/bin/ls", "/home", envp,
   |               stdin_fd=-1 (default),   <- terminal stdin
   |               stdout_fd=p01[1])         <- write to pipe 0->1
   |     close(p01[1]) in shellsrv (the child takes it)
   |
   |  Spawn stage 1: "grep TXT"
   |     osn_spawn("/bin/grep", "TXT", envp,
   |               stdin_fd=p01[0],          <- read pipe 0->1
   |               stdout_fd=p12[1])         <- write pipe 1->2
   |     close(p01[0]); close(p12[1])
   |
   |  Spawn stage 2: "sort"
   |     osn_spawn("/bin/sort", "", envp,
   |               stdin_fd=p12[0],          <- read pipe 1->2
   |               stdout_fd=-1)             <- default stdout
   |     close(p12[0])
   |
   |  Wait the LAST pid (sort's). When sort closes, the previous
   |  ones already closed because their stdout pipes saw EOF.
   v
[ring 0]  sys_spawn implementation (in src/micro/syscall.c):
   |  proc_execve creates the child task. Then:
   |  if (stdin_fd >= 0)  child.fds[0] = caller.fds[stdin_fd];   MOVE
   |                       caller.fds[stdin_fd].used = false;
   |  (same for stdout). MOVES, not COPIES — the caller loses the
   |  slot so the pipe refcount is correct.
   v
ring-3 stages run in parallel, scheduler preempts them every 50 ms.
When one finishes with exit(0), proc_exit_current_user frees child
fds (decrements pipe refcounts; reaching 0 means subsequent read()
on the read-end return 0 = EOF).
```

## Flow of a direct osn_spawn (no pipes)

```
[ring 3]  shellsrv: dispatch("hello")
   |  resolve_cmd_path("hello") via PATH -> "/bin/hello"
   |  pack_envp(envbuf) -> "PATH=/bin\0HOME=/home\0...\0\0"
   |  osn_spawn("/bin/hello", "", envp, -1, -1)
   v
[lib/libc/include/osnos_ipc.h] inline:
   osnos_syscall5(SYS_SPAWN, path, args, envp, -1, -1)
   v
[ring 0]  sys_spawn:
   |  caller = task_current() (= shellsrv)
   |  copy_from_user for path / args / envp_flat (all to kernel scratch)
   |  unpack envp_flat ("k=v\0k=v\0\0") -> envp_array[MAX]
   |  child_pid = proc_execve(path_kbuf, args_kbuf, envp_array)
   |  if (stdin_fd  >= 0) move caller.fds[stdin_fd]  -> child.fds[0]
   |  if (stdout_fd >= 0) move caller.fds[stdout_fd] -> child.fds[1]
   |  return child_pid (positive) or -errno
   v
[ring 3]  shellsrv sees pid > 0:
   |  if background ("&"):  bg_jobs_remember(pid, cmd_label); return
   |  else:                  osn_set_fg(pid); wait_pid_or_stop(pid)
```

## Flow of a fork (SYS_FORK = #57 Linux)

`fork` creates a new task identical to the current one (same
memory image, same fds, same cwd), with the only difference that
the two return differently: parent sees the child pid, child sees
0.

```
[ring 3]  pid_t r = fork();
   |
   |  libc: osnos_syscall0(SYS_FORK=57)
   v
[ring 0]  sys_fork (src/micro/syscall.c):
   |
   |  parent = task_current()
   |
   |  --- allocations (all or nothing) ---
   |  child_pml4   = address_space_clone(parent->pml4)
   |     v walk pml4[0..255] -> pdpt -> pd -> pt -> leaf
   |     v for each present leaf: pmm_alloc_page() + memcpy via HHDM
   |     v vmm_map(child_pml4, virt, new_phys, flags)
   |  child_kstack = kmalloc(USER_KSTACK_BYTES)
   |  child_pid    = task_create(parent->name, parent->entry)
   |     (any of the 3 fails -> free previous + return -ENOMEM)
   |
   |  --- per-task state ---
   |  child->pml4 / kernel_stack / heap / mmap / fpu = parent's
   |  child->cwd / redirs / fds[0..15] = parent's
   |     for each fd is_pipe: pipe_dup_reader/writer (++ ref_w/r)
   |  child->kill_pending / stop_pending = 0  (clean slate)
   |
   |  --- snapshot of syscall context ---
   |  iret = parent->kernel_stack_top - 40
   |  child->saved_iret_{rip,cs,rflags,rsp,ss} = iret[0..4]
   |  sf = iret - sizeof(syscall_frame_t)
   |  child->saved_{rbx,rcx,rdx,...,r15} = sf->{rbx,...}
   |  child->saved_rax = 0                   <- child sees fork()=0
   |  child->saved_valid = 1
   |  child->state = TASK_READY
   |
   |  return child->pid                       <- parent sees fork()=child_pid
   v
[scheduler] eventually dispatches the child:
   |  user_task_trampoline: saved_valid=1 -> "replay" path
   |     - load CR3 = child_pml4 (kernel high-half intact, user low
   |       half is the cloned copy)
   |     - push saved_iret_{ss,rsp,rflags,cs,rip}
   |     - mov saved_rax -> %rax  (0 in child!)
   |     - restore saved_rbx..r15
   |     - iretq
   v
[ring 3 — CHILD]
   |  arrives at the instruction RIGHT AFTER the syscall, with
   |  rax=0 (so libc fork() returns 0).
   |  All memory writes the child does from here on go to ITS pml4
   |  (the cloned copy), not the parent's.
```

**Critical invariants**:
- Atomic: if `address_space_clone` or `kmalloc(kstack)` or
  `task_create` fail, the parent is NOT modified. fork returns
  -errno and the parent continues normally.
- No COW: each fork immediately duplicates ALL parent's user
  pages. For a process with 1 MiB of heap, fork costs 1 MiB of
  physical RAM instantly. Enough for our programs (~64 KiB
  typical); optimizable to COW later.
- Shared fds (with refcount): pipe opened by parent -> child also
  sees it. When either closes, it only decrements the counter;
  the pipe lives until both close.
- No automatic SIGCHLD: the parent does not receive SIGCHLD when
  the child dies. Use `wait(2)` / `waitpid(2)` (blocking via
  TASK_ZOMBIE + parent_pid + async wake-up from
  `proc_exit_current_user`). forktest and waittest use this
  POSIX-real API. shellsrv still polls with `sys_taskinfo` to
  support `WUNTRACED` (Ctrl+Z), but user-mode apps use waitpid
  directly. When SIGCHLD arrives the classic `signal(SIGCHLD,
  reaper); fork(); wait();` pattern will be possible.

## Flow of an execve (SYS_EXECVE = #59 Linux)

Unlike `osn_spawn` which creates a NEW task, `execve` kills the
current user-mode image and loads another in its place,
**keeping the same pid**. It is the "exec" block of fork+exec.

```
[ring 3]  shellsrv uses `exec` builtin (do_exec):
   |  resolve_cmd_path(name) -> /bin/foo via $PATH
   |  leave_raw()                <- cooked TTY for the new image
   |  osn_set_fg(getpid())       <- claim foreground (pid preserved)
   |  execve("/bin/foo", argv, environ)
   v
[ring 3 libc] execve wrapper:
   |  osnos_syscall3(SYS_EXECVE=59, path, argv, envp)
   v
[ring 0]  sys_execve (src/micro/syscall.c):
   |  copy_from_user of path / argv[] (including strings) / envp[]
   |  args_kbuf = join argv[1..] with spaces
   |  envp_arr  = kernel array NULL-terminated
   |  proc_execve_replace(path, args_kbuf, envp_arr)
   v
[ring 0]  proc_execve_replace (src/proc/exec.c):
   |  resolve_executable(path) -> blob (VFS first, ROM fallback)
   |  elf_load(blob) -> new_pml4 / new_entry / new_stack_top
   |  build_argv_block(new_pml4, new_stack_top, name, args, envp)
   |     -> init_rsp on the new stack
   |
   |  --- all OK, atomic swap ---
   |  old_pml4 = t->pml4
   |  t->pml4 = new_pml4
   |  t->user_entry / user_stack_top / heap_* / mmap_* / redirs reset
   |  t->saved_valid = 0          <- fresh start, no resume from prev
   |  task name = basename(path)
   |  fpu_state_init(t->fpu_state)
   |  address_space_destroy(old_pml4)   <- frees the old AS
   |
   |  sched_resume_jump()                <- never returns
   v
[scheduler] next dispatch enters user_task_trampoline
   |  saved_valid=0 -> "first-time" path -> iretq with t->user_entry
   |  and t->user_stack_top (= init_rsp of the new image).
   v
[ring 3]  /bin/foo starts at its _start, reads argv/envp from the
          stack, calls its main(). Same pid, same inherited fds,
          same cwd. shellsrv no longer exists — execution continues
          with the new binary.
```

**Key invariants**:
- Only the user-mode portion of the task changes. PID, kstack,
  fds[16], cwd, slot index, kernel_fg_pid (if it was ours) — all
  the same.
- ATOMIC: if elf_load fails, we return -errno and the old image
  remains alive. Nothing is destroyed before the new one is ready.
- POST EXIT: the `init-respawn` watchdog detects when the new
  image (e.g. `/bin/top`) dies and brings shellsrv back. Without
  it, an interactive `exec /bin/top` would leave the system
  shell-less after Ctrl+C.

## Flow of a wait (SYS_WAIT4 = #61) + TASK_ZOMBIE

`wait/waitpid` blocks the parent until a child enters TASK_ZOMBIE.
Lifecycle:

```
parent: pid_t r = waitpid(child, &status, 0);
   |  libc -> osnos_syscall4(SYS_WAIT4, child, &status, 0, 0)
   v
[ring 0] sys_wait4:
   |  1st sweep: walk task table for tasks with parent_pid == self.
   |  - If any is ZOMBIE: status = encode_wait_status(exit_code)
   |    (WIFEXITED << 8 or WTERMSIG & 0x7f); copy_to_user;
   |    transition ZOMBIE -> DEAD; return child pid. OK
   |  - If no ZOMBIE but there are live children: WNOHANG -> 0;
   |    otherwise snapshot iret+GPRs (saved_rax=0), state=BLOCKED,
   |    waiting_for_pid=child, wait_status_ptr=u_status,
   |    sched_resume_jump.
   |  - If no children at all: -ECHILD.
   v
[meanwhile]  child eventually calls _exit(N):
   v
[ring 0] proc_exit_current_user(N):
   |  parent_t = task_by_pid(t->parent_pid).
   |  state = parent alive ? TASK_ZOMBIE : TASK_DEAD.
   |  If parent is BLOCKED waiting for us (-1 or == our pid):
   |    - status_value = encode_wait_status(N).
   |    - vmm_lookup(parent->pml4, parent->wait_status_ptr) ->
   |      HHDM-mapped phys + offset -> write int.
   |    - parent->saved_rax = our pid (wait4 return value).
   |    - parent->state = READY.
   |  address_space_destroy(our pml4).
   |  reaper_add_kstack(our kstack).
   |  sched_resume_jump.
   v
[scheduler] dispatches parent -> user_task_trampoline -> saved_valid
   path -> iretq with rax = child pid.
   v
[ring 3] wait4 returns child pid. *status written via pml4.
   |  If a signal arrived before child wake -> sig_pending != 0 or
   |  saved_rax = -EINTR (sys_kill wrote it). user_task_resume
   |  delivers signal or syscall returns -EINTR.
```

**Design decision**: ZOMBIE instead of "extend reap grace" — a
ZOMBIE slot stays explicitly until wait() consumes it (transition
ZOMBIE -> DEAD). The reaper only reaps DEAD. This preserves the
exit_code without races and allows a future `ps` to show
"defunct" like Linux.

**Orphan handling**: if parent_pid is 0 (kernel task) or parent
is already DEAD/UNUSED, the child at exit goes directly to DEAD
(skipping ZOMBIE) — the reaper picks it up. No "init" pid 1 yet.

## Flow of a signal (sigaction + delivery + sigreturn)

POSIX sa_handler-only. The kernel keeps 32 handlers per task,
delivery in `user_task_resume` right before the iretq.

```
[ring 3] sigaction(SIGINT, &act, NULL):
   |  libc wrap: if !act.sa_restorer, fill with __sigtramp.
   v
[ring 0] sys_rt_sigaction(SIGINT, &act, NULL, _):
   |  Validation: SIGKILL/SIGSTOP -> EINVAL.
   |  copy_from_user(&kact, &act, sizeof(kuser_sigaction_t)).
   |  t->sa_handler [1] = kact.sa_handler.
   |  t->sa_restorer[1] = kact.sa_restorer.
   v
... later, signal delivery:

[ring 3] another task does kill(t->pid, SIGINT) or user hits Ctrl+C:
   v
[ring 0] sys_kill / tty_signal:
   |  t->sig_pending |= 1 << (SIGINT-1) ;
   |  If t->state == BLOCKED:
   |    If t->saved_valid: t->saved_rax = -EINTR;
   |    t->state = READY;
   |  return.
   v
[scheduler] dispatches t -> user_task_trampoline -> user_task_resume:
   |  Build buf[] from iret+GPRs as usual.
   |  Loop: lowest_signal(t->sig_pending) = SIGINT.
   |        handler = t->sa_handler[SIGINT-1].
   |        If SIG_IGN (1): clear bit, continue.
   |        If SIG_DFL (0): SIGINT/TERM/etc -> proc_exit_current_user(128+sig);
   |          SIGCHLD/URG/WINCH -> clear+continue.
   |        If fn ptr: build sigframe_t on user stack via
   |          write_other_user(t->pml4, sigframe_base, ...). 160 B
   |          aligned to 16. Write restorer at sigframe_base - 8.
   |          buf[4] = handler. buf[1] = restorer_slot.
   |          buf[10] = signum (rdi). Clear bit. break.
   |  iretq with modified buf.
   v
[ring 3] enters the handler with rdi=signum, rsp pointing to restorer.
   handler runs user code; when it does `ret`, pop restorer.
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
[scheduler] re-dispatches t -> user_task_resume restores saved_* ->
   iretq to the RIP/RSP right after the original syscall (before
   the signal). RAX = what it was before the signal (e.g. -EINTR
   if we were in an interrupted wait4).
```

**Invariants**:
- `sa_handler == 0` = SIG_DFL, `== 1` = SIG_IGN, others = user ptr.
- Only ONE signal is delivered per dispatch (loop break after
  successful delivery). Other pending wait for the next iretq.
- Stack overflow: if user stack doesn't reach sigframe_base
  (~200 B below orig RSP), `write_other_user` returns 0 and we
  drop the signal silently. Better than kernel panic.
- POSIX exec-on-handler: NOT implemented (on fork handlers are
  inherited, on execve they're reset in proc_execve_replace —
  TODO).

## VFS layered backend dispatch

```
vfs_stat("/bin/hello")
   |  check_path  -> validates path, starts with '/'
   |  find_mount(longest-prefix):
   |    /bin -> aliasfs (post-FASE 10 + FAT mounted)
   |  -> aliasfs_ops->stat(priv, "/bin/hello"):
   |       priv = { mountpoint="/bin", target="/sd/bin" }
   |       rewrites path: "/bin/hello" -> "/sd/bin/hello"
   |       recursively calls vfs_stat("/sd/bin/hello")
   |  -> find_mount("/sd/bin/hello") = fat16
   |  -> fat_vfs_ops->stat(priv, "/sd/bin/hello"):
   |       strip_mount: "/sd/bin/hello" -> "/bin/hello" (internal to FAT)
   |       fat_lookup walks dir entries (with LFN if needed)
   |       returns vfs_stat_t { type=REG, size=44384 }
```

Aliasfs is trivial (rewrite + delegate); fat is the only backend
with real state (BPB, FAT, ATA reads). Sysfs/devfs/binfs are
synthetic — they generate responses on demand without storage.

**Offset-native reads (FASE 11.0)**: `vfs_ops_t.read` receives an
`off` parameter; each backend honors the offset (`fat_read_file`
propagates it to the cluster walk, ramfs/sysfs/binfs slice in
buffer-local, devfs ignores it because its streams have no
offset). There is also a public `vfs_read_at()` API for callers
that need random access. This replaced the old "read whole file
to scratch + slice" scheme that had a silent 1 KiB cap in
sys_read — that's what TCC's port surfaced as the first big bug.

## Ox flow — a mouse click opens an app (FASE 12)

Example: user types `oxsrv &`, then right-clicks on the wallpaper,
then clicks "Notepad" in the menu.

```
   user moves the mouse / clicks
        |
        v
   PS/2 AUX port -> byte arrives at 0x60 with STAT_AUX_DATA bit
        |
        v
   [ring 0] mouse_server_tick (cooperative kernel task)
        |  mouse_poll() reads byte, assembles 3-byte packet
        |  decode dx/dy/buttons -> mouse_event_t
        |  devfs_mouse_push(ev) — to /dev/mouse0 ring (32 slots)
        v
   scheduler dispatches oxsrv
        |
        v
   [ring 3] oxsrv main loop (33 ms tick)
        |  read(/dev/mouse0) drain to EAGAIN (O_NONBLOCK)
        |  process_mouse_event: cx,cy += dx,dy; clamp screen bounds
        |  edge-detect right-button down over wallpaper:
        |    g_menu_visible = 1; g_menu_x = cx; g_menu_y = cy
        |  g_dirty = 1
        v
   composite_and_flush()
        |  memcpy(g_back, g_wp_scaled, w*h*4)        — wallpaper
        |  draw windows back-to-front (frame+body+title+close [x])
        |  draw_menu (if visible)                    — 5 entries
        |  draw_cursor                               — 12x17 sprite
        |  ioctl(fb_fd, FBIO_BLIT, &req)             — syscall
        v
   [ring 0] sys_ioctl detects /dev/fb0 -> FBIO_BLIT case
        |  copy_from_user(&req, arg, sizeof(req))
        |  kmalloc(row_bytes)  — scratch for 1 row
        |  loop h rows: copy_from_user(scratch, src_row, row_bytes)
        |              + framebuffer_blit_kernel(...)
        |  kfree(scratch)
        v
   pixels visible on the framebuffer
```

User clicks on "Notepad" in the menu:

```
   left-button-down on y = g_menu_y + idx*MENU_ITEM_H
        |
        v
   [ring 3] oxsrv: g_menu[idx].path = "/bin/oxnotepad"
        |  g_menu_visible = 0
        |  spawn_app("/bin/oxnotepad")
        |     -> osn_spawn(path, "", envp_flat, -1, -1)
        v
   [ring 0] sys_spawn (#266)
        |  proc_execve("/bin/oxnotepad", "", envp_flat)
        |  ELF loader maps PT_LOADs in new address space
        |  build_argv_block: argc=1, argv[0]="oxnotepad", envp + auxv
        |  task_create_user_elf -> state=READY
        v
   scheduler dispatches oxnotepad
        |
        v
   [ring 3] oxnotepad main()
        |  ox_init() -> ipc_service_lookup(SERVER_OX) -> server pid
        |  ox_window_create(600, 400, "Notepad")
        |    -> IPC_OX_WINDOW_CREATE msg with arg0=(w<<16)|h, data=title
        |    -> ipc_send (to=SERVER_OX, kernel routes to oxsrv pid)
        |    -> wait for IPC_OX_RESPONSE (with arg1 = win_id)
        v
   [ring 3] oxsrv handle_ipc(IPC_OX_WINDOW_CREATE)
        |  alloc_window(from=oxnotepad_pid, w, h, "Notepad")
        |  malloc(w*h*4) backing buffer + push to z-stack
        |  send_response(from, OK, win_id) with retry-on-EAGAIN
        v
   [ring 3] oxnotepad receives RESPONSE
        |  load_file() from the path (argv[1] or default /home/notepad.txt)
        |  render() -> sequence of ox_draw_rect + ox_draw_text
        |    each one = IPC_OX_DRAW_RECT/TEXT to oxsrv
        |  ox_present(win) -> IPC_OX_PRESENT marks window dirty
        |  ox_wait_event(&ev) — event delivery loop
```

**Key changes FASE 12 introduced to the rest of the system**:

- `keyboard.c` now skips bytes with STAT_AUX_DATA from the 8042 —
  without this, mouse bytes contaminated the TTY (random numbers
  when moving the mouse).
- `consrv.c` adds a `suspended` flag + watchdog that checks
  `ipc_service_lookup(SERVER_OX)` every ~32 IPC msgs: if oxsrv
  disappeared (kill -9 / crash), auto-RESUME and `\x1b[2J\x1b[H`
  to the FB for a clean canvas.
- `kbdsrv.c` same pattern but with `O_NONBLOCK` on `/dev/input0`
  (needed to multiplex IPC polling with read).
- `ipc.c` `ipc_send` now does **two-step routing**: first
  `service_get_pid(msg->to)`; if it returns 0, fallback to treat
  `to` as a literal pid + `task_by_pid(pid)` to validate. This
  lets oxsrv send events to clients (which have no SID of their
  own).
- `exec.c` `build_argv_block` now places **auxv** after the envp
  NULL — without this, musl `__init_libc` reads random bytes as
  aux keys and crashes or sets libc.page_size = nonsense.

## musl flow — hello_musl bootstrapping (FASE 13)

```
   user types: hello_musl alpha beta
        |
        v
   shellsrv -> osn_spawn("/bin/hello_musl", "alpha beta", ...)
        |
        v
   [ring 0] sys_spawn -> proc_execve
        |  elf_load: parses PT_LOAD, maps pages with PTE_U
        |  build_argv_block writes on the user stack:
        |    [argc=3] [argv: hello_musl, alpha, beta, NULL]
        |    [envp: PATH=/bin, HOME=/home, ..., NULL]
        |    [auxv: {AT_PAGESZ=6, 4096}, {AT_NULL=0, 0}]
        |  iretq to the entry point (musl's crt1.o)
        v
   [ring 3] musl crt1.o `_start` (arch/x86_64/crt_arch.h)
        |  xor %rbp,%rbp        — clear frame pointer
        |  mov %rsp,%rdi        — pass SP block as 1st arg
        |  call _start_c
        v
   _start_c -> __libc_start_main(main, argc, argv, ...)
        |
        v
   __init_libc(envp, pn)
        |  walks envp until NULL -> auxv = envp + i + 1
        |  for (i=0; auxv[i]; i+=2) cache aux entries
        |    -> libc.page_size = aux[AT_PAGESZ] = 4096   OK
        |    -> AT_RANDOM = 0 (no SSP entropy, SSP off OK)
        |  __init_tls(aux) -> static_init_tls
        |    -> __init_tp(__copy_tls(builtin_tls))
        |    -> __set_thread_area asm:
        |        movl $0x1002,%edi  ; ARCH_SET_FS
        |        movl $158,%eax     ; SYS_arch_prctl
        |        syscall
        v
   [ring 0] sys_arch_prctl(0x1002, fs_base)
        |  wrmsr MSR_FS_BASE = fs_base
        |  -> from now on any %fs:* access works (errno, TLS)
        v
   __libc_start_init() — __init_array + DT_INIT (empty in static)
        |
        v
   main(argc=3, argv) runs with valid TLS
        |  printf / write call sys_write or sys_writev
        |  sys_writev iterates iov, reuses sys_write for each one
        |  text comes out via consrv -> /dev/fb0
        v
   main returns -> __funcs_on_exit + fflush -> SYS_EXIT
```
