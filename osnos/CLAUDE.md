# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository layout (important)

This directory (`osnos/`) holds the actual kernel sources, but it sits **inside** a Limine C template at the parent directory `/home/osn/osnso/`. The parent's `GNUmakefile` is what builds the bootable ISO and runs QEMU; it invokes `$(MAKE) -C osnos` to compile the kernel ELF into `osnos/build/kernel`, then packages that ELF with Limine.

- `../GNUmakefile` — top-level: builds ISO/HDD, downloads Limine + edk2-ovmf, runs QEMU.
- `../limine.conf` — boot entry; loads `boot():/boot/kernel`.
- `../kernel/` — Limine-supplied freestanding helpers used by this kernel: `cc-runtime/`, `freestnd-c-hdrs/`, `limine-protocol/`, `linker-scripts/`, plus `get-deps` to fetch them. The `osnos/` build references these via `../kernel/...` paths.
- `./GNUmakefile` — kernel-only build (called by parent's `kernel:` target).
- `./src/` — kernel sources.
- `./build/` — kernel ELF output.

The `osnos/` directory itself is untracked in the parent git repo.

## Build & run

From the parent directory `/home/osn/osnso/`:

- `make run` — build kernel + ISO, boot in QEMU with OVMF (UEFI).
- `make run-bios` — same but boot with SeaBIOS (faster cold-start; this is what `build_and_run.sh` uses).
- `make all` — build ISO only (no QEMU). Equivalent: `make`.
- `make clean` — remove ISO/HDD and recursively clean kernel.
- `make distclean` — also wipe downloaded `limine-binary/` and `edk2-ovmf/`.
- `make ARCH=aarch64 run` (or `riscv64`, `loongarch64`) — cross-arch. **Note:** the per-target kernel `GNUmakefile` here is currently hard-coded to x86_64; other ARCHes will fail to compile until that file is generalized.

From this directory (`osnos/`):

- `make` — compile `build/kernel` only.
- `make clean` — wipe `build/`.

Toolchain: `clang` + `ld.lld`. The kernel `GNUmakefile` globs `src/**/*.c` plus `../kernel/cc-runtime/src/*.c` via `find`, so **new `.c` files under `src/` are picked up automatically**. `-Werror` is on for `src/` (not for cc-runtime). Cero warnings expected on a clean build.

## Architecture

OSnOS is a small microkernel-style hobby OS booted by Limine on a linear framebuffer. Everything runs in ring 0 today; "tasks" are cooperatively scheduled C functions that talk to each other through an IPC message queue. The longer-term plan in `ROADMAP.md` / `STATUS.md` is to push drivers and servers into userspace once ELF/ring3 land.

See `ARCH.md` for a full diagram + IPC flow walkthroughs.

Boot path (`src/kernel/main.c`, `kmain`):
1. Validate Limine base revision and framebuffer response; `framebuffer_init`.
2. Memory layer: `pmm_init → vmm_init → kheap_init`.
3. CPU tables: `gdt_init → tss_init → idt_init → uaccess_init → syscall_msr_init`.
   - `uaccess_init` registers the copy_*_user span in the extable so a
     fault inside the loop unwinds to EFAULT instead of panicking.
   - `syscall_msr_init` enables EFER.SCE and programs STAR/LSTAR/FMASK
     so ring-3 code can use the `syscall` instruction.
4. Microkernel state: `ipc_init → task_init → reaper_init → scheduler_init
   → syscall_init → ramfs_init → bootstrap_fs`.
5. `task_create` for each server (state READY). Register service IDs
   (`SERVER_FS=4`, `SERVER_KEYBOARD=1`, `SERVER_SHELL=2`, `SERVER_CONSOLE=3`).
   **Then** call each server's `_init()`. Order matters: shell_init sends
   the initial banner+prompt and needs the console service already
   registered, otherwise `ipc_send` returns `OSNOS_ESRCH` and the message
   is dropped.
6. `scheduler_loop()` — saves a longjmp resume point at the top of the
   `for(;;)` and never returns. Each tick first drains the reaper, then
   dispatches the next READY task.

### Subsystem map

- `src/micro/` — kernel core.
  - `task.{c,h}` — fixed table of 16 tasks. States: UNUSED/READY/RUNNING/BLOCKED/DEAD. Cooperative dispatch (task_run_next round-robins). Ring-3 fields: `pml4`, `kernel_stack_*`, `user_entry`, `user_stack_top` populated by `task_create_user{,_elf}`.
  - `scheduler.{c,h}` — cooperative scheduler. `scheduler_loop` is the long-jump host: it saves a resume point on entry, then iterates; `sched_resume_jump()` is called from any nested context (sys_exit, fault) to wind RSP back to that frame.
  - `ipc.{c,h}` — `ipc_msg_t` (1024-byte payload + arg0/arg1, typed via `ipc_type_t`). Single shared queue of 64. `ipc_send` returns `osnos_status_t` (OK / EAGAIN / ESRCH); callers MUST check.
  - `service.{c,h}` — name→pid registry. Servers find each other by `SERVER_*` constant.
  - `gdt.{c,h}` + `tss.{c,h}` — GDT layout (kcode 0x08, kdata 0x10, udata 0x18, ucode 0x20, TSS 0x28). User-data BEFORE user-code is required so SYSRET64 lands on the right selectors. `tss.rsp0` mirrored in `tss_kernel_rsp0` for the SYSCALL stub.
  - `idt.{c,h}` — 256-entry IDT. `fault_try_recover` runs before every panic: kernel-mode RIP in extable → frame->rip rewrite; user-mode CPL=3 with `t->pml4` → `proc_exit_current_user(139)`; otherwise panic.
  - `extable.{c,h}` — `{rip_start, rip_end, recovery_rip}` table. Today only the copy_*_user span is registered.
  - `uaccess.{c,h}` — `copy_from_user` / `copy_to_user` wrap `__uaccess_copy_bytes` (asm in the same file). A page fault inside that span is redirected to `__uaccess_copy_bytes_fault` which returns `OSNOS_EFAULT`.
  - `reaper.{c,h}` — queues kstacks freed at task death; `reaper_drain()` is called at the top of every `scheduler_tick`. Also reaps DEAD slots to UNUSED.
  - `syscall.{c,h}` — Linux x86_64 syscall numbers + `syscall_dispatch(frame)`. `int80.c` and `syscall_entry.c` share this dispatcher.
  - `int80.c` — IDT[0x80] entry stub (legacy compat ABI).
  - `syscall_msr.{c,h}` — programs EFER.SCE, STAR, LSTAR, FMASK at boot.
  - `syscall_entry.c` — `syscall` instruction entry stub. Mirrors the int80 stub but saves/restores user RSP (`syscall_user_rsp` global) and exposes RCX/R11 to SYSRET.
  - `pmm.{c,h}` / `vmm.{c,h}` / `kmalloc.{c,h}` — physical, virtual, heap layer.
- `src/drivers/` — `framebuffer` (handles `\b`, `\n`, `\r`, `\t` in draw_string), `keyboard` (PS/2; tracks shift, ctrl, extended `0xE0`; emits `keyboard_event_t { ascii, keycode }` — keycode uses Linux `input-event-codes.h` values for arrows).
- `src/servers/` — `*_init` + `*_tick` pattern, communicate over IPC.
  - `keyboard_server` — `keyboard_poll` → `IPC_KEY_EVENT` (ascii in `data[0]`, keycode in `arg0`).
  - `console_server` — drains `IPC_CONSOLE_WRITE` / `IPC_CONSOLE_CLEAR` to framebuffer.
  - `shell_server` — line editor with history (16 entries, dedup consecutivos), up/down arrow nav, Ctrl+C cancel input. Commands dispatched via a `commands[]` table (`CMD(name, handler, help)` / `ALIAS(name, handler)`; help auto-generated). FS senders unified in `shell_send_fs1(type, path)` / `shell_send_fs2(type, a, b)` — both return `osnos_status_t`.
  - `fs_server` — request/response wrapper around RAMFS. Every reply sets `response.arg0 = osnos_status_t`, `response.arg1 = size_or_count`, `response.data` = text payload.
- `src/fs/ramfs.{c,h}` — 32 slots × 128B path × 512B data. **Slot ownership invariant** (see `ramfs.h`): a slot's index is stable for the entry's lifetime; deletion marks `used=false` without compacting; `const ramfs_file_t *` returned by `ramfs_find` stays valid until that specific entry is deleted.
- `src/fs/bootstrap.{c,h}` — creates `/home`, `/sys`, `/dev`, `/bin` and seed files at boot. Lives outside ramfs because in FASE 2 it becomes a series of `vfs_mount()` calls.
- `src/fs/vfs.h` — backend contract: `vfs_ops_t` per mount; longest-prefix dispatch. Backends today: `ramfs_vfs`, `sysfs`, `devfs`, `binfs`.
- `src/proc/` — user-task lifecycle layer.
  - `builtin.{c,h}` — registry of /bin/* entries. Three flavors: kernel (C fn), user blob (asm bytes), user ELF (`elf_start..elf_end`).
  - `exec.{c,h}` — `proc_exec(path, args)` dispatches by flavor. `task_create_user_elf` invokes `elf_load`, then sets up the task. `proc_exit_current_user(code)` is the single teardown path for ring-3 tasks (sys_exit + fault handlers).
  - `elf.{c,h}` — minimal ELF64 ET_EXEC loader. Validates magic/class/machine/type. Walks PT_LOAD, allocates + maps pages with PTE_U (+ PTE_W when PF_W). Allocates a 1-page user stack at `0x7FFFE000-0x7FFFF000`.
- `src/lib/` — freestanding C helpers. `string.c` exposes `os_strlcpy` / `os_strlcat` / `os_strncmp` / `os_strstarts` / `os_strchr` / `os_strrchr` / `os_strlen` / `os_strcmp` / `os_streq` / `os_memcpy` / `os_memset`. Use these; do not roll your own copy loop.
- `src/include/` — public, leaf headers:
  - `osnos_limits.h` — `OSNOS_PATH_MAX=128`, `OSNOS_NAME_MAX=64`, `OSNOS_INPUT_MAX=128`. Has `_Static_assert` checking that two paths plus slack fit inside `IPC_DATA_SIZE`.
  - `osnos_status.h` — error enum. **Numeric values match Linux x86_64 errno exactly** (`EPERM=1`, `ENOENT=2`, `EIO=5`, `EEXIST=17`, `ENOTDIR=20`, `EISDIR=21`, `EINVAL=22`, `ENOSPC=28`, `ENOTEMPTY=39`, etc.). See ABI invariant below.
  - `osnos_keys.h` — Linux `input-event-codes.h` subset: `KEY_UP=103`, `KEY_DOWN=108`, `KEY_LEFT=105`, `KEY_RIGHT=106`.
  - `osnos_elf.h` — ELF64 layout subset (Elf64_Ehdr / Elf64_Phdr + PT_*/PF_*/ET_*/EM_X86_64 constants), used by the loader.
  - `osnos_path.h` — `osnos_path_t { buf, len }` sketch, used by future VFS.
  - `font.h`, `theme.h`.
- `tests/` — user-mode programs that the build compiles into ring-3 ELFs and embeds in the kernel:
  - `tests/user_hello.c` + `tests/user_hello.lds` → `/bin/hello_elf` (bare, no libc).
  - `tests/hello_libc.c` + `tests/hello_libc.lds` → `/bin/hello_libc` (linked with the libc).
  - Pattern: any `tests/<name>.c` paired with `tests/<name>.lds` will be picked up by the make rule `$(BUILD)/tests/%.elf` and embedded via objcopy as `_binary_<name>_elf_start/end`. Add it to `USER_ELF_SRCS` (bare) or `USER_ELF_LIBC_SRCS` (libc-linked) in `GNUmakefile`.
- `lib/libc/` — osnos user-side libc (FASE 7). Compiled separately with `USER_CFLAGS` (ring-3 freestanding, NOT `-mcmodel=kernel`). Bundled into `build/lib/libc/libosnos_c.a` via `ar`, plus a standalone `crt0.S.o`.
  - `include/`: `stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `fcntl.h`, `errno.h`, `sys/{types,stat}.h`. These are what user code `#include`s — *not* visible to the kernel build.
  - `syscall.h`: internal `osnos_syscall0..4` inline helpers. Linux x86_64 numbers; matches `src/micro/syscall.h`.
  - `crt0.S`: `_start` arranges argc=0 / argv=NULL / envp=NULL, calls `main`, hands return to `_exit`.
  - `unistd.c`: wraps every syscall with the Linux errno convention (`-1 + errno` on failure).
  - `string.c` / `stdlib.c` / `stdio.c` / `errno.c`: standard library functions. `malloc` sits on top of `sbrk` (first-fit free list; no split/merge). `printf` is a mini variadic engine supporting `%d %u %x %X %o %c %s %p %%` plus flags `-`, `0`, `+`, ` `, width, length `l/ll/z`.

### Key invariants

**Linux ABI compat.** Any numeric value that will be visible to userland eventually (errno, syscall numbers, key codes, ELF constants, ioctls/signals later) must match Linux x86_64. Goal: run unmodified Linux ELF binaries once libc lands (FASE 7). Do not invent numbers in the Linux-occupied range; if osnos needs a non-Linux code, reserve above 200.

**SYSCALL ABI.** Both `int 0x80` and `syscall` reach the same `syscall_dispatch` over a shared `syscall_frame_t`. Register contract: `rax = syscall #`, `rdi/rsi/rdx/r10/r8/r9 = args` (note R10 not RCX, matching Linux). The SYSCALL stub also preserves user RCX/R11 on the kernel stack so SYSRET can restore them. Calls into kernel must NOT modify those two before sysret restoration.

**IPC contract** (`src/micro/ipc.h`). Opcodes in fixed numeric ranges (`0x00–0x0F` system, `0x10–0x1F` console, `0x20–0x3F` fs/vfs, `0x40+` reserved). Every response sets `arg0=status, arg1=size, data=text`. `ipc_send` may fail with EAGAIN or ESRCH — never ignore the return value when correctness matters (shell propagates via `report_fs_failure`).

**Ramfs slot ownership** (`src/fs/ramfs.h`). Borrowed pointers from `ramfs_find` survive deletes of *other* slots. Critical for future file descriptor layer.

**Cooperative scheduling**. A task that loops indefinitely hangs the entire kernel. Ctrl+C (cancels typed input) does NOT rescue. Long operations must not exist in this codebase. Real preemption is FASE 9 of ROADMAP.

**Single shared IPC queue** of 64 slots. Outputs of N lines must be packed into a single message (see how `cmd_help` and `cmd_history` build a buffer with `os_strlcat` and emit once). Per-line sends overflow the queue and get silently dropped as EAGAIN.

### Adding a new server

1. Write `foo_server.{c,h}` exporting `foo_server_init()` and `foo_server_tick()`.
2. Add `SERVER_FOO` constant in `src/micro/service.h`.
3. Add any new `IPC_*` opcodes in `src/micro/ipc.h` (in the right numeric range — keep ranges tight).
4. In `kmain`: `int foo_pid = task_create("foo", foo_server_tick);` THEN `service_register(SERVER_FOO, foo_pid);` THEN `foo_server_init();`. Order matters (see Boot path step 4).

### Adding a new shell command

Add an entry to `commands[]` in `src/servers/shell_server.c` and write a `cmd_foo(const char *args)` handler. The help line is auto-generated.

### Adding a new builtin

Three flavors live in `src/proc/builtin.c`'s `builtins[]`:

- `KERN("name", fn, "desc")` — a C `int fn(const char *args)` running in ring 0 but using only the syscall API.
- `USER("name", start, end, "desc")` — a flat blob of x86_64 machine code (typically file-scope inline asm in `builtin.c`). The blob is copied into a single user page at `USER_CODE_VIRT = 0x400000`.
- `USERELF("name", start, end, "desc")` — pointer to an embedded ELF64. The kernel parses it via `elf_load` and lays it out per its program headers.

Adding a new ELF builtin:

1. Drop `tests/<name>.c` and `tests/<name>.lds` (use `tests/user_hello.lds` as a template for bare ELFs, or `tests/hello_libc.lds` for libc-linked ones).
2. Append the source to **`USER_ELF_SRCS`** (bare, has its own `_start`) or **`USER_ELF_LIBC_SRCS`** (libc-linked, provides `int main(...)`) in `GNUmakefile`.
3. Declare the `_binary_<sanitized>_elf_start/end` extern in `src/proc/builtin.c` and add the `USERELF(...)` registry entry.

`CREATE_BUILTINS.es.md` covers the kernel-mode flavor; `CREATE_ELF.es.md` is the long-form tutorial for hand-written bare ELFs. For libc-linked programs, the pattern is `int main(int, char**)` + `#include <stdio.h>` etc.

## Roadmap & status

- `STATUS.md` — what works today, with sections by subsystem.
- `ROADMAP.md` — multi-phase plan (VFS → ELF/ring3 → preemptive scheduler → real disk → graphical).
- `ARCH.md` — architecture diagram and IPC flow walkthroughs.

These are written in Spanish. Consult them before proposing where new features slot in.
