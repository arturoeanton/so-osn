# STATUS — osnos

Estado actual del proyecto y bitácora de fases. Este documento es la
**fuente de verdad sobre qué funciona hoy**. Para arquitectura por
capas ver [`ARCH.md`](ARCH.md); para overview pitch ver el
[`README.md`](../README.md) raíz; para tutoriales de cómo extender
ver [`CREATE_BUILTINS.es.md`](CREATE_BUILTINS.es.md) y
[`CREATE_ELF.es.md`](CREATE_ELF.es.md).

Convenciones:
- ✅ funciona / cerrado / verificado
- ⚠️ funcional pero con limitaciones conocidas
- ❌ pendiente / no implementado
- **FASE X — CERRADA** = fase del roadmap terminada
- **FASE X — PENDIENTE** = fase futura, plan documentado

---

## Resumen ejecutivo

OSnOS es un microkernel hobby x86_64 escrito desde cero. Bootea con
Limine, corre en QEMU, y trae:

- **Kernel ring-0** con ELF loader, paging propio (4 niveles), VFS
  multi-backend (ramfs + FAT16 + devfs + sysfs + binfs + aliasfs),
  scheduler preemptivo (50 ms quantum en CPL=3), IPC queue de 64
  slots, line discipline POSIX, ~70 syscalls compatibles con Linux
  x86_64 (read/write/open/fork/execve/wait/sigaction/pipe/mmap/
  socket/select/poll/...).
- **Servidores ring-3**: console (`consrv`), keyboard (`kbdsrv`),
  mouse feeder, shell (`busybox sh` desde FASE 13.1; antes `shellsrv`
  custom).
- **Dos libcs**: una mini hecha en casa (`lib/libc/`) para programas
  chicos, y **musl 1.2.5 vendoreado** (FASE 13.0) para programas
  serios. Coexisten — cada ELF opta-in al linkear.
- **BusyBox 1.36.1** linkeado contra musl (FASE 13.1): ~60 applets
  (`vi awk sed find diff stat dd md5sum sha256sum base64 hexdump
  bc dc more tac fold xargs find ...`) accesibles vía aliases en
  `/home/.ashrc`. Default shell es `busybox sh` con history persistente
  a `/home/.ash_history`.
- **Cuatro lenguajes self-host**: C (TCC 0.9.27, FASE 11.0), Lua
  5.4.7 (FASE 11.2), jq 1.7.1 (FASE 11.3), **SQL via SQLite 3.45.2**
  (FASE 13.3) — todos como ELFs en `/bin/`, todos funcionales con
  REPL interactivo (echo + backspace visible) o redirect.
- **Ox mini-X window system** (FASE 12.0): server ring-3 `/bin/oxsrv`
  con compositor + 5 apps GUI (oxnotepad, oxcalc, oxterm, oxfiles,
  oxsettings), wallpapers PPM, root menu Openbox-style. Opt-in
  (usuario lanza `oxsrv` desde el shell).
- **Console serial dual** (UART 16550 COM1): boot headless con
  `./build_and_run.sh headless` para CI; framebuffer + serial siempre
  en paralelo, panic backtrace persiste en serial.log.
- **18/18 tests automatizados** via `/bin/alltest` (kernel + libc + ports).
- **Bug fixes recientes** que destrabaron el camino hacia REPLs
  interactivos usables: FS_BASE save/restore en task switch +
  reset en execve (bug #9), echo y backspace via `framebuffer_write_bytes`
  consistente con el path de apps (bugs #10 + #11), restart_syscall
  pattern en sys_read/poll (bug #2 cuando portamos BusyBox).

**Pitch en una frase**: hobby OS x86_64 que corre BusyBox + SQLite +
Lua + jq + TCC, todos compilados nativos contra musl, con un mini-X
window system propio, todo desde un microkernel escrito desde cero.

---

## Lo que funciona hoy

### Boot + arquitectura base
| Subsistema | Estado | Notas |
|---|---|---|
| Limine boot + framebuffer linear | ✅ | BIOS legacy (`-M pc`) |
| GDT + IDT + TSS (ring 0/3) | ✅ | SYSCALL + INT80 dual entry |
| PMM (bitmap) + VMM 4-level paging + kheap | ✅ | kheap cap 32 MiB |
| PIT @ 100 Hz + LAPIC | ✅ | Quantum scheduling 50 ms en CPL=3 |
| `copy_from_user`/`copy_to_user` + extable | ✅ | Faulting user ptr → EFAULT, no panic |
| FPU/SSE setup + FXSAVE/FXRSTOR per-task | ✅ | Concurrent FP entre tasks seguro |
| Serial UART 16550 dual-console | ✅ | Headless boot, panic logs persisten |

### Microkernel
| Subsistema | Estado | Notas |
|---|---|---|
| Task table (16 slots) + scheduler preemptivo | ✅ | longjmp resume pattern |
| **`block_restart_syscall` pattern** en sys_read / sys_poll | ✅ | Bloquea via iret rewind, no longjmp con rax=0 (FASE 13.1) |
| **`fs_base` save/restore en task switch + reset en execve** | ✅ | Per-task TLS pointer (FASE 13.3) |
| Per-task fd table (16 fds) + OFD pool (128) | ✅ | Shared offsets POSIX |
| pipe / dup / dup2 / fcntl | ✅ | FD_CLOEXEC per-fd |
| `mmap`/`munmap` anónimo + brk/sbrk | ✅ | mmap_regions tracking |
| Signal delivery (sigaction, sigreturn, EINTR) | ✅ | Sigframe en user stack |
| SIGCHLD automático + waitpid + WIFEXITED/SIGNALED | ✅ | TASK_ZOMBIE state |
| Process groups + sessions + Ctrl+C fan-out a pgid | ✅ | WUNTRACED/WCONTINUED |
| PTY pairs (`/dev/ptmx` + `/dev/pts/N`, pool 8) | ✅ | Canon/raw, ECHO, TIOCS* ioctls |
| **POSIX line discipline TTY** + echo + backspace consistentes | ✅ | Echo via `framebuffer_write_bytes` mismo path que apps (FASE 13.3 fix) |
| IPC queue 64 × 1024 B + service registry | ✅ | Routing por SID o pid directo |
| `init-respawn` watchdog para servers | ✅ | consrv/kbdsrv/shellsrv auto-restart |

### Sistema de archivos
| Subsistema | Estado | Notas |
|---|---|---|
| VFS con backend longest-prefix dispatch | ✅ | 16 mount slots (era 8 pre-FASE-13.1) |
| ramfs (`/`) 32 slots × 128 B path + 512 B data | ✅ | |
| sysfs (`/sys`) read-only synthetic | ✅ | task table, ipc count, mem stats |
| devfs (`/dev`) con fb0/input0/mouse0/tty/ttyS0/ptmx/pts | ✅ | ioctls FBIOGET/FBIO_BLIT en fb0 |
| binfs (`/bin`) fallback diskless | ✅ | Sobre kernel builtin registry |
| **FAT16** read/write/append + dir-chain extension + sector cache | ✅ | 32 MiB sd.img, persistente |
| **aliasfs** bind-mount style (`/bin → /sd/bin`, `/home → /sd/home`, `/etc → /sd/etc`, `/lib → /sd/lib`, `/usr → /sd/usr`) | ✅ | Read/write transparente a FAT |
| Offset-native VFS reads (`vfs_read_at`) | ✅ | O(count) en vez de O(file_size) |
| `sys_stat` byte-a-byte path copy | ✅ | No faultea con paths cortos (FASE 13.1 fix) |
| Syscalls: open/openat/close/read/write/lseek/fstat/stat/lstat/newfstatat/getdents64/access/mkdir/rmdir/unlink/rename/chdir/getcwd/dup/dup2/fcntl/fsync/ftruncate/ioctl/select/poll/pipe | ✅ | Linux x86_64 compatible |

### Networking
| Subsistema | Estado | Notas |
|---|---|---|
| RTL8139 driver + ARP + IPv4 + ICMP + UDP + TCP | ✅ | PCI bus scan |
| Sockets POSIX (socket/bind/listen/accept/connect/send/recv/select) | ✅ | |
| DNS resolver + getaddrinfo (vía slirp 10.0.2.3) | ✅ | |
| `/bin/httpd` sirviendo FAT16 sobre HTTP | ✅ | hostfwd 8080 |
| Demos (`/bin/tcpclient`, `udptest`, `echotcp`, `selectserver`) | ✅ | |

### Userland — shell + comandos
| Componente | Estado | Notas |
|---|---|---|
| **`/bin/busybox` (1.36.1, musl-linked)** | ✅ | Default shell + ~60 applets (FASE 13.1) |
| **History persistente `/home/.ash_history`** | ✅ | `FEATURE_EDITING_SAVEHISTORY=y` + `SAVE_ON_EXIT=y`, cross-reboot |
| **`/etc/profile` + `/home/.ashrc`** (estilo .bashrc) | ✅ | Banner + PS1 + aliases para applets |
| **`vi awk sed find diff patch hexdump more dd df du stat readlink realpath base64 md5sum sha1sum sha256sum cksum bc dc xargs tac factor fold expand rev strings timeout`** | ✅ | Via aliases en `.ashrc` |
| `/bin/shellsrv` (legacy custom shell) | ✅ | Fallback diskless si `/bin/busybox` falta |
| Coreutils nativos (~60 ELFs) — `ls cat cp mv rm mkdir touch echo wc head tail grep sort uniq cut tr seq yes tee env pwd which printf date uname basename dirname clear tree banner ...` | ✅ | Mini-libc-linked |
| `/bin/less` con `/pattern` highlight + `n`/`N` | ✅ | Pipe-mode (`cat foo \| less`) drena stdin + dup2 /dev/tty |
| `/bin/ovi` editor modal vim-style | ✅ | hjkl, i/a/o, x/dd, :w/:q |
| `/bin/readelf -a/-l/-S/-h` | ✅ | ELF header + phdr + shdr inspector |
| `/bin/poweroff` + `/bin/reboot` | ✅ | ACPI S5 + 8042 reset |
| `tail -f` (`/bin/tail`) | ✅ | Poll loop 200 ms con EAGAIN/EINTR |
| `/bin/term` + `/bin/minishell` | ✅ | Sub-shell interactivo en PTY (showcase POSIX) |

### Self-hosting (4 lenguajes)
| Componente | Estado | Notas |
|---|---|---|
| **`/bin/tcc` — TinyCC 0.9.27** | ✅ | C compiler; produce ELFs estáticos runnable contra `/lib/libc.a` (FASE 11.0) |
| **`/bin/lua` — Lua 5.4.7** | ✅ | REPL + scripts (FASE 11.2) |
| **`/bin/jq` — jq 1.7.1** | ✅ | Filter/transformer JSON (FASE 11.3) |
| **`/bin/sqlite3` — SQLite 3.45.2** | ✅ | SQL engine completo + `/home/demo.db` (15 books + view + indices) preseeded (FASE 13.3) |

### Window system (Ox)
| Componente | Estado | Notas |
|---|---|---|
| **`/bin/oxsrv`** — server ring-3 ~700 LOC | ✅ | Compositor + cursor + z-order + root menu Openbox-style (FASE 12.0) |
| **Ox client API** (`lib/libc/ox.{c,h}`) estilo mini-Xlib | ✅ | window_create/draw_rect/draw_text/draw_image/present/poll_event |
| `/bin/oxnotepad` text editor con argv path | ✅ | |
| `/bin/oxcalc` calculadora 4-func | ✅ | |
| `/bin/oxterm` PTY + uxsh sub-shell + parser ANSI completo (SGR truecolor, cursor pos, erase) | ✅ | (FASE 12.1) |
| `/bin/oxfiles` file browser click-to-open | ✅ | (FASE 12.1) |
| `/bin/oxsettings` wallpaper picker | ✅ | Edita `/home/.oxrc`, IPC_OX_RELOAD_SETTINGS |
| Wallpapers PPM P6 (`/home/wallpapers/samurai.ppm`, `girl.ppm`) | ✅ | Generados al build con `tools/gen_wallpapers.sh` (PNG si está, fallback procedural) |
| Ioctls de FB: `FBIOGET_VSCREENINFO`, `FBIO_BLIT` (Linux-compat) | ✅ | |

### Limitaciones conocidas
- ❌ SMP (multi-core)
- ❌ Copy-on-write para fork (hoy full page copy)
- ❌ File-backed mmap (solo anonymous)
- ❌ Real X11/tinyX (Ox es protocolo IPC propio; pero los ioctls + `<linux/fb.h>` están listos para un futuro port)
- ⚠️ TTY global compartido entre tasks (no per-PTY real) — mitigado con anti-clobber de tcsetattr y echo via path compartido, pero un fork+exec a un programa que necesite raw mode no convive bien con un parent que también raw
- ⚠️ Single FP state HW para múltiples tasks — FXSAVE/FXRSTOR per-task implementado pero no extensivamente testeado
- ⚠️ sqlite3 con SQL en argv tiene argv passing issues; workaround: stdin redirect (`echo "SQL" \| sqlite3` o `sqlite3 db < q.sql`)
- ⚠️ sqlite3 exit limpio puede page-faultear en musl atexit cleanup (no afecta a ash — gracias al fix de FS_BASE)

---

## Bitácora de fases (orden cronológico inverso)

**Más recientes primero**. Cada entrada describe trabajo, decisiones,
y bugs notables encontrados.

| Fase | Subsistema | LOC aprox |
|------|-----------|-----------|
| **FASE 13.3 — SQLite 3.45.2 port (cuarto lenguaje self-host: SQL) + bug fixes profundos** | (1) **`vendor/sqlite/`** amalgamation (sqlite3.c ~250K LOC + shell.c + sqlite3.h). Linkeado contra musl. Output `/bin/sqlite3` ~5 MB ELF estático. (2) **4 syscalls nuevos**: `SYS_FSYNC=74`/`FDATASYNC=75` (stubs, FAT16 ya es sync), `SYS_FTRUNCATE=77` (real, vía vfs_read+pad+rewrite), `SYS_GETTIMEOFDAY=96` (alias clock_gettime con conversion), `SYS_GETRANDOM=318` (PRNG xorshift seeded por timer). (3) **`sys_fcntl` extendido**: F_SETLK/F_GETLK/F_SETLKW/F_OFD_* retornan 0 (single-process, advisory locks no aplican); F_DUPFD_CLOEXEC mappeado. (4) **Bumps**: `EXEC_VFS_BLOB_MAX` 2→16 MiB; `KHEAP_MAX_BYTES` 4→32 MiB (sqlite ELF 5 MB no entraba). (5) **SQLite CFLAGS**: THREADSAFE=0, OMIT_LOAD_EXTENSION, OMIT_WAL, DEFAULT_LOCKING_MODE=1 (exclusive), DEFAULT_TEMP_STORE=2 (memory), NO_SYNC=1, DEFAULT_MMAP_SIZE=0. (6) **`res/demo.sql` + `res/demo.db`** shipped a `/home/demo.db` (15 books + 4 users + 6 checkouts + view + indices). (7) **🔥 Bug crítico #9 — FS_BASE save/restore + reset en execve** (`task.{c,h}` + `exec.c` + `syscall.c`): `arch_prctl(ARCH_SET_FS)` escribe MSR_FS_BASE globalmente en el CPU; sin save/restore per-task ash heredaba el FS_BASE de sqlite3 y page-faulteaba en `__errno_location` post-wait. Tres patches: (a) `uint64_t fs_base` en task_t + rdmsr/wrmsr en task_run_next; (b) reset a 0 en proc_execve (ambos paths — task_create_user_elf + in-place exec); (c) copy del parent al child en sys_fork. **Sin estos 3, NINGÚN programa musl-linked spawneado desde ash sobrevivía a su parent**. (8) **🔥 Bug #10 + #11 — echo y backspace REPL**: `tty_echo_char` usaba `framebuffer_draw_string` directo (sin serial mirror; cursor distinto al path de apps via consrv). Fix: usar `framebuffer_write_bytes` (mismo cursor, mismo serial mirror). `tty_echo_erase` igual: secuencia `"\b \b"` via write_bytes (cursor atrás, sobreescribe con espacio, cursor atrás). **Sin estos fixes, REPLs de sqlite/lua tenían stdin funcional pero CERO eco visual** — usuario tipeaba a ciegas. (9) **`page_fault` log mejorado**: agregado task name + pid + cr2 + rip (`*** task 'busybox' pid=6 killed: Page fault cr2=0x... rip=0x...`) — critical para diagnosticar #9 (descubrir que ash, no sqlite3, era quien faulteaba). (10) **Verificación end-to-end**: `sqlite3 :memory: < q.sql` con `SELECT 99` → `99`; `sqlite3 /home/demo.db` REPL interactivo con `.tables`, `SELECT title FROM books`, `.quit` todos con echo + backspace visibles; ash sobrevive a múltiples runs de sqlite3 sin respawnear. **Cuarto lenguaje self-host**: C + Lua + jq + SQL. | 900 |
| **FASE 13.2 — BusyBox rebuild con history file + ~30 applets nuevos** | (1) **Bug crítico del wrapper `osnos-cc-wrapper.sh`**: compile mode no pasaba `-target x86_64-unknown-none-elf` → clang on macOS producía Mach-O ARM64 nativo, no ELF x86_64. ld.lld rechazaba con "unknown file type". Fix: agregado `-target` + `-U__APPLE__ -D__linux__` (evita rama BSD de `include/platform.h` que requiere `<machine/endian.h>` macOS-only) + musl includes injectados via `-isystem` (busybox no las pasa por default) + filtrado de flags clang-only del link path (`-finline-limit`, `-falign-*`, `-Wp,*`) + branch separada para preprocess (`-E -xc -MM -dM`). (2) **`.config` actualizada**: `FEATURE_EDITING=y` + `EDITING_HISTORY=500` + `EDITING_SAVEHISTORY=y` + `EDITING_SAVE_ON_EXIT=y` + `EDITING_FANCY_PROMPT=y` (PS1 `\w` expansion) + ~30 applets nuevos. (3) **STANDALONE_SHELL deshabilitado** (bug de dispatch multi-arg). En vez de eso `/home/.ashrc` define `alias vi='busybox vi'`, etc — FAT16 no soporta symlinks así que el approach Linux-style "/bin/vi → /bin/busybox" no aplica. (4) **`history` builtin** + persistencia cross-reboot. (5) **Verificado**: `sed s/x/y/`, `awk -F: ...`, `find -type f`, `stat /home/README.TXT`, `base64`, `md5sum`, `bc -e "5*5"` funcionales. **FASE 12 TUI del roadmap original superseded** — BusyBox cubre vi/less/sed/awk/find/etc. | 800 |
| **FASE 13.1 — BusyBox ash como init shell + login mode + .bashrc-style /home/.ashrc** | (1) **`vendor/busybox/`** — BusyBox 1.36.1 vendored, linkeado contra musl via `osnos-cc-wrapper.sh`. (2) **🔥 Bug crítico #1 — restart_syscall pattern**: `sys_read` + `sys_poll` loopeaban con `sys_nanosleep()`; pero nanosleep hace `sched_resume_jump()` (longjmp al scheduler) y deja al task con `saved_rax=0` apuntando al RIP user-space POST-syscall. ash llamaba read(0), kernel longjumpeaba, ash recibía read=0 → EOF → exit(0) → watchdog respawn → loop infinito. Fix: `block_restart_syscall(wakeup_ms, syscall_nr)` stampa iret frame con `rip -= 2` + `saved_rax = syscall_nr`. CPU re-ejecuta el syscall al despertar — patrón POSIX restart_syscall. (3) **🔥 Bug crítico #2 — colisión de syscall numbers**: osnos vivían en 260-268; chocaban con Linux #262=newfstatat (que musl `stat()` invoca). Movidos a 510-518. Nuevos mappings: `SYS_LSTAT=6`, `SYS_OPENAT=257`, `SYS_NEWFSTATAT=262`, `SYS_EXIT_GROUP=231`. (4) **🔥 Bug #3 — `sys_stat` faulteaba con paths cortos**: `copy_from_user(kpath, path, OSNOS_PATH_MAX)` pedía 128 bytes; fix: copy byte-a-byte hasta NUL. (5) **🔥 Bug #4 — `VFS_MAX_MOUNTS=8` insuficiente**: con 9 mounts `/home` no entraba. Bumpado a 16. (6) **Login shell + split estilo bash**: `proc_execve("/bin/busybox", "sh -l", envp)`. `/etc/profile` sourced ONCE → exports + `ENV=/home/.ashrc`. `/home/.ashrc` sourced cada shell interactiva (mirror exacto de ~/.bashrc) → PS1 verde `osnos:\w# ` + aliases + banner. (7) **Verificado**: ash sobrevive como init shell, `echo $((100*7))=700`, `for i in a b c`, `ls /etc` via aliasfs, pipes, redir, glob, todo POSIX. | 800 |
| **FASE 13.0 — musl libc port (segunda libc opt-in)** | (1) **`vendor/musl/`** — musl 1.2.5 (~140K LOC). `./configure --target=x86_64 --disable-shared` + `make -j4` compila al primer intento — zero patches al árbol upstream. Output `vendor/musl/build-osnos/lib/{libc.a, crt1.o, crti.o, crtn.o}`. (2) **Kernel gaps cerrados**: `SYS_WRITEV=20` (musl stdio via writev), `SYS_ARCH_PRCTL=158` (ARCH_SET_FS → wrmsr MSR_FS_BASE), `SYS_SET_TID_ADDRESS=218`. (3) **`build_argv_block` extendido** con auxv mínimo `[{AT_PAGESZ=6, 4096}, {AT_NULL=0, 0}]`. (4) **`elfs/musl.lds`** preserva init_array/fini_array + agrega PT_TLS. (5) **`elfs/tests/hello_musl.c`** smoke test: crt1 boot + auxv parse + TLS wrmsr + argv pass-through + snprintf con `%f` + exit limpio. (6) **`GNUmakefile`** `USER_ELF_MUSL_SRCS` + regla pattern. **Hito**: dos libcs coexisten, path claro a portear apps POSIX reales. | 200 |
| **FASE 12.1 — Polish UX GUI + watchdog + ANSI completo** | (1) **`/bin/uxsh`** mini-shell para oxterm. (2) **oxnotepad acepta argv[1]**. (3) **Parser ANSI completo en oxterm**: state machine ESC→CSI→final; SGR truecolor, cursor pos, erase. Grid de cells `{ch, fg, bg}`. (4) **`/bin/oxfiles`** file browser: opendir + click-to-cd / click-to-edit. (5) **libc stdio EAGAIN retry** (drain_write 200×1ms). (6) **Watchdog auto-resume en consrv + kbdsrv** (defensa contra kill -9 oxsrv). (7) **oxsrv coalesce mouse MOVE** a 1/frame. | 600 |
| **FASE 12.0 — Ox mini-X window system** | (1) **Kernel framebuffer ioctls** Linux-compat: `FBIOGET_VSCREENINFO`, `FBIO_BLIT`. (2) **ABI Ox**: `SERVER_OX=5`, rango IPC `0x60-0x7F`, 14 opcodes. (3) **Cliente libc** (`lib/libc/ox.{c,h}`): API estilo mini-Xlib. (4) **`/bin/oxsrv`** (~700 LOC): registra SERVER_OX, abre /dev/fb0 + mouse0 + input0, backbuffer BGRA full-screen + parse PPM. Loop: drain → recompose (wallpaper → window stack → menu → cursor) → un solo `FBIO_BLIT` por frame dirty. Eventos: click title=focus/drag/close; right-click wallpaper o F1=root menu Openbox-style; Alt+F4=close; Alt+Left=cycle focus. Settings via `/home/.oxrc`. (5) **Apps GUI** (5 × ~250 LOC): oxnotepad, oxcalc, oxterm (PTY+minishell), oxsettings. (6) **Wallpapers** generados al build (PNG si presente, sino procedural). (7) **sd.img 16→32 MiB** + `mformat -c 8` (FAT16 cluster count <65525). (8) **Decisión FAT case-sensitivity**: case-insensitive + case-preserving via LFN. | 2200 |
| **FASE 11.4 — PS/2 mouse driver + `/dev/mouse0`** | Driver PS/2 polling (3-byte packets, sign extension, sync recovery), `mouse_server` kernel task que pushea a ring de 32 events. `/bin/mousetest` muestra eventos en vivo. Habilitó la línea gráfica. PIC IRQ 12 sigue masked. | 250 |
| **FASE 11.3 — jq 1.7.1 port (tercer lenguaje self-host)** | jq vendored (~24K LOC) compilado con `-DWITHOUT_ONIG=1`. Libc gap-fill: `alloca.h`, `pthread.h` shim single-thread, `libgen.h`, `memmem`, `isnormal`, `realpath`, `rand/srand`. **🔥 Bug crítico**: `malloc(0)` retornaba NULL — glibc/musl retornan non-NULL. Fix: `if (size==0) size=1`. Sin este fix jq crashaba al primer `calloc(0, 24)`. `/home/test.json` shipped. | 350 |
| **FASE 11.2 — Lua 5.4 port (segundo lenguaje self-host)** | Lua 5.4.7 vendored (~24K LOC) sin LUA_USE_POSIX → fallback ISO C path. Libc gap-fill: `locale.h`, `sig_atomic_t`, math (`asin/acos/sinh/cosh/tanh/frexp/modf`), time (`clock/mktime/difftime/strftime`), stdlib `system` stub. `/bin/lua` REPL + scripts. | 200 |
| **FASE 11.1 polish — FAT true append + offset-native + caching** | `fat_extend_existing` cluster-chain extend real (O(len) vs O(N) RMW). FAT-sector cache. BUFSIZ 512→4096 en libc. TCC compile time **instantáneo**. `/bin/readelf -S` agregado. | 300 |
| **FASE 11.0 — TinyCC port + offset-native VFS reads (self-hosting tier)** | **HITO HISTÓRICO**: osnos compila C desde adentro. TinyCC 0.9.27 (~30K LOC) con patch crítico: PLT32→PC32 direct relocation cuando static_link (sin esto cada call libc saltaba a *NULL). sysroot en sd.img. **🔥 Bug crítico #1 — sys_read truncaba files >1024 B**: stack scratch hardcoded. Fix: offset-native VFS reads (`vfs_read_at`). **🔥 Bug crítico #2 — fat_append_path truncaba writes >8192 B**: scratch hardcoded. Fix: `kmalloc(existing+len)` cap 4 MiB. libc gap-fill (`ldexp`, `strtod/f`, `struct tm`, `localtime/gmtime`, `gettimeofday`, `fdopen`, `mprotect` noop, `sscanf`). `tcc hello.c -o hello && ./hello` end-to-end. | 900 |
| **FASE 10 — Servers a ring 3** | consrv + kbdsrv + shellsrv ELFs ring-3 reemplazan a los kernel-mode equivalentes. IPC vía service registry. Watchdog auto-restart. Refactor crítico: **el kernel ya no tiene UI** — todo es ring 3. | 1500 |
| **FASE 9 — Scheduler real preemptivo CPL=3** | Timer-driven preemption (50 ms quantum) para tasks ring-3. Ring-0 sigue cooperative. longjmp resume pattern desde sys_exit / fault handlers. | 800 |
| **Pre-FASE 9 — ABI POSIX core** | fork(2) + execve(2) + wait(2) + sigaction(2) reales. Process groups + sessions. OFD shared offsets. FD_CLOEXEC. PTY pairs. SIGCHLD automático. EINTR. WUNTRACED/WCONTINUED. mmap anónimo + brk. Pipes multi-stage + O_NONBLOCK. FXSAVE/FXRSTOR per-task. Pipes shell `\|`, redirection `> >> <`. Self-tests: 23/23 PASS. | 4000 |
| **Pre-FASE 9 — Networking** | Stack TCP/IP completo: ARP + IPv4 + ICMP + UDP + TCP. RTL8139 driver. Sockets POSIX. DNS. `/bin/httpd`. selectserver de Beej verbatim. | 3500 |
| **Pre-FASE 9 — Disco real FAT16** | block_ata PIO. FAT16 read/write. Persistent /home, /etc via aliasfs. sd.img pre-poblado al build. | 2000 |
| **FASE 8 — Base anterior** | Kheap robusto, TTY line discipline + termios, env passing + PATH, shell rc + history, job control (Ctrl+Z/fg/bg/jobs), `/bin/ovi` editor modal vim-style, getcwd/chdir, mmap. Total kernel + libc pre-FASE-11: ~25K LOC. | 8000 |

---

## Inventario actual (snapshot post-FASE-13.3)

- **Kernel ELF**: ~1.5 MB stripped (`build/kernel`)
- **sd.img**: 32 MiB FAT16, ~98 ELFs en `/bin/` + sysroot completo en `/lib/` + `/usr/include/`
- **ISO bootable**: ~19 MB (`build/osnos-x86_64.iso`)
- **Memoria total esperada**: 2 GiB de RAM (`-m 2G` en QEMU)
- **Boot time**: ~3-4 segundos (kernel + spawn servers + ash banner)
- **Tests automated**: 18/18 PASS via `/bin/alltest`

---

## Roadmap futuro

### FASE 14 — Self-hosting completo (plan en curso)

Objetivo: poder `cd /home && make hello && ./hello` desde adentro,
luego construir el resto incrementalmente.

#### FASE 14.1 — Port `make` (pdpmake) — ✅ **CERRADA — self-hosting build funciona end-to-end**

`cd /home && make hello && /home/hello` compila y corre tcc-generated ELF
desde adentro de osnos. Verificado: output `hello from tcc on osnos!`.

Trabajo (~6 cambios cascading, todos críticos):

- ✅ **`vendor/pdpmake/`** — pdpmake 1.4.1 (POSIX make, public domain, ~3.4K LOC) vendoreado y compilado contra mini-libc → `/bin/make`.
- ✅ **Mini-libc gap-fill** (`lib/libc/posix_extras.c` + headers): `getopt/optarg/optind/opterr/optopt`, `stpcpy`, `popen/pclose`, `utimensat` (stub), nuevos headers `<strings.h>`, `<glob.h>` (stub `GLOB_NOMATCH`), `<ar.h>`, extensiones a `<fcntl.h>` (AT_FDCWD, UTIME_NOW, UTIME_OMIT) + `<sys/stat.h>` rediseñada a `st_atim/mtim/ctim` (struct timespec) con macros legacy `st_atime` → `st_atim.tv_sec` (layout binario intacto, compat Linux).
- ✅ **`resolve_path`** helper en `sys_open` + `sys_stat/access/mkdir/rmdir/unlink/rename/chdir` — relative paths se resuelven contra `task->cwd`. Antes el kernel rechazaba paths sin `/` inicial con EINVAL, rompiendo `fopen("Makefile")` desde pdpmake/tcc.
- ✅ **Fix exec preserva cwd** (`src/proc/exec.c`): antes `proc_execve` reseteaba `t->cwd = "/"` y leía PWD del envp. Pero busybox ash no exporta PWD consistente al envp del child → `cd /home && make hello` terminaba con cwd=`/`. Fix: si `t->cwd` ya está seteado (caso normal de fork+exec), preservar. Sólo seedear cuando viene vacío (spawn directo del kernel).
- ✅ **Fix getopt convención GNU libc** (`lib/libc/posix_extras.c`): pdpmake hace `optind = 0` para resetear getopt entre llamadas (`GETOPT_RESET()`). Convención GNU dice "0 = reset + arrancar en argv[1]". Mi getopt tomaba 0 literal y consumía argv[0]. Sin este fix `make hello` decía `make: don't know how to make make` (target = nombre del programa).
- ✅ **`/bin/sh` = copia de `/bin/busybox`** (FAT16 no tiene symlinks; busybox dispatcha por argv[0]). pdpmake's `system()` invoca `/bin/sh -c "..."`.
- ✅ **`/home/Makefile` + `/home/hello.c`** seeded al sd.img como demo del workflow `make hello`.
- ✅ **🔥 Bug crítico — sys_execve aplanaba argv en string + re-tokenizaba**: `sys_execve` concatenaba `argv[1..N]` en `args_kbuf` separados por espacio, luego `proc_execve_replace → build_argv_block` re-tokenizaba ese string por whitespace. Resultado: `execve("/bin/sh", ["sh","-c","echo HELLO"])` se convertía en argv=`["sh","-c","echo","HELLO"]` y `sh -c echo` corría echo con `HELLO` como `$0` (no como arg) → output vacío. Esto rompía TODA recipe de make que pasara comandos con args via `system()`. Fix: nueva `proc_execve_replace_argv(path, argv[], envp)` + `build_argv_block_argv` que consumen argv ARRAY sin tokenizar; `sys_execve` lo usa directamente preservando boundaries. (`build_argv_block` string-version sigue para callers internos que pasan strings ya tokenizables — `proc_execve` desde kmain.)
- ✅ **`SYS_CLONE` real con `CLONE_VM` + `CLONE_VFORK`** (`src/micro/syscall.c` + `src/proc/exec.c` + `src/micro/task.{c,h}`): para musl `posix_spawn`. Cuando `flags & CLONE_VM`, el child comparte `pml4` con el parent (refcount via lookup en `task_pml4_other_users`); `user_stack_top = child_stack`. Cuando además `CLONE_VFORK`, parent se marca `TASK_BLOCKED` con snapshot del syscall context, child arranca primero; al `proc_execve_replace_argv` o `proc_exit_current_user` del child, parent se despierta (saved_rax = child pid). `address_space_destroy` en exit/exec se skip-ea si todavía hay otros tasks usando ese pml4. Sin flags `CLONE_VM`, alias trivial de `sys_fork`. **Sin esto, posix_spawn (que musl usa para `system()`) corrompía el address space del parent al compartir AS sin refcounting**.

- ✅ **🔥 Bug crítico — execve no reseteaba sa_handler[] (POSIX violation)**: `proc_execve_replace[_argv]` no reseteaba los signal handlers caught a `SIG_DFL`. Cuando ash forkeaba para `make hello`, make heredaba la sa_handler[] tabla — incluido el `SIGCHLD` handler de busybox apuntando a `signal_handler` en el text segment de busybox (0x235787). Cuando sh exec'd terminaba, kernel mandaba SIGCHLD a make; iretq jumped a 0x235787 que NO está mapeado en el address space de make → page fault. Fix: en execve, iterar 32 slots y resetear cualquier handler distinto de SIG_IGN a SIG_DFL (`t->sa_handler[i] = 0; t->sa_restorer[i] = 0`). Diagnosis via dump del user stack en el page fault handler (vimos rip=0x235787, restorer=0x25a179=`__restore_rt` en busybox via `llvm-objdump`).

Verificación end-to-end: `sh -c "echo a b c"` → `a b c`; `cd /home && make hello` → tcc compila SIN SEGFAULT; `/home/hello` → "hello from tcc on osnos!"; `make clean` ejecuta la recipe limpia. Único item pendiente cosmético: mini-libc `/bin/rm` no soporta `-f` flag (independiente, no bloquea FASE 14.1).

#### FASE 14.2 — AF_UNIX sockets (pendiente)
- ❌ Familia `AF_UNIX` en `sys_socket` + `bind`/`connect`/`accept` con namespace de paths (`/tmp/X11-unix/X0` etc.).
- Necesario para X11 wire protocol (xeyes habla AF_UNIX al server).

#### FASE 14.3 — POSIX SHM (pendiente)
- ❌ `shm_open` + `mmap(MAP_SHARED, fd)`: shared file-backed mmap para pixmaps cliente↔server.
- Hoy `mmap` es anonymous-only.

#### FASE 14.4 — Dynamic linking (.so) (pendiente)
- ❌ ELF dynamic loader (`ld-musl.so`), PT_INTERP, PT_DYNAMIC, GOT/PLT, lazy resolve.
- Habilita ejecutables Linux unmodified que asuman dynamic libc.

#### FASE 14.5 — Ox extendido + Ox-as-X11 (pendiente)
- ❌ Ox compite con nano-X (botones reales, widget tree, propiedades).
- ❌ Capa de traducción protocol X11-wire ↔ IPC Ox (analogía: "Ox a X11 como osnos a Linux ABI").

#### FASE 14.6 — `xeyes` (test del camino completo)

### FASE 14-misc — Quality of life menores
- ❌ Per-PTY termios real (cada shell/REPL su propio termios, no global)
- ❌ Fix de argv passing en sqlite3 (debug por qué SQL en argv se trunca)
- ❌ Fix de page fault en musl atexit (sqlite3 sale limpio cosmético)
- ❌ Habilitar más BusyBox: `top` `ps` `free` `uptime` (necesitan `/proc`)
- ❌ Implementar `/proc` synthetic (al menos `/proc/<pid>/cmdline`, `/proc/meminfo`)
- ❌ Chip-8 emulator (último item pendiente del roadmap original gráfico)

### FASE 15 — Drivers a ring 3 (item pendiente del FASE 11 original)
- ❌ IRQ delegation por IPC desde kernel-side handlers
- ❌ MMIO mapping per-task con permisos especiales
- ❌ Port-IO delegation (syscall whitelist o IOPB en TSS)
- ❌ DMA bouncing via kernel-mediated buffer pool
- ❌ Portar PS/2, framebuffer, ATA, RTL8139, PIT a `elfs/osn-driver/`

### Futuro lejano
- ❌ SMP (multi-core)
- ❌ Copy-on-write para fork (hoy full page copy)
- ❌ File-backed mmap (path a port real de tinyX/X11)
- ❌ Real X11 wire protocol (oxlib es shim hasta que llegue tinyX)
- ❌ ext2/ext4 read-only (alternativa a FAT16 para más capacidad)
- ❌ Más vendor ports: perl tiny, sqlite-net, lua-luarocks, etc.

---

## Convenciones del proyecto

- **Lenguaje**: C99 (kernel + mini-libc), código del kernel con `-Werror`
- **Toolchain**: clang + ld.lld (cross-compile desde macOS o Linux)
- **Bootloader**: Limine 8.x (instalado del sistema, no versioned en repo)
- **Test infra**: `./build_and_run.sh headless` + serial captura → grep para CI
- **Doc en español**: STATUS.md (este), CREATE_BUILTINS.es.md, CREATE_ELF.es.md
- **Doc en inglés**: README.md raíz, CLAUDE.md (para asistente IA), ARCH.md

Para entrar al proyecto después de meses: leer este STATUS.md →
README.md → ARCH.md → CLAUDE.md → CREATE_ELF.es.md (en ese orden).

---

## Cómo extender

Tres puntos de entrada típicos:

1. **Agregar un comando al shell** — solo es un nuevo applet de
   BusyBox (rebuild con la option) o un nuevo ELF en `elfs/tools/`.
   Para los ELFs: drop `elfs/tools/foo.c`, agregar a
   `USER_ELF_LIBC_SRCS` en GNUmakefile, `make` — el binario aparece
   en `/bin/foo`.

2. **Agregar un programa contra musl** — drop `elfs/tests/foo.c`,
   agregar a `USER_ELF_MUSL_SRCS`, agregar regla específica en
   GNUmakefile (template: copiar la de `hello_musl.elf`). Útil para
   programas que necesitan stdio completo, printf-%f, locale.

3. **Agregar un syscall nuevo** — definir el número en
   `src/micro/syscall.h` (rango 500+ para osnos-specific; matching
   Linux x86_64 para POSIX), implementar handler en
   `src/micro/syscall.c` (`int64_t sys_foo(...)`), agregar case en
   el dispatcher. Si tiene wrapper de libc: drop en `lib/libc/`
   correspondiente.

Detalle paso a paso en [`CREATE_ELF.es.md`](CREATE_ELF.es.md) y
[`CREATE_BUILTINS.es.md`](CREATE_BUILTINS.es.md).
