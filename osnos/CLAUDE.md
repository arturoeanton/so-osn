# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository layout

`osnos/` is the **self-contained** kernel + userland tree. The Limine
boot template is no longer a parent directory — its freestanding
helpers were vendored into `kernel-deps/` and the top-level
`GNUmakefile` here builds everything (kernel ELF, libc, user ELFs,
ISO, sd.img) and runs QEMU directly.

- `GNUmakefile` — single top-level Makefile. Builds the kernel, every
  user ELF (`elfs/`), the libc (`lib/libc/`), the TCC/Lua/jq ports
  (`vendor/`), the ISO and the FAT16 `sd.img`. Also wraps `qemu-system`.
- `limine.conf` — boot entry; loads `boot():/boot/kernel`.
- `kernel-deps/` — Limine-supplied freestanding helpers:
  `cc-runtime/`, `freestnd-c-hdrs/`, `limine-protocol/`,
  `linker-scripts/`, plus `get-deps` to fetch them. The build
  references these via `kernel-deps/…` paths.
- `src/` — kernel sources.
- `lib/libc/` — osnos user-side libc (compiled separately with
  `USER_CFLAGS`, NOT linked into the kernel). Headers in
  `lib/libc/include/` are what user ELFs `#include`.
- `lib/sysroot/` — `crti.S` / `crtn.S` stubs for the TCC sysroot.
- `elfs/` — user-mode programs compiled into ring-3 ELFs.
  - `shell/` — `osh` (script interpreter).
  - `tools/` — coreutils + extras (~60 entries: `ls cat cp mv rm
    mkdir touch echo head tail wc grep sort uniq cut tr seq yes
    tee env pwd which printf date uname basename dirname clear tree
    banner calc top kill sleep ovi less readelf poweroff reboot
    minishell term mousetest`, plus `tcc`, `lua`, `jq` from vendor).
  - `net/` — `tcpclient udptest echotcp selecttest selectserver httpd`.
  - `tests/` — libc / kernel smoke tests (~28 entries: `alltest
    libctest forktest exectest waittest sigtest sigchldtest
    pgrouptest jobtest pipetest ptytest mmaptest fbtest inputtest
    kerntest spawntest envtest fptest ofdtest fdedgetest termtest
    serialtest tcctest luatest jqtest ttytest hello_libc user_hello`).
  - `osn-server/` — **the actual ring-3 servers**: `consrv.c`,
    `kbdsrv.c`, `shellsrv.c` (FASE 10 done).
  - `libc.lds` — linker script shared by libc-linked ELFs.
  - `tests/user_hello.lds` — bare ELF's own linker script.
- `vendor/` — `tinycc/` (0.9.27), `lua/` (5.4.7), `jq/` (1.7.1) ported
  as ring-3 ELFs against osnos libc.
- `build/` — all outputs (kernel ELF, object files, per-program ELFs,
  ISO at `build/osnos-x86_64.iso`).
- `sd.img` — 16 MiB FAT16 disk image, rebuilt by the Makefile on every
  build. Populated with `/bin/<every ELF>`, `/home/`, `/lib/` (TCC
  sysroot: crt scaffolding + `libc.a` + `libtcc1.a`) and `/usr/include/`
  (full libc headers + freestanding headers). Used by QEMU as primary
  IDE disk.

## Build & run

Host requirements: `clang`, `ld.lld`, `xorriso`, `mtools` (mformat /
mcopy / mmd), `qemu-system-x86_64`, and a system-installed **Limine**
(`brew install limine` on macOS, distro package on Linux). The
Makefile auto-detects Limine under `/opt/homebrew/share/limine`,
`/usr/local/share/limine`, or `/usr/share/limine`. Override with
`make LIMINE_DIR=/path/to/limine …`.

From this directory:

- `make` (or `make all`) — build `build/kernel` and `sd.img`.
- `make iso` — also produce the bootable ISO.
- `make run` / `make run-bios` — build everything and boot in QEMU
  (SeaBIOS, `-M pc`). Same target.
- `make clean` — wipe `build/` and `sd.img`.

QEMU is launched with `-M pc` (NOT `q35`) — the `block_ata` driver
talks PIO to the legacy 0x1F0 ports; q35 attaches disks to AHCI and
the driver wouldn't see anything. NIC is `rtl8139`, with slirp NAT
hostfwds `tcp::8080-:80`, `tcp::9034-:9034`, `udp::1234-:1234` so
`httpd` and the `net/` demos are reachable from the host.

Toolchain: `clang` + `ld.lld`. Kernel CFLAGS are `-mcmodel=kernel
-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone` etc. User
ELFs build with `USER_CFLAGS` — small code model, ring-3 freestanding,
**x87 + SSE + SSE2 left on** (SysV ABI default; the kernel never
touches FP registers, ring-3 may use double/float freely). The
kernel `GNUmakefile` globs `src/**/*.c` plus
`kernel-deps/cc-runtime/src/*.c` via `find`, so **new `.c` files under
`src/` are picked up automatically**. `-Werror` is on for `src/`
(not for cc-runtime / vendor).

## Architecture

OSnOS is a small microkernel-style hobby OS booted by Limine on a
linear framebuffer. The kernel hosts drivers, the VFS, IPC, the
scheduler, and a syscall layer. Console, keyboard policy and the
shell themselves run as ring-3 ELFs (`/bin/consrv`, `/bin/kbdsrv`,
`/bin/shellsrv`) since FASE 10. User tasks have their own page
tables and enter the kernel via `syscall` (preferred) or `int 0x80`
(legacy compat).

Scheduler: timer-driven preemptive in CPL=3 (50 ms quantum), still
cooperative for ring-0 tasks (kernel servers must yield). PIT @ 100 Hz
on IRQ 0.

See `ARCH.md` for a full layered diagram + IPC and syscall
walkthroughs. See `STATUS.md` for the running history of what works
today (current phase: FASE 11.4 — PS/2 mouse + `/dev/mouse0`; FASE
11.0–11.3 added TCC/Lua/jq self-hosting). Both are in Spanish and
much more current than this file.

Boot path (`src/kernel/main.c`, `kmain`):

1. `serial_init(COM1)` — UART first so panics have a sink even if
   the framebuffer fails.
2. Validate Limine base revision and framebuffer response; `framebuffer_init`.
3. Memory layer: `pmm_init → vmm_init → kheap_init`.
4. CPU + IRQ tables: `gdt_init → tss_init → idt_init → uaccess_init →
   syscall_msr_init → fpu_init → pic_init → lapic_init → timer_init`.
   - `uaccess_init` registers the copy_*_user span in the extable
     so a fault inside the loop unwinds to EFAULT instead of
     panicking.
   - `syscall_msr_init` enables EFER.SCE and programs STAR/LSTAR/FMASK
     so ring-3 code can use `syscall`.
   - `fpu_init` flips CR0/CR4 + FNINIT so ring-3 can use SSE/x87.
   - `pic_init` remasks all 8259 lines; `lapic_init` enables LAPIC
     with LINT0=ExtINT; `timer_init` programs PIT@100Hz + IDT[0x20].
5. Device init: `block_ata_init` (IDENTIFY primary master), `rtl8139_init`
   (PCI scan; silent if no NIC), `net_init` (ARP + RX dispatch).
6. Microkernel state: `ipc_init → pipe_init → pty_init → task_init →
   reaper_init → scheduler_init → syscall_init → ramfs_init → bootstrap_fs`.
7. Spawn the kernel-side hardware feeders as cooperative ring-0 tasks:
   `keyboard` (drains PS/2 into `/dev/input0`), `mouse` (PS/2 AUX →
   `/dev/mouse0`), `serial-in` (COM1 RX → `tty_input`).
8. Spawn the ring-3 servers via `proc_execve("/bin/consrv")`,
   `/bin/kbdsrv`, `/bin/shellsrv`, registering each one against its
   `SERVER_*` ID. An `init-respawn` watchdog task respawns any of
   the three if they die (e.g. `exec` from the interactive shell).
9. `keyboard_server_init()` + `mouse_server_init()` for the feeders.
10. `sti` to enable IRQs.
11. `scheduler_loop()` — saves a longjmp resume point at the top of
    the `for(;;)`; called from any nested context (`sys_exit`, fault
    handlers) via `sched_resume_jump()`. Each tick first drains the
    reaper, then dispatches the next READY task.

### Subsystem map

- `src/micro/` — kernel core.
  - `task.{c,h}` — fixed table of 16 tasks. States
    UNUSED/READY/RUNNING/BLOCKED/STOPPED/ZOMBIE/DEAD. Each task carries
    ring-3 fields (`pml4`, `kernel_stack_*`, `user_entry`, `user_stack_top`),
    saved iret + GPR set (for fork/sleep/signals), per-task fd table,
    `pgid`, `sid`, `parent_pid`, signal disposition tables, etc.
  - `scheduler.{c,h}` — preemptive (CPL=3 only) on top of a
    cooperative ring-0 loop. `scheduler_loop` is the long-jump host.
    Timer IRQ checks the quantum; user tasks get preempted, kernel
    tasks must still yield by returning.
  - `timer.{c,h}` — PIT @ 100 Hz, monotonic `timer_ms()`.
  - `ipc.{c,h}` + `src/include/osnos_ipc_abi.h` — `ipc_msg_t`
    (1024-byte payload + arg0/arg1, typed via `ipc_type_t`). Single
    shared queue of 64. **The ABI types live in `osnos_ipc_abi.h`**
    so ring-3 servers see the same wire shape; `src/micro/ipc.h` only
    holds the kernel-internal helpers. Ring-3 reaches the queue via
    `SYS_IPC_SEND` / `SYS_IPC_RECV`. `ipc_send` returns
    `osnos_status_t` (OK / EAGAIN / ESRCH); callers MUST check.
  - `service.{c,h}` — name→pid registry. `SERVER_KEYBOARD=1`,
    `SERVER_SHELL=2`, `SERVER_CONSOLE=3`, `SERVER_FS=4` (reserved but
    no longer used — the shell talks VFS directly via syscalls since
    FASE 10.3).
  - `gdt.{c,h}` + `tss.{c,h}` — GDT (kcode 0x08, kdata 0x10, udata 0x18,
    ucode 0x20, TSS 0x28). User-data BEFORE user-code is required so
    SYSRET64 lands on the right selectors. `tss.rsp0` mirrored in
    `tss_kernel_rsp0` for the SYSCALL stub.
  - `idt.{c,h}` — 256-entry IDT. `fault_try_recover` runs before
    every panic: kernel-mode RIP in extable → frame->rip rewrite;
    user-mode CPL=3 → `proc_exit_current_user(139)`; otherwise panic.
  - `extable.{c,h}` — `{rip_start, rip_end, recovery_rip}` table for
    kernel-mode page-fault recovery.
  - `uaccess.{c,h}` — `copy_from_user` / `copy_to_user`, asm core
    redirected to `__uaccess_copy_bytes_fault` via the extable.
  - `fpu.{c,h}` — CR0/CR4 + FNINIT at boot. **No per-task FXSAVE
    yet** — concurrent FP use across user tasks may corrupt state.
  - `reaper.{c,h}` — queues kstacks freed at task death; drained at
    the top of every `scheduler_tick`. Also collects DEAD slots.
  - `syscall.{c,h}` — Linux x86_64 syscall numbers + `syscall_dispatch(frame)`.
    Implements ~60 syscalls today: full POSIX core (`fork`, `execve`,
    `wait4`, `kill`, `rt_sigaction`/`sigreturn`, `pipe`, `dup`/`dup2`,
    `mmap`/`munmap`, `brk`, `nanosleep`, `getdents`, `getcwd`/`chdir`,
    `stat`/`fstat`/`access`, `time`/`clock_gettime`, `mkdir`/`rmdir`/
    `unlink`/`rename`, full BSD sockets, `select`, `ioctl` for termios,
    `fcntl`, process groups + sessions), plus osnos-specific (#250+):
    `SYS_ISATTY`, `SYS_IPC_SEND/RECV`, `SYS_SERVICE_REGISTER/LOOKUP`,
    `SYS_TTY_INPUT`, `SYS_TASKINFO`, `SYS_SPAWN`, `SYS_SET_FG`,
    `SYS_RESUME`.
  - `int80.c` — IDT[0x80] entry stub (legacy compat ABI).
  - `syscall_msr.{c,h}` — programs EFER.SCE, STAR, LSTAR, FMASK.
  - `syscall_entry.c` — `syscall` instruction entry stub. Mirrors the
    int80 stub but saves user RSP (`syscall_user_rsp`) and preserves
    RCX/R11 for SYSRET64.
  - `pmm.{c,h}` / `vmm.{c,h}` / `kmalloc.{c,h}` — phys / virt / heap.
  - `fd.{c,h}` — per-task fd table (regular files, pipes, ptys, sockets,
    tty, devfs entries). OFD shared offsets across `dup`/`fork`.
  - `pipe.{c,h}` — kernel pipe object (ring buffer, refcounted ends).
  - `pty.{c,h}` — `/dev/ptmx` + `/dev/pts/N` pool used by `term` and
    the libc `posix_openpt` family.
  - `tty.{c,h}` — POSIX line discipline. termios canonical / raw,
    echo, signal generation (Ctrl+C/Z → SIGINT/SIGTSTP to fg pgid),
    EINTR on blocked reads. Backs both PS/2 and serial input.
- `src/drivers/`
  - `framebuffer` — handles `\b`, `\n`, `\r`, `\t` in `draw_string`;
    also exposes `/dev/fb0` (writes paint pixels).
  - `keyboard` — PS/2; tracks shift/ctrl, extended `0xE0`; emits
    `keyboard_event_t { ascii, keycode }` (keycode uses Linux
    `input-event-codes.h` values).
  - `mouse` — PS/2 AUX; 3-byte packets → `mouse_event_t { dx, dy, buttons }`
    pushed into `/dev/mouse0` (ring of 32 events).
  - `serial` — 16550 UART on COM1. RX/TX + IRQ 4; also exposes
    `/dev/ttyS0`. Used for dual-console and headless boot.
  - `block_ata` — ATA PIO on 0x1F0 (primary master). Backs FAT16.
    Only attaches under QEMU `-M pc`.
  - `pic` / `lapic` — 8259 remap + LAPIC enable.
  - `pci` — bus scan; used by `rtl8139_init`.
  - `rtl8139` — RTL8139 NIC driver (RX ring + TX descriptors). Backs
    `src/net/eth.c`.
- `src/net/` — IPv4 stack. `eth → arp → ip → {icmp, udp, tcp}`,
  plus `socket.c` (the BSD-style fd backend used by the socket
  syscalls).
- `src/servers/` — kernel-side feeders only (the real "servers" are in
  `elfs/osn-server/`).
  - `keyboard_server` — `keyboard_poll` → `/dev/input0` ring. Does NOT
    own `SERVER_KEYBOARD`; that belongs to ring-3 `kbdsrv`.
  - `mouse_server` — same pattern for `/dev/mouse0`.
  - `serial_input_server` — drains COM1 RX into `tty_input`.
- `src/fs/`
  - `ramfs.{c,h}` — 32 slots × 128B path × 512B data. **Slot
    ownership invariant**: slot index stable for the entry's
    lifetime; deletion marks `used=false` without compacting;
    `const ramfs_file_t *` from `ramfs_find` stays valid until that
    specific entry is deleted.
  - `ramfs_vfs.{c,h}` — VFS adapter for ramfs.
  - `fat.{c,h}` + `fat_vfs.{c,h}` — FAT16 driver + VFS adapter. Backs
    the `/sd` mount when `block_ata` finds a disk. Sector cache;
    offset-native reads + true append; long names not supported.
  - `aliasfs.{c,h}` — bind-mount style. `bootstrap_fs` aliases
    `/bin → /sd/bin`, `/home → /sd/home`, `/lib → /sd/lib`,
    `/usr → /sd/usr` when a disk is present.
  - `sysfs.{c,h}` — synthetic read-only `/sys` (task table, ipc
    pending count, mem stats, …).
  - `devfs.{c,h}` — synthetic `/dev` (`fb0`, `input0`, `mouse0`,
    `tty`, `ttyS0`, `ptmx`, `pts/N`, …).
  - `binfs.{c,h}` — synthetic `/bin` fallback over the in-kernel
    builtin registry (diskless boot only).
  - `vfs.{c,h}` — backend contract (`vfs_ops_t`) + longest-prefix
    dispatch.
  - `bootstrap.{c,h}` — boot-time mount + seed setup. Decides
    disk-backed vs ramfs-only layout. On first boot with a disk it
    seeds `/sd/bin/` with every embedded ELF.
- `src/proc/` — user-task lifecycle.
  - `builtin.{c,h}` — registry of /bin/* entries. Three flavors:
    kernel (C fn), user blob (asm bytes), user ELF
    (`elf_start..elf_end`).
  - `exec.{c,h}` — `proc_execve(path, args, envp)` dispatches by
    flavor; `task_create_user_elf` invokes `elf_load`. Also home of
    fork/exec/wait machinery (`proc_fork`, `proc_exit_current_user`,
    zombie reaping, parent notification).
  - `elf.{c,h}` — minimal ELF64 ET_EXEC loader. Walks PT_LOAD,
    allocates + maps pages with PTE_U (+ PTE_W when PF_W). User
    stack at `0x7FFFE000-0x7FFFF000`.
- `src/lib/` — freestanding C helpers used by the kernel.
  - `string.c` — `os_strlcpy / os_strlcat / os_strncmp / os_strstarts
    / os_strchr / os_strrchr / os_strlen / os_strcmp / os_streq /
    os_memcpy / os_memset`. Use these; do not roll your own copy loop.
  - `memory.c` — alignment / page math helpers.
  - `printf.c` — kernel `printf` / `snprintf` (panic + serial logging).
- `src/include/` — public, leaf headers. **`osnos_*_abi.h` files are
  the kernel↔userland boundary** — changing one requires rebuilding
  kernel + libc + every ELF.
  - `osnos_ipc_abi.h` — `ipc_msg_t`, `ipc_type_t`, `SERVER_*`,
    `IPC_DATA_SIZE=1024`, `IPC_QUEUE_SIZE=64`.
  - `osnos_status.h` — error enum. **Numeric values match Linux
    x86_64 errno exactly** (`EPERM=1`, `ENOENT=2`, `EIO=5`,
    `EEXIST=17`, …). See ABI invariant below.
  - `osnos_keys.h` — Linux `input-event-codes.h` subset.
  - `osnos_elf.h` — ELF64 layout subset for the loader.
  - `osnos_stat.h` — `osnos_stat_t` layout (mirrors Linux `struct stat`).
  - `osnos_dirent.h` — getdents64 record layout.
  - `osnos_fcntl.h` — open flags, fcntl cmds, file modes.
  - `osnos_taskinfo.h` — `SYS_TASKINFO` snapshot record.
  - `osnos_limits.h` — `OSNOS_PATH_MAX=128`, `OSNOS_NAME_MAX=64`,
    `OSNOS_INPUT_MAX=128`. Has `_Static_assert` checking that two
    paths plus slack fit inside `IPC_DATA_SIZE`.
  - `osnos_path.h`, `font.h`, `theme.h`.

### Userland: libc, ELFs, vendor

- `lib/libc/` — user-side libc (libosnos_c.a + crt0.o). Compiled
  with `USER_CFLAGS` (no `-mcmodel=kernel`). Headers in
  `lib/libc/include/`: `stdio stdlib string unistd fcntl errno
  dirent signal time math setjmp termios pthread locale libgen
  inttypes ctype alloca assert endian float limits` plus
  `sys/{ioctl mman mouse reboot select socket stat time types wait}`,
  `arpa/inet.h`, `netinet/in.h`, `osnos_ipc.h`. Internals:
  `crt0.S` (`_start` → argc/argv/envp → `main` → `_exit`),
  `unistd.c` (Linux errno convention `-1 + errno`), `signal.c` +
  `sigtramp.S` (sigframe + `__sigtramp`), `setjmp.S`, `pthread.c`,
  `termios.c`, full `stdio.c` (4KB BUFSIZ; `printf` supports
  `%d %u %x %X %o %c %s %p %%` + flags + width + `l/ll/z`),
  `stdlib.c` (`malloc` on `sbrk` first-fit free list), `resolver.c`
  (DNS), `inet.c`, `time.c`, `mman.c`, `wait.c`, `dirent.c`,
  `reboot.c`, `pty.c`, `math.c`, `libgen.c`, `locale.c`, `netdb.c`,
  `errno.c`.
- `elfs/` — see layout above. Pattern: drop
  `elfs/<category>/<name>.c`, append to `USER_ELF_LIBC_SRCS` (libc-
  linked, provides `main`) or `USER_ELF_SRCS` (bare, provides own
  `_start`), and the build picks it up. Basenames must stay unique
  across categories. `objcopy` strips directory components when
  wrapping the ELF into `.o`, so the symbol is
  `_binary_<basename>_elf_start/end`. Only a tiny ROM subset
  (`consrv`, `kbdsrv`, `shellsrv`, `banner`) actually travels inside
  the kernel image; everything else is copied into `::/bin` on
  `sd.img` by the build and exec'd from `/bin` via the
  `/bin → /sd/bin` aliasfs.
- `vendor/tinycc`, `vendor/lua`, `vendor/jq` — third-party sources
  built as ring-3 ELFs against osnos libc. TCC's sysroot
  (`crt1.o crti.o crtn.o libc.a libtcc1.a` + builtin headers) is
  staged onto `sd.img` at `/lib/` and `/usr/include/`, so
  `tcc hello.c -o hello && ./hello` works inside osnos.

### Key invariants

**Linux ABI compat.** Any numeric value visible to userland (errno,
syscall numbers, key codes, ELF constants, ioctls/signals, sockaddr
layouts, getdents records) must match Linux x86_64. Goal: run
unmodified Linux ELF binaries against osnos libc. Do not invent
numbers in the Linux-occupied range; if osnos needs a non-Linux code,
reserve above 200 (syscalls) / 250 (used today: `SYS_ISATTY=250`,
`SYS_IPC_*=260..263`, `SYS_TTY_INPUT=264`, `SYS_TASKINFO=265`,
`SYS_SPAWN=266`, `SYS_SET_FG=267`, `SYS_RESUME=268`).

**ABI frontier in `src/include/osnos_*_abi.h`.** Any header named
`*_abi.h` is the kernel↔userland wire contract. Changing one means
recompiling kernel + libc + every ELF, so the change should be
backed by a note in `STATUS.md`.

**SYSCALL ABI.** Both `int 0x80` and `syscall` reach the same
`syscall_dispatch` over a shared `syscall_frame_t`. Register contract:
`rax = syscall #`, `rdi/rsi/rdx/r10/r8/r9 = args` (R10, not RCX,
matching Linux). The SYSCALL stub also preserves user RCX/R11 on the
kernel stack so SYSRET can restore them — calls into kernel must NOT
modify those two before sysret restoration.

**IPC contract** (`osnos_ipc_abi.h`). Opcode numeric ranges:
`0x00–0x0F` system, `0x10–0x1F` console, `0x20–0x3F` fs/vfs,
`0x40–0x5F` process lifecycle (`IPC_PROC_EXITED/STOPPED/CONTINUED`).
Every response sets `arg0=status, arg1=size, data=text`. `ipc_send`
may fail with EAGAIN (queue full) or ESRCH (no such service) — never
ignore the return when correctness matters.

**Ramfs slot ownership** (`src/fs/ramfs.h`). Borrowed pointers from
`ramfs_find` survive deletes of *other* slots.

**Scheduling.** Ring-3 tasks are preempted by the 50 ms timer
quantum; ring-0 tasks are still **cooperative** — a kernel server
that loops without returning hangs the whole kernel. Use
`task_current()->wakeup_at_ms = timer_ms() + N; state = TASK_BLOCKED;`
pattern (see `server_respawn_tick` in `main.c`) for periodic kernel
tasks.

**Single shared IPC queue** of 64 slots. Outputs of N lines must be
packed into a single message (the legacy shell used `os_strlcat` to
build one buffer and emit once). Per-line sends overflow the queue
and get silently dropped as EAGAIN.

**No per-task FPU save yet.** Single-task FP (e.g. TCC compiling in
the foreground) is fine; mixing FP across multiple concurrent ring-3
tasks may corrupt state. Real FXSAVE lands when needed.

**QEMU machine type matters.** `block_ata` talks PIO to legacy 0x1F0;
`-M pc` attaches the IDE disk there. `-M q35` would attach via AHCI
and the driver wouldn't see the disk.

### Adding a new kernel-side feeder (cooperative ring-0)

1. Write `foo_server.{c,h}` exporting `foo_server_init()` and
   `foo_server_tick()`. Return after one iteration; if you need to
   pace it, set `task_current()->wakeup_at_ms + state = TASK_BLOCKED`.
2. In `kmain`: `task_create("foo", foo_server_tick)` and call
   `foo_server_init()` afterwards. Don't register against a
   `SERVER_*` ID unless you're replacing one of the ring-3 servers.

### Adding a new ring-3 server (the real flavor since FASE 10)

1. Drop `elfs/osn-server/<name>.c`. Use `sys_service_register(SERVER_FOO)`
   inside it to claim the service slot, then loop on `sys_ipc_recv`.
2. If introducing a new service: add `SERVER_FOO` in
   `src/include/osnos_ipc_abi.h` (numeric value is part of the ABI).
3. Add new `IPC_*` opcodes in the right numeric range in
   `osnos_ipc_abi.h`.
4. Add the source to `USER_ELF_LIBC_SRCS` and to `USER_ELF_ROM_SRCS`
   (so it's embedded in the kernel as recovery ROM).
5. In `kmain`: `proc_execve("/bin/<name>", "", 0)` and
   `service_register(SERVER_FOO, pid)`. Add it to
   `server_respawn_tick` so it gets respawned if it dies.

### Adding a new shell command

The shell is now `elfs/osn-server/shellsrv.c` (ring 3). Its command
table is in there — add an entry and the `cmd_foo(const char *args)`
handler.

### Adding a new builtin

Three flavors live in `src/proc/builtin.c`'s `builtins[]`:

- `KERN("name", fn, "desc")` — a C `int fn(const char *args)` running
  in ring 0 but using only the syscall API.
- `USER("name", start, end, "desc")` — a flat blob of x86_64 machine
  code (typically file-scope inline asm in `builtin.c`). Copied into a
  single user page at `USER_CODE_VIRT = 0x400000`.
- `USERELF("name", start, end, "desc")` — pointer to an embedded ELF64.
  The kernel parses it via `elf_load` and lays it out per program
  headers.

Adding a new ELF builtin:

1. Drop `elfs/<category>/<name>.c` (pick a category: `tools`, `net`,
   `shell`, `tests`). For bare ELFs also drop your own `.lds` next to
   it (template: `elfs/tests/user_hello.lds`); libc-linked ones share
   `elfs/libc.lds`.
2. Append the source to **`USER_ELF_SRCS`** (bare) or
   **`USER_ELF_LIBC_SRCS`** (libc-linked) in `GNUmakefile`. The
   build copies all built ELFs to `::/bin/<name>` on `sd.img`
   (extension stripped, so `/bin/cat` matches the exec path).
3. If you want it embedded in the kernel ROM (most ELFs don't —
   they live only on disk), add the `USERELF(...)` entry in
   `src/proc/builtin.c` with the matching
   `_binary_<basename>_elf_start/end` extern, and add the source to
   `USER_ELF_ROM_SRCS`.

`CREATE_BUILTINS.es.md` covers the kernel-mode flavor;
`CREATE_ELF.es.md` is the long-form tutorial for hand-written bare
ELFs. For libc-linked programs, the pattern is `int main(int, char**)`
+ `#include <stdio.h>` etc.

## Roadmap & status

- `STATUS.md` — running log of what works today, ordered newest-first
  by phase, with sections by subsystem. **The most up-to-date source
  of truth**; consult before proposing where new features slot in.
- `ARCH.md` — architecture diagram + IPC / syscall flow walkthroughs.
- `ROADMAP_APENDICE.md` — multi-phase plan appendix.
- `PLAN_FASE10.md` — detailed plan for FASE 10 (ring-3 servers).
- `CREATE_BUILTINS.es.md` / `CREATE_ELF.es.md` — tutorials for adding
  kernel builtins / hand-rolled ELFs.

These are written in Spanish.
