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

- **Kernel ring-0** con ELF loader (ET_EXEC + ET_DYN/PT_INTERP),
  paging propio (4 niveles), VFS multi-backend (ramfs + FAT16 + devfs
  + sysfs + binfs + aliasfs), scheduler preemptivo (50 ms quantum en
  CPL=3), IPC queue de 64 slots, line discipline POSIX, **~80
  syscalls** compatibles con Linux x86_64 (read/write/open/fork/clone/
  execve/wait/sigaction/pipe/mmap/shm/socket/AF_UNIX/select/poll/...).
- **Servidores ring-3**: console (`consrv`), keyboard (`kbdsrv`),
  mouse feeder, shell (`busybox sh` desde FASE 13.1).
- **Dos libcs**: mini-libc (`lib/libc/`) para programas chicos, y
  **musl 1.2.5** (FASE 13.0) — ahora también **shared lib**
  (`/lib/libc.so` + `/lib/ld-musl-x86_64.so.1`) para dynamic linking
  (FASE 14.4).
- **BusyBox 1.36.1** linkeado contra musl (FASE 13.1): ~60 applets
  accesibles vía aliases en `/home/.ashrc`. Default shell es
  `busybox sh` con history persistente.
- **Cinco lenguajes / self-hosting completo**: C (TCC 0.9.27,
  FASE 11.0), Lua 5.4.7 (FASE 11.2), jq 1.7.1 (FASE 11.3), SQL via
  SQLite 3.45.2 (FASE 13.3), **POSIX make (pdpmake) — `cd /home &&
  make hello && ./hello` compila con tcc desde adentro** (FASE 14.1).
- **Ox mini-X window system** (FASE 12.0): server + 5 apps GUI.
- **🎉 lighttpd 1.4.76** (FASE 14.5) — webserver real sirviendo
  HTTP/1.1 sobre `/home`; `curl http://localhost:8080/` → 200 OK.
- **Networking real** (FASE 14-misc-3): `nslookup google.com → 142.251.x.x`
  (DNS UDP resolver vía musl `getaddrinfo`) y `ping 8.8.8.8 → 64 bytes
  ttl=255` (SOCK_RAW + ICMP echo). Nuevas syscalls: `recvmsg`/`sendmsg`,
  `getsockname`/`getpeername`/`getsockopt` stubs. `SOCK_CLOEXEC` +
  `SOCK_NONBLOCK` bundled flags ahora soportados.
- **POSIX IPC moderno** (FASE 14.2-14.4): AF_UNIX SOCK_STREAM,
  `shm_open` + `mmap(MAP_SHARED, fd)` con shared memory cross-fork,
  dynamic linking via `ld-musl.so` (apps `.so`-linked corren).
- **Console serial dual** (UART 16550 COM1) para boot headless / CI.
- **21/21 tests automatizados** via `/bin/alltest` (incluye
  unixtest, shmtest, hello_dyn). Cada test con timeout de 60s para
  que un cuelgue no bloquee la suite.
- **Bug fixes recientes notables**: (a) `rdmsr FS_BASE` en
  `sys_fork`/`sys_clone` (el snapshot stale daba NULL deref en
  musl `__post_Fork`'s `__get_tp`); (b) `kill_pending` ya no force-
  exit-ea si la app instaló handler (lighttpd graceful shutdown via
  Ctrl+C); (c) `TIOCSPGRP`/`TIOCGPGRP` ioctls para que `tcsetpgrp`
  rute Ctrl+C al pgid correcto; (d) `SA_SIGINFO` handlers reciben
  rsi/rdx = NULL (antes eran basura → page fault); (e) execve
  resetea `sa_handler[]` a SIG_DFL (POSIX); (f) `sys_execve`
  preserva argv boundaries; (g) `sys_read`/`write` AF_INET dispatch;
  (h) **IPC_OX_PRESENT siempre marca dirty** (FASE 12.2) — el check
  legacy `if (g_wins[slot].dirty)` quedó obsoleto tras refactor SHM,
  los draws nunca lo seteaban → composite no disparaba (causa raíz
  de "Settings sin thumbs" + "lag mouse post-close"); (i) **PTE_SHM
  bit** (FASE 12.2) — address_space_destroy ya no libera al PMM
  páginas que pertenecen al shm_obj (eran double-free latente +
  corrupción del backing de oxsrv post-exit del cliente).

**Pitch en una frase**: hobby OS x86_64 que corre BusyBox + SQLite +
Lua + jq + TCC + **make + lighttpd**, todos compilados nativos contra
musl (static y dynamic), con AF_UNIX + POSIX SHM + dynamic linking
funcionales, y un mini-X window system propio — todo desde un
microkernel escrito desde cero.

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
| **`fs_base` save/restore en task switch + rdmsr live en fork** | ✅ | Per-task TLS pointer; rdmsr en sys_fork/sys_clone evita stale snapshot que NULL-derefeaba musl `__post_Fork` |
| Per-task fd table (16 fds) + OFD pool (128) | ✅ | Shared offsets POSIX |
| pipe / dup / dup2 / fcntl | ✅ | FD_CLOEXEC per-fd |
| `mmap`/`munmap` anónimo + brk/sbrk | ✅ | mmap_regions tracking + shm_backed flag |
| **`mmap(MAP_SHARED, fd_shm)` con fork preserva pages compartidas** | ✅ | Fork fixup re-mappea phys pages del parent (FASE 14.3) |
| Signal delivery (sigaction, sigreturn, EINTR) | ✅ | Sigframe en user stack; rdi/rsi/rdx = sig/NULL/NULL (SA_SIGINFO compatible) |
| **`kill_pending` honra user handler** | ✅ | Ctrl+C en apps con SIGINT handler hace graceful shutdown, no force-exit (FASE 14.5 polish) |
| **`sa_handler[]` reset a SIG_DFL en execve** | ✅ | POSIX violation fix — antes child heredaba handlers cuyos pointers vivían en text del binario viejo |
| SIGCHLD automático + waitpid + WIFEXITED/SIGNALED | ✅ | TASK_ZOMBIE state |
| Process groups + sessions + Ctrl+C fan-out a pgid | ✅ | WUNTRACED/WCONTINUED; **`TIOCSPGRP`/`TIOCGPGRP`** ioctls (FASE 14.5 polish) |
| PTY pairs (`/dev/ptmx` + `/dev/pts/N`, pool 8) | ✅ | Canon/raw, ECHO, TIOCS* ioctls |
| **POSIX line discipline TTY** + echo + backspace consistentes | ✅ | Echo via `framebuffer_write_bytes` mismo path que apps (FASE 13.3 fix) |
| **`SYS_CLONE` real** (`CLONE_VM`, `CLONE_VFORK`, `SIGCHLD`) | ✅ | Para musl `posix_spawn`; pml4 sharing via lookup-refcount (FASE 14.1) |
| **`sys_execve` preserva argv boundaries** | ✅ | Array-based path (no flat-join + re-tokenize) — `sh -c "echo HELLO"` ahora ve 3 argv correctos (FASE 14.1) |
| **`sys_read`/`write` dispatch AF_INET + AF_UNIX** | ✅ | Read/write directo sobre stream sockets (lighttpd usa este path) |
| **`sys_setsockopt` permisivo** | ✅ | SOL_SOCKET/IPPROTO_TCP/IPPROTO_IP = no-op success (acepta TCP_NODELAY etc.) (FASE 14.5) |
| **Auxv completo** (AT_PHDR/PHENT/PHNUM/BASE/ENTRY/RANDOM) | ✅ | Para que ld-musl.so pueda parsear (FASE 14.4) |
| IPC queue 64 × 1024 B + service registry | ✅ | Routing por SID o pid directo |
| `init-respawn` watchdog para servers | ✅ | consrv/kbdsrv/busybox auto-restart |

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
| Sockets POSIX (socket/bind/listen/accept/connect/send/recv/select) | ✅ | read/write directo soportado (FASE 14.5) |
| **`SOCK_CLOEXEC` + `SOCK_NONBLOCK` flag bundle en `socket(2)` type arg** | ✅ | musl `res_msend` pasa `SOCK_DGRAM\|SOCK_CLOEXEC\|SOCK_NONBLOCK` (=0x80802) — antes rejectaba con EAFNOSUPPORT y rompía `getaddrinfo` (FASE 14.6) |
| **`recvmsg(2)` / `sendmsg(2)`** (Linux 46/47) — single-iovec | ✅ | musl resolver usa recvmsg; sin esto nslookup nunca matcheaba (FASE 14.6) |
| **`getsockname` / `getpeername` / `getsockopt` stubs** | ✅ | Devuelven OK + zero — suficiente para musl post-connect (FASE 14.6) |
| **`sys_recvfrom` UDP path preserva `src_ip/src_port`** | ✅ | Antes routeaba via `sock_recv` que descartaba el peer → musl resolver rechazaba el reply (FASE 14.6) |
| **`SOCK_RAW` + ICMP echo (ping)** | ✅ | `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)`; ip_handle mirror al raw socket pool; `ping 8.8.8.8 → "64 bytes from 8.8.8.8 ttl=255"` (FASE 14.6) |
| DNS resolver + getaddrinfo (vía slirp 10.0.2.3) | ✅ | `nslookup google.com → 142.251.128.46` (FASE 14.6) |
| `/bin/httpd` sirviendo FAT16 sobre HTTP | ✅ | hostfwd 8080 |
| **`/bin/lighttpd` 1.4.76 webserver real** | ✅ | poll-based, 10 builtin mods, sirve `/home` (FASE 14.5) |
| **`SIOCGIFCONF` + 7 ioctls SIOC*** | ✅ | `ifconfig` muestra eth0 (10.0.2.15 + MAC 52:54:00:12:34:56) + lo |
| Demos (`/bin/tcpclient`, `udptest`, `echotcp`, `selectserver`, `udp_send`, `udp_connect`) | ✅ | |

### POSIX IPC + dynamic linking (FASE 14)
| Subsistema | Estado | Notas |
|---|---|---|
| **AF_UNIX SOCK_STREAM** (`/bin/unixtest` smoke) | ✅ | Pool 32 sockets + 16 paths bound + ring buffers 4 KiB; sin abstract namespace (FASE 14.2) |
| **POSIX `shm_open` + `mmap(MAP_SHARED, fd)`** (`/bin/shmtest`) | ✅ | Pool 16 objetos × 256 páginas; shared memory cross-fork verificado (FASE 14.3) |
| **Dynamic linking via `ld-musl-x86_64.so.1`** (`/bin/hello_dyn`) | ✅ | PT_INTERP + auxv completo; apps `.so`-linked corren (FASE 14.4) |
| **`/lib/libc.so` + `/lib/ld-musl-x86_64.so.1`** staged en sd.img | ✅ | musl rebuilt con shared support; mismo binario es libc.so y el dynamic linker |

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

### Self-hosting (5 lenguajes + make)
| Componente | Estado | Notas |
|---|---|---|
| **`/bin/tcc` — TinyCC 0.9.27** | ✅ | C compiler; produce ELFs estáticos runnable contra `/lib/libc.a` (FASE 11.0) |
| **`/bin/lua` — Lua 5.4.7** | ✅ | REPL + scripts (FASE 11.2) |
| **`/bin/jq` — jq 1.7.1** | ✅ | Filter/transformer JSON (FASE 11.3) |
| **`/bin/sqlite3` — SQLite 3.45.2** | ✅ | SQL engine completo + `/home/demo.db` (15 books + view + indices) preseeded (FASE 13.3) |
| **`/bin/make` — pdpmake 1.4.1 (POSIX make)** | ✅ | `cd /home && make hello && ./hello` compila con tcc end-to-end (FASE 14.1) |

### Window system (Ox)
| Componente | Estado | Notas |
|---|---|---|
| **`/bin/oxsrv`** — server ring-3 ~1900 LOC | ✅ | Compositor SHM-backed + cursor + z-order + root menu Adwaita-dark (FASE 12.0/12.2) |
| **SHM-backed window backings** | ✅ | Cliente + oxsrv comparten mmap MAP_SHARED del mismo shm_obj; `ox_draw_*` son escrituras locales, `ox_present` = 1 IPC. De ~1270 IPCs por render a 1 (FASE 12.2) |
| **PTE_SHM bit en PTEs** | ✅ | `address_space_destroy` salta páginas SHM (las posee shm_obj, no el task). Sin esto, exit de cliente corrompía framebuffer (FASE 12.2) |
| **Diagnostics heartbeat** | ✅ | iters/Hz, ev/s(m/k/i), full breakdown (alloc/destroy/raise/reload), t_full/t_dirty/t_destroy en ms con max, avg_px del dirty rect (FASE 12.2) |
| **Ox client API** (`lib/libc/ox.{c,h}`) estilo mini-Xlib | ✅ | window_create/draw_rect/draw_text/draw_image/present/poll_event; draws son writes locales al mmap'd backing |
| `/bin/oxnotepad` text editor con argv path | ✅ | |
| `/bin/oxcalc` calculadora 4-func | ✅ | |
| `/bin/oxterm` PTY + uxsh sub-shell + parser ANSI completo (SGR truecolor, cursor pos, erase) | ✅ | (FASE 12.1) |
| `/bin/oxfiles` file browser click-to-open | ✅ | (FASE 12.1) |
| `/bin/oxtop` process viewer (kill por PID) | ✅ | (FASE 12.2) |
| `/bin/oxsettings` wallpaper picker | ✅ | Edita `/home/.oxrc`, IPC_OX_RELOAD_SETTINGS; thumb grid 200×120 desde `/home/wallpapers/thumbs/` |
| Wallpapers JPG → PPM build-time (`wallpaper1.jpg` default + 9 más) | ✅ | `tools/gen_wallpapers.sh` produce PPM P6 + thumbnails 200×120 |
| Ioctls de FB: `FBIOGET_VSCREENINFO`, `FBIO_BLIT` (Linux-compat) | ✅ | |

### Limitaciones conocidas
- ❌ SMP (multi-core)
- ❌ Copy-on-write para fork (hoy full page copy; shm-backed regiones SÍ comparten físico)
- ❌ File-backed mmap de archivos regulares (solo anonymous + MAP_SHARED sobre shm fd)
- ❌ Real X11/tinyX (Ox es protocolo IPC propio; pero los ioctls + `<linux/fb.h>` están listos para un futuro port)
- ❌ IPv6, epoll, kqueue, sendfile, inotify
- ❌ PIE main / ET_DYN executable con load offset random (solo el interpreter es ET_DYN)
- ❌ RTLD_LAZY / dlopen / dlsym / DT_NEEDED transitivo (musl los expone pero no probamos cargar libs adicionales)
- ❌ `epoll_*` syscalls (lighttpd usa el fallback `poll`)
- ⚠️ lighttpd con `&` background falla por `sh: can't open '/dev/null'` raro de busybox — workaround: correr foreground
- ⚠️ TTY global compartido entre tasks (no per-PTY real) — mitigado con anti-clobber de tcsetattr y echo via path compartido
- ⚠️ Single FP state HW para múltiples tasks — FXSAVE/FXRSTOR per-task implementado pero no extensivamente testeado
- ⚠️ sqlite3 con SQL en argv tiene argv passing issues residuales (workaround: stdin redirect)
- ⚠️ sqlite3 exit limpio puede page-faultear en musl atexit cleanup (cosmético; no afecta a ash gracias al fix FS_BASE)
- ⚠️ `SA_SIGINFO` handlers reciben `siginfo_t *` = NULL (no populamos struct; apps con null-check ok, apps que lo asumen non-null pueden faultear)

---

## Bitácora de fases (orden cronológico inverso)

**Más recientes primero**. Cada entrada describe trabajo, decisiones,
y bugs notables encontrados.

| Fase | Subsistema | LOC aprox |
|------|-----------|-----------|
| **FASE 12.2 — Ox performance + premium fluidity (SHM-backed windows funcionando end-to-end)** | (1) **🔥 Causa raíz del "lag al cerrar + Settings sin thumbs"**: el handler de `IPC_OX_PRESENT` en oxsrv tenía un check legacy `if (g_wins[slot].dirty)` heredado de la era pre-SHM (cuando cada `DRAW_RECT/TEXT/IMAGE` IPC seteaba el flag). Tras refactor SHM, los draws son escrituras locales al mmap — el flag NUNCA se setea → `mark_dirty` nunca dispara → composite se saltea. Settings cargaba thumbs en SHM pero la pantalla no se refrescaba hasta que un evento externo (open/close de otra ventana) forzaba un full repaint que incidentalmente repintaba settings. Mismo bug para "todo se siente laggy post-close de una app": cualquier redraw de las apps que quedaban abiertas era ignorado, cursor encima de ventanas con contenido stuck. Fix: PRESENT siempre marca dirty (no flag check). (2) **🔥 PTE_SHM bit (kernel `vmm.h` + `syscall.c` + `vmm.c`)**: nuevo flag software AVL bit 9 en PTEs. `sys_mmap` shm path + `address_space_clone` fork shm fixup setean el bit. `address_space_destroy` salta `pmm_free_page` para PTEs con PTE_SHM — esas páginas las posee el `shm_obj`, las libera el último `shm_unref`. Sin esto, exit del cliente Ox devolvía al PMM páginas que oxsrv todavía tenía mapeadas → corrupción de framebuffer + double-free latente. (3) **`task_reap_dead` defensive IPC cleanup**: además del `ipc_drop_for_pid` en `proc_exit_current_user`, el reaper hace un segundo pase al recyclar el slot — cierra la ventana de carrera donde una IPC llega entre el drop y el state-flip a ZOMBIE. (4) **`task_wake_pollers` filtrado**: solo despierta tasks BLOCKED con `saved_rax == SYS_POLL` o `SYS_IPC_SEND` (vs todos). Evita thundering herd cuando un mouse push despertaba a consrv que estaba BLOCKED en IPC recv. (5) **Menu dirty-rect en oxsrv**: 5 sitios del menu (right-click open, F1 toggle, hover, item pick, click outside) seteaban `g_dirty=1` sin llamar `mark_dirty(...)` → caían al path full (~12 MB memcpy + blit). Nuevo `mark_menu_dirty()` helper marca solo el bbox del menu. (6) **vmm_unmap cleanup intermediate PT pages bottom-up**: walks PT/PD/PDPT freeing empty levels. Sin esto, mmap/munmap cycles en procesos long-running (oxsrv window backings) leakeaban ~4 KB por ciclo. (7) **`framebuffer_blit_kernel` row-memcpy**: pasó de pixel-por-pixel volatile loop a `os_memcpy` per row (~10x más rápido en QEMU). (8) **SHM bumped 16/256→32/1024**: 4 MiB max por objeto, 32 objetos. Acomoda oxsettings 720×560 (1.6 MiB) + 10 thumbnails + ventanas concurrentes. (9) **`fd_readable` para `/dev/mouse0` y `/dev/input0`**: revisa el nivel del ring vía `devfs_mouse_has_data()`/`devfs_input_has_data()` — antes siempre retornaba true causando que `sys_poll` regresara inmediatamente sin datos. (10) **Heartbeat 2s→5s**: el `write(ttyfd, hb, ~250)` al UART COM1 bloquea ~22 ms por byte-by-byte busy-loop en `serial_putc`. A 5 seg el freeze visible es <0.5% del tiempo. (11) **Diagnostics instrumentadas** en oxsrv heartbeat: counters per-trigger de fulls (alloc/destroy/raise/reload/other), timing en ms (`t_full_ms` / `t_dirty_ms` / `t_destroy_ms` con max), iters/sec, ev/s(m/k/i), avg_px dirty rect, last full reason. Estos datos confirmaron que composite es <1ms en QEMU y descartaron al compositor como cuello de botella — el problema real era el bug del flag PRESENT. | 500 |
| **FASE 14.5 polish — Ctrl+C catchable + TIOCSPGRP + SA_SIGINFO null-args** | (1) **`kill_pending` honra user handler**: antes `proc_exit_current_user(128+sig)` se llamaba siempre si kill_pending=1. Ahora si la app instaló handler (sa_handler ≠ DFL ≠ IGN) y sig ≠ SIGKILL, fall-through al signal delivery loop para invocar el handler. (2) **`TIOCGPGRP`/`TIOCSPGRP` ioctls**: busybox ash llama `tcsetpgrp(STDIN, pgid_of_fg_job)`; sin estos ioctls fallaba con ENOTTY y `kernel_fg_pid` quedaba 0, tty_signal silently dropped. Ahora `tcsetpgrp` actualiza `kernel_fg_pid` y Ctrl+C rutea correctamente. (3) **SA_SIGINFO compat**: handlers con signature 3-arg `void h(int, siginfo_t *, void *)` leían rsi/rdx con basura de la syscall → page fault al primer `movups (rsi+0x70)`. Fix: zerar buf[8]/buf[9] (rdx/rsi = NULL) en signal delivery. Apps con NULL-check (lighttpd cmovneq) usan fallback. (4) **Verificado**: lighttpd Ctrl+C → graceful shutdown (exit=0). | 60 |
| **FASE 14.5 — lighttpd 1.4.76 port (real HTTP server)** | (1) **`vendor/lighttpd/`** (124 .c, ~106K LOC) sin autotools/cmake: hand-craft `build-osnos/config.h` (30 HAVE_* matching musl), `plugin-static.h` (10 builtin mods), `lemon` compilado en host genera `configparser.c`. (2) **fdevent backend = poll** (no epoll). (3) Output `/bin/lighttpd` 1.85 MB ELF estático. (4) **Kernel fix #1 — sys_read/write dispatch AF_INET**: era omisión; httpd viejo usaba sendto/recvfrom directo, lighttpd usa read/write standard. Fix: ramos sock_recv/sock_send también en sys_read/write. (5) **Kernel fix #2 — sys_setsockopt permisivo**: ahora acepta no-op success todos los flags bajo SOL_SOCKET/IPPROTO_TCP/IPPROTO_IP. (6) **Config seedeada** en `/etc/lighttpd/lighttpd.conf`, alias `lighttpd='lighttpd -f /etc/lighttpd/lighttpd.conf'` en `.ashrc`. (7) **Verificado**: `curl http://localhost:8080/` → HTTP 200 OK + body, múltiples paths (`/index.html`, `/hello.c`, `/demo.sql`). | 350 |
| **FASE 14.4 — Dynamic linking via ld-musl.so** | (1) **musl rebuild con shared**: `./configure` sin `--disable-shared`; `lib/libc.so` (882 KB ELF DYN) sirve a la vez como libc.so y como dynamic linker (`ld-musl-x86_64.so.1`). Manual `ld.lld` link (clang chokeaba con `-Wa,--noexecstack`). (2) **Stubs compiler-rt** (`__mulxc3`/`__mulsc3`/`__muldc3`) linkeados a libc.so para que ld.so no reporte undefined symbols. (3) **`elf_load_dyn(main, interp)` + `elf_get_interp`**: detecta PT_INTERP en main, carga interpreter en `INTERP_LOAD_BASE=0x40000000`, devuelve `elf_load_result_t` con e_entry, phdr_user_va, phnum, phentsize, interp_base. (4) **Auxv extendido** (8 pairs): AT_PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY/RANDOM/NULL. (5) **`proc_execve_replace_argv`** detecta PT_INTERP y rutea a `elf_load_dyn` + `build_argv_block_argv_dyn`. (6) **sd.img bump 32→64 MiB** para acomodar libc.so duplicado. (7) **`elfs/tests/hello_dyn.c`** verificado: `/bin/hello_dyn` → "hello from dynamic linker on osnos!". | 700 |
| **FASE 14.3 — POSIX SHM (`shm_open` + `mmap MAP_SHARED`)** | (1) **`src/micro/shm.{c,h}`** (~170 LOC): pool 16 objetos × 256 páginas = 1 MiB. Estado `refcount + unlinked` (POSIX: persiste hasta unlink + último close). (2) **OFD extendido** con `is_shm + shm_ref`. (3) **Syscalls** `SYS_SHM_OPEN=519`/`SYS_SHM_UNLINK=520`; `sys_ftruncate` dispatchea a `shm_truncate`. (4) **`sys_mmap` con MAP_SHARED fd-backed**: vmm_map las páginas físicas del shm_obj sin pmm_alloc, `shm_backed=1` en `mmap_regions` para que munmap solo vmm_unmap. (5) **🔥 Fix crítico en `sys_fork`**: `address_space_clone` clonaba shm pages; fix re-mappea phys originales del parent. Sin esto, write del child invisible al parent. (6) **mini-libc gap-fill**: `ftruncate` wrapper + `shm_open`/`shm_unlink`. (7) **`elfs/tests/shmtest.c`** verifica round-trip shared cross-fork. | 250 |
| **FASE 14.2 — AF_UNIX SOCK_STREAM** | (1) **`src/include/osnos_unix_abi.h`** + **`lib/libc/include/sys/un.h`**: sockaddr_un layout-compat Linux. (2) **`src/micro/unix_sock.{c,h}`** (~270 LOC): pool 32 sockets + 16 paths bound, ring buffers 4 KiB por dir, backlog 8. Estados UNUSED/UNBOUND/LISTENING/CONNECTED/DISCONNECTED. Sin abstract namespace ni SOCK_DGRAM. (3) **OFD extendido** con `is_unix_socket + unix_idx` paralelos a is_socket. (4) **Dispatch en syscalls**: sys_socket/bind/listen/connect/accept/read/write/sendto/recvfrom/fd_readable ramifican por familia. (5) **Errno extendido**: EISCONN=106, ENOTCONN=107. (6) **`elfs/tests/unixtest.c`** verifica PING/PONG roundtrip parent↔forked child. | 300 |
| **FASE 14.1 — POSIX make (pdpmake) — self-hosting build** | (1) **`vendor/pdpmake/`** 1.4.1 (~3.4K LOC) contra mini-libc → `/bin/make`. (2) **mini-libc gap-fill** (`posix_extras.c`): getopt, stpcpy, popen/pclose, utimensat stub. Nuevos headers `<strings.h>`, `<glob.h>` (GLOB_NOMATCH stub), `<ar.h>`. `<sys/stat.h>` rediseñada con `st_atim/mtim/ctim` (struct timespec) + macros legacy `st_atime` → `st_atim.tv_sec`. (3) **`resolve_path`** helper en sys_open/stat/access/mkdir/rmdir/unlink/rename/chdir — relative paths resuelven contra `task->cwd`. (4) **Exec preserva cwd** si ya está seteado (caso fork+exec); antes lo reseteaba siempre. (5) **getopt convención GNU**: `optind=0` = "reset + arranca en argv[1]" (sin esto pdpmake decía `make: don't know how to make make`). (6) **`/bin/sh` = copia de busybox** (busybox dispatcha por argv[0]). (7) **🔥 Bug — sys_execve aplanaba argv en string + re-tokenizaba** rompiendo args con espacios: nueva `proc_execve_replace_argv(path, argv[], envp)` + `build_argv_block_argv` que consumen array directo. (8) **🔥 `SYS_CLONE` real** con CLONE_VM + CLONE_VFORK para musl `posix_spawn`. PML4 sharing via lookup-refcount. (9) **🔥 execve resetea `sa_handler[]` a SIG_DFL** (POSIX violation fix — antes child heredaba handlers de ash que vivían en text de busybox). (10) Verificado: `cd /home && make hello && /home/hello` end-to-end. | 800 |
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

## Inventario actual (snapshot post-FASE-14.5)

- **Kernel ELF**: ~1.6 MB stripped (`build/kernel`)
- **sd.img**: **64 MiB** FAT16, ~104 ELFs en `/bin/` + sysroot completo en `/lib/` (libc.a + crt + libtcc1.a + **libc.so + ld-musl-x86_64.so.1**) + `/usr/include/`
- **ISO bootable**: ~19 MB (`build/osnos-x86_64.iso`)
- **Memoria total esperada**: 2 GiB de RAM (`-m 2G` en QEMU)
- **Boot time**: ~3-4 segundos (kernel + spawn servers + ash banner, sin segfault al startup)
- **Tests automated**: **21/21 PASS** via `/bin/alltest` (kerntest, fork/wait/sig/sigchld/pgroup/spawn/exec/ofd/pty/fdedge/job/term/serial/tcc/lua/jq/libc/**unix/shm/hello_dyn**); cada test con timeout 60s para que un cuelgue no bloquee la suite

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

#### FASE 14.2 — AF_UNIX sockets — ✅ **CERRADA**

`socket(AF_UNIX, SOCK_STREAM)` + `bind(pathname)` + `listen` + `connect` + `accept` + `read/write/send/recv` + `close` funcionan end-to-end. Smoke test `/bin/unixtest` hace round-trip PING/PONG entre parent (server) y forked child (client) sin networking real involucrado.

Trabajo:

- ✅ **`src/include/osnos_unix_abi.h`** + **`lib/libc/include/sys/un.h`**: `sockaddr_un { sun_family, sun_path[108] }` layout-compat Linux.
- ✅ **`src/micro/unix_sock.{c,h}`** (~270 LOC): pool fijo de 32 sockets + tabla de 16 paths bound. Estados UNUSED / UNBOUND / LISTENING / CONNECTED / DISCONNECTED. Per-conn ring buffer de 4 KiB por dirección. Backlog de pending connects = 8. Sin abstract namespace ni SOCK_DGRAM.
- ✅ **OFD extendido** (`src/micro/fd.h`): nuevos campos `is_unix_socket` + `unix_idx` paralelos a `is_socket`/`sock_idx`. `ofd_clear` los resetea, `ofd_unref` cierra ambos backends.
- ✅ **Dispatch en syscalls** (`src/micro/syscall.c`): `sys_socket` rama AF_UNIX → `unix_sock_create` + fd con `is_unix_socket=true`. `sys_bind/listen/connect/accept` chequean familia y delegan. Helper `copy_un_path` valida `sockaddr_un` user-side. `sys_read/write/sendto/recvfrom` y `fd_readable` (path de `select`) también ramifican. `accept` retorna fd recién-creado, peer-side queda CONNECTED apuntando al cliente.
- ✅ **Boot init** (`src/kernel/main.c`): `unix_sock_init()` después de `pty_init`.
- ✅ **Errno extendido**: agregados `OSNOS_EISCONN=106`, `OSNOS_ENOTCONN=107` (números Linux).
- ✅ **`elfs/tests/unixtest.c`** smoke test: parent abre socket, bind, listen, fork; child connect, write "PING", read "PONG"; parent accept, read "PING", write "PONG", waitpid. Verificado: `/bin/unixtest` → `server got: 'PING'` / `client got: 'PONG'` / `unixtest: OK`.

**Out of scope**: SOCK_DGRAM (datagrams), `SCM_RIGHTS` (fd passing entre procesos via UNIX), abstract namespace (`sun_path[0]==0` Linux extension), credentials passing. Para xeyes/X11 lo que tenemos alcanza.

#### FASE 14.3 — POSIX SHM — ✅ **CERRADA**

`shm_open(name, O_CREAT|O_RDWR) + ftruncate(fd, size) + mmap(NULL, size, PROT_*, MAP_SHARED, fd, 0)` funciona end-to-end, incluyendo el caso crítico **shared memory across fork** — child y parent ven las MISMAS páginas físicas, no copias snapshot.

Trabajo:

- ✅ **`src/include/osnos_shm_abi.h`** + **`lib/libc/include/sys/mman.h`** extendida: constantes `MAP_SHARED=0x01`, `PROT_*`, `SHM_NAME_MAX=64`, declaraciones `shm_open`/`shm_unlink`.
- ✅ **`src/micro/shm.{c,h}`** (~170 LOC): pool fijo de 16 named shm objects, cada uno hasta 256 páginas = 1 MiB. Estados con `refcount + unlinked` flag (POSIX: objeto persiste hasta `shm_unlink + último close`). Operaciones: `shm_open(name, create?)`, `shm_unlink(name)`, `shm_truncate(obj, bytes)` alloca/free pages, `shm_phys_page(obj, page_off)` devuelve dir física, `shm_unref(obj)` decrementa refcount + free si zero+unlinked.
- ✅ **OFD extendido** (`src/micro/fd.h`): `is_shm + shm_ref` paralelos a is_socket. `ofd_clear` los inicializa, `ofd_unref` llama `shm_unref` al close.
- ✅ **Nuevos syscalls** (`src/micro/syscall.c` + `syscall.h`): `SYS_SHM_OPEN=519`, `SYS_SHM_UNLINK=520`. `sys_shm_open` strippea leading `/` (POSIX `/foo` == `foo`), bumps refcount, retorna fd. `sys_shm_unlink` marca el name unlinked. Strict POSIX path (Linux's `shm_open` libc también es un shim sobre open de `/dev/shm/NAME` — equivalent semantics).
- ✅ **`sys_ftruncate` dispatchea a shm**: si `f->is_shm`, llama `shm_truncate`. Sino, comportamiento legacy (vfs).
- ✅ **`sys_mmap` con MAP_SHARED fd-backed** (~50 LOC nuevos): si `flags & MAP_SHARED && fd >= 0 && is_shm`, vmm_map las páginas físicas del shm_obj en la AS del caller sin pmm_alloc. Track `shm_backed=1` en `mmap_regions` para que `sys_munmap` haga solo vmm_unmap (sin pmm_free).
- ✅ **🔥 Fix crítico en `sys_fork`**: `address_space_clone` hace deep-copy de páginas. Para regiones `shm_backed=1`, el child queda con COPIAS, no con las páginas compartidas. Fix: después del clone, walk `mmap_regions`; para cada shm region, liberar las copias del child y re-mappear al physical original del parent (= `shm_obj`'s pages). Sin esto, write del child no era visible al parent.
- ✅ **mini-libc gap-fill**: agregada `ftruncate` declaration en `<unistd.h>` + wrapper en `unistd.c` (era pendiente — sólo musl la tenía). `shm_open`/`shm_unlink` wrappers en `mman.c`. `SYS_FTRUNCATE=77` + `SYS_SHM_OPEN/UNLINK` declarados en `lib/libc/syscall.h`.
- ✅ **`elfs/tests/shmtest.c`**: parent shm_open + ftruncate + mmap + escribe "HELLO FROM PARENT" + fork; child mmap heredado, verifica leer el string, sobreescribe con "CHILD WAS HERE", _exit; parent waitpid + verifica ver el string del child. Verificado: `child: read parent's data OK / parent: saw child's write OK ('CHILD WAS HERE') / shmtest: OK`.

**Out of scope**: file-backed (non-shm) mmap de archivos regulares en disco (futuro: necesario para programas que mmappean ELFs). PROT_EXEC enforcement vía NX bit (todo mmap hoy es efectivamente RWX). Resize después de mmap (changing un shm que ya tiene mappers vivos rompe; requiere notificación a clientes via signals).

#### FASE 14.4 — Dynamic linking (.so) — ✅ **CERRADA**

`/bin/hello_dyn` linkeado dynamic-musl arranca via PT_INTERP, el dynamic linker (`ld-musl.so` que en musl ES la libc.so) resuelve printf + libc symbols contra `libc.so`, y main() corre limpio imprimiendo a stdout.

Trabajo:

- ✅ **musl rebuild con shared**: re-`./configure` sin `--disable-shared`, luego `make` produce `lib/libc.so` (la libc.so DE musl ya incluye `_dlstart` y todo el dynamic linker — no hay un binario `ld-musl.so` separado, es el mismo `libc.so` con dos roles). Link de libc.so necesitó manual fix con `ld.lld` directamente (clang chokeaba con `-Wa,--noexecstack` durante linking). Output: 882 KB ELF DYN con SONAME=libc.so y dynamic section completa.
- ✅ **Stubs compiler-rt** (`vendor/musl/build-osnos/stubs.c`): musl interno usa `__mulxc3`/`__mulsc3`/`__muldc3` (complex multiplication helpers) que normalmente vienen de libgcc.a/compiler_rt — nuestro musl build no incluye LIBCC. Sin stubs, ld-musl.so reportaba "Error relocating … symbol not found" al cargar libc.so. Stubs vacíos (apps no usan complex math) los resuelven. Linkeados al libc.so.
- ✅ **`src/proc/elf.{c,h}` extendido**: `validate_ehdr_loose` acepta ET_DYN (para el interpreter); `elf_get_interp(blob)` parsea PT_INTERP y devuelve el path string; `elf_load_dyn(main, interp, ...)` carga AMBOS blobs en el mismo PML4 — main en sus p_vaddr originales (ET_EXEC), interpreter offseteado a `INTERP_LOAD_BASE=0x40000000`. Devuelve `elf_load_result_t` con: e_entry del main, start_entry (= interpreter entry+base si hay interp, else main entry), phdr_user_va (para AT_PHDR), phnum, phentsize, interp_base.
- ✅ **Auxv extendido** (`src/proc/exec.c` `build_argv_block_tokens` ahora toma `const elf_load_result_t *aux_info`): para el path estático sigue emitiendo solo `AT_PAGESZ + AT_NULL` (32 bytes). Para el path dinámico emite **8 pares**: `AT_PHDR=3`, `AT_PHENT=4`, `AT_PHNUM=5`, `AT_PAGESZ=6`, `AT_BASE=7`, `AT_ENTRY=9`, `AT_RANDOM=25` (apunta a 16 bytes plantados en el área de strings), `AT_NULL=0`. ld-musl.so necesita TODOS estos para parsear y reubicar el main + libs.
- ✅ **`proc_execve_replace_argv` wirea PT_INTERP**: detecta PT_INTERP en el blob principal, lee el interp ELF del VFS (`/lib/ld-musl-x86_64.so.1`), llama `elf_load_dyn`, usa `build_argv_block_argv_dyn` con el result. Path estático (sin PT_INTERP) sigue usando `elf_load` legacy.
- ✅ **sd.img bump 32→64 MiB**: las dos copias de libc.so (~1.7 MiB combined) + busybox (1.3 MiB) + binarios existentes saturaban el 32 MiB. 64 MiB con `-c 8` (4 KiB clusters) = 16384 clusters, bajo el FAT16 max de 65525.
- ✅ **GNUmakefile stage**: `mcopy` libc.so a `/lib/libc.so` Y a `/lib/ld-musl-x86_64.so.1` (FAT16 no tiene symlinks; el path en PT_INTERP es lo que importa). Nueva regla `$(BUILD)/elfs/tests/hello_dyn.elf` linkea dynamic: `ld -m elf_x86_64 -nostdlib -no-pie -z noexecstack --dynamic-linker=/lib/ld-musl-x86_64.so.1 --hash-style=both --allow-shlib-undefined crt1.o crti.o app.o libc.so crtn.o`.
- ✅ **`elfs/tests/hello_dyn.c`** smoke test: `printf("hello from dynamic linker on osnos!\n")`. Verificado: `/bin/hello_dyn` → `hello from dynamic linker on osnos!` / `argc=1 argv[0]='/bin/hello_dyn'`.

**Test integral FASE 14.1-14.4**: `make hello && /home/hello` ✓; `shmtest: OK` ✓; `unixtest: OK` ✓; `hello_dyn` ✓; `sqlite3 SELECT 7*8 → 56` ✓; `lua print(1+2+3) → 6` ✓.

**Out of scope**: PIE main (ET_DYN executable cargado con load offset random). RTLD_LAZY (lazy bind via PLT trampolines — hoy todo se relocata eagerly). dlopen/dlsym (musl los expone via libc.so pero no probamos cargar libs dynamic adicionales). Multiple .so deps (DT_NEEDED transitivo).

#### FASE 14.5 — lighttpd 1.4.76 port (real HTTP server) — ✅ **CERRADA**

`curl http://localhost:8080/index.html` → `HTTP/1.1 200 OK` con headers completos y body servido desde `/home`. lighttpd compila + bind:80 + accept + read request + serve static + close — todo el path HTTP funciona.

Trabajo:

- ✅ **`vendor/lighttpd/`** — lighttpd 1.4.76 (124 .c files, ~106K LOC). No corremos su build system (autotools/cmake); en su lugar **hand-craft** del build:
  - **`vendor/lighttpd/build-osnos/config.h`** — 30 `HAVE_*` defines que coinciden con musl en osnos. Excluido todo lo grande (epoll, kqueue, SSL, PCRE, zlib, IPv6, posix_spawn, sendfile, inotify, brotli/zstd/deflate, lua plugins). `LIGHTTPD_STATIC` para link estático sin dlopen.
  - **`vendor/lighttpd/build-osnos/plugin-static.h`** — hand-crafted PLUGIN_INIT macros para 10 built-in mods incluidos (mod_indexfile, mod_staticfile, mod_access, mod_alias, mod_setenv, mod_expire, mod_redirect, mod_simple_vhost, mod_evhost, mod_rewrite).
  - **lemon parser**: compilamos `vendor/lighttpd/src/lemon.c` en host, lo usamos para generar `configparser.c` desde `configparser.y` (~85 KB output).
  - **fdevent backend = poll** (no epoll): `HAVE_POLL + HAVE_SYS_POLL_H` defined, lighttpd's `fdevent_impl.h` selecciona `FDEVENT_USE_POLL` automáticamente. `strings lighttpd.elf | grep poll` confirma poll path elegido en runtime.
- ✅ **`GNUmakefile` recipe**: 52 source files compilados con MUSL_CFLAGS + lighttpd config. Output `/bin/lighttpd` ~1.85 MB ELF estático.
- ✅ **🔥 Kernel: `sys_read`/`sys_write` dispatch a AF_INET sockets**: era una omisión existente — `sys_read(fd)` solo soportaba pipe/PTY/file/AF_UNIX; AF_INET caía al path VFS y devolvía EINVAL. `/bin/httpd` viejo funcionaba porque usaba `sendto`/`recvfrom` directo. lighttpd usa `read`/`write` (standard POSIX para stream sockets) y necesitaba el dispatch. Fix: ramos `sock_recv`/`sock_send` también en sys_read/sys_write.
- ✅ **Kernel: `sys_setsockopt` permisivo**: antes solo aceptaba `SO_REUSEADDR`. Ahora acepta como no-op success cualquier flag bajo `SOL_SOCKET`, `IPPROTO_TCP`, `IPPROTO_IP` (suficiente para que `TCP_NODELAY` etc. no aborten lighttpd al startup). Real implementación queda pendiente; el no-op alcanza para HTTP serving.
- ✅ **`res/lighttpd/lighttpd.conf`**: config mínimo (server.document-root=/home, server.port=80, server.modules=mod_indexfile+mod_staticfile, MIME types comunes, errorlog=/home/lighttpd.log para inspección post-mortem).
- ✅ **`/etc/lighttpd/lighttpd.conf`** + **`/home/index.html`** seeded en sd.img.
- ✅ **End-to-end verificado**: corremos `/bin/lighttpd -f /etc/lighttpd/lighttpd.conf -D` foreground; desde el host `curl http://localhost:8080/` (via QEMU hostfwd) recibe `HTTP/1.1 200 OK` + body. Múltiples paths probados (`/index.html`, `/hello.c`, `/demo.sql`).

**Limitaciones**: lighttpd en background con `&` falla por `sh: can't open '/dev/null'` (busybox redirect path raro — no es lighttpd). Workaround actual: correr foreground o usar `osn_spawn` desde otro proceso. `server.upload-dirs` requiere directorio en FAT16; `/home` funciona, `/tmp` no porque no hay tmpfs mount. PHP/CGI/FastCGI no compilados (necesitarían fork+execve+pipe roundtrips, todo funcionaría pero scope creep).

#### FASE 14.6 — Ox extendido o nano-X (pendiente)
Tres caminos posibles, decisión abierta:
- **A** — xeyes-via-Ox: cliente nativo Ox que dibuja dos círculos siguiendo al cursor. ~150 LOC. Demuestra que la infra GUI ya alcanza sin meter X11.
- **B** — Vendorizar nano-X (~20K LOC) sobre FBDEV. Abre API Xlib-like real. 1-2 sesiones.
- **C** — X11 wire protocol mínimo bind a `/tmp/.X11-unix/X0` (AF_UNIX ya tenemos), traduce a Ox. Permite xeyes Linux unmodified. Múltiples sesiones (spec X11 enorme).

#### FASE 14.7 — `xeyes` (test del camino completo)
Depende de 14.6 (B o C).

#### FASE 14-misc — Quality of life menores — ✅ **CERRADA**

Sesión "de un saque": 8 items resueltos sin regresiones. `alltest` sigue **21/21 PASS**. Verificado end-to-end con tests integrales de FASE 14.1-14.5.

- ✅ **Per-PTY termios real**: agregados `task_t.tty_termios_valid + tty_iflag/oflag/cflag/lflag/line/cc[19]`. `sys_ioctl TCGETS` para fd 0/1/2 snapshot del global al primer call + return de task's struct. `TCSETS/TCSETSW/TCSETSF` actualizan task's struct + sync al global. En task switch (`task_run_next`), si task entrante tiene `tty_termios_valid=1`, restaura via `tty_restore_from(struct)`. fork copia parent's struct. Cada task ahora "ve" su propio modo raw/canon/echo al ser dispatched. (`src/micro/task.{c,h}`, `src/micro/tty.{c,h}`, `src/micro/syscall.c`)
- ✅ **sqlite3 argv passing**: ya estaba resuelto por el fix `sys_execve preserves argv boundaries` (FASE 14.1). Verificado: `sqlite3 /home/demo.db "SELECT title FROM books WHERE year > 1980 ORDER BY year LIMIT 3"` devuelve 3 filas correctas con exit=0.
- ✅ **Page fault en musl atexit (sqlite3 exit limpio)**: ya estaba resuelto por los fixes acumulados (FS_BASE rdmsr, kill_pending catchable, sa_handler reset). Verificado: 3 invocaciones de sqlite3 seguidas exitcode=0 sin page fault.
- ✅ **BusyBox aplicación masiva: 116 applets totales (era 65)**: enabled `.config` + fixeado el wrapper `osnos-cc-wrapper.sh` (filtra `-Wl,-Map,*`, `--warn-common`, etc. que ld.lld rechaza). Rebuild produce binary de 1.45 MB. Aliases agregados a `/home/.ashrc`. **51 nuevos applets**: networking (`wget`, `nc`, `ping`, `traceroute`, `ifconfig`, `netstat`, `route`, `arp`, `hostname`, `telnet`, `microcom`, `nslookup`, `ftpgetput`); archives (`tar`, `gzip`, `gunzip`, `zcat`, `bzip2`, `bunzip2`, `bzcat`, `xz`, `unxz`, `xzcat`, `ar`, `lzma`, `unlzma`); fs/perms (`chmod`, `chown`, `chgrp`, `ln`, `mkfifo`, `mknod`, `mktemp`, `mountpoint`, `sync`, `fsync`, `truncate`, `install`, `chroot`); process/user (`id`, `whoami`, `groups`, `who`, `users`, `tty`, `pidof`, `pgrep`, `pkill`, `watch`, `setsid`, `nice`, `nohup`, `nproc`, `time`, `last`); text/filter (`nl`, `od`, `split`, `comm`, `paste`, `join`, `fmt`, `expand`, `unexpand`, `shuf`, `yes`, `less`, `ed`, `uuencode`, `uudecode`, `ipcalc`). **Stubs syscall agregados** en syscall.c para `getpriority/setpriority`, `sched_setparam/get`, `sched_setscheduler/get`, `sched_yield`, `setrlimit/getrlimit`, `prctl`, `setresuid/gid`, `setuid/gid`, `sync` — todos retornan 0 (single-task no priority/perms). Sin estos stubs, `nice -n 5 echo hi` daba ENOSYS. Verificado: `nice -n 5 echo hi` → `hi`, `pgrep -l bus` → `6 busybox`, `whoami` → `root`, `id` → `uid=0(root) gid=0(root)`, `nproc` → `1`, `nl` numera líneas, `gzip|gunzip|wc` roundtrip 12 bytes, `paste a.txt b.txt` tab-joined correcto.
- ✅ **`/proc` synthetic filesystem** (`src/fs/procfs.{c,h}`, ~420 LOC): mount en `/proc`. Top-level: `meminfo` (PMM stats), `uptime`, `loadavg`, `cpuinfo`, `stat`, `version`. Per-pid: `/proc/<pid>/{cmdline,comm,stat,status}` enumerando task table. `/proc/self` alias del task actual. **`/proc/net/{dev,route,tcp,udp}`** para que `route -n`, `netstat -tan`, `ifconfig` (parcial) puedan leer la net info. **🔥 Bug fixed**: trailing-slash form `/proc/<pid>/` también devuelve PROC_PID_DIR. Verificado: `cat /proc/meminfo` muestra MemTotal=2096480 kB, `top` muestra 8 procesos, `route -n` muestra default via 10.0.2.2.
- ✅ **`/etc/resolv.conf` seeded** en sd.img con `nameserver 10.0.2.3` (QEMU slirp DNS) + fallback 8.8.8.8. `/etc/hosts` extendido con `10.0.2.2 host`. Apps que leen el resolver config ahora encuentran lo necesario. (DNS sobre el wire requiere más kernel network stack work — los apps DNS-dependientes como `nslookup`/`ping <hostname>`/`wget <hostname>` necesitan UDP outbound contra slirp que todavía no anda 100%.)

#### FASE 14-misc-2 — Network ioctls SIOCGIF* — ✅ **CERRADA**

`ifconfig` ahora muestra info completa de eth0 + lo. Para apps Linux que enumeran interfaces via ioctl.

- ✅ **`net_iface_ioctl()`** en `sys_ioctl` (`src/micro/syscall.c`, ~160 LOC): handler para todo el rango `0x8910-0x8950` (Linux SIOCG*/SIOCS*) cuando se llama sobre socket fd (AF_INET o AF_UNIX). Maneja:
  - **SIOCGIFCONF=0x8912**: enumera interfaces (`lo`, `eth0`) en formato `struct ifreq[]`. Devuelve `ifc_len` total para 2 interfaces (80 bytes), o si user pidió size=0 devuelve needed size.
  - **SIOCGIFADDR/NETMASK/BRDADDR=0x8915/891b/8919**: devuelve IP/máscara/broadcast en `sockaddr_in` embebido. eth0 toma de `net_local_ip()` + `net_local_netmask()`, lo es 127.0.0.1/255.0.0.0.
  - **SIOCGIFHWADDR=0x8927**: MAC desde `net_local_mac()` (RTL8139 driver). lo es 00:00:00:00:00:00.
  - **SIOCGIFFLAGS=0x8913**: emite flags Linux (IFF_UP=1, IFF_BROADCAST=2, IFF_LOOPBACK=8, IFF_RUNNING=64, IFF_MULTICAST=0x1000). eth0=BROADCAST|RUNNING|MULTICAST|UP; lo=LOOPBACK|RUNNING|UP.
  - **SIOCGIFMTU=0x8921**: 1500 para eth0, 65536 para lo.
  - **SIOCGIFINDEX=0x8933**: 1=lo, 2=eth0.
  - **SIOCSIF\* (set variants)**: retornan EPERM — no permitimos reconfigurar via ioctl.
- ✅ **BusyBox `.config`**: enabled `FEATURE_IFCONFIG_STATUS`, `FEATURE_IFCONFIG_HW`, `FEATURE_IFCONFIG_BROADCAST_PLUS`, `FEATURE_NETSTAT_WIDE`, `FEATURE_NETSTAT_PRG`. Rebuild de busybox.
- ✅ **Verificado end-to-end**:
  ```
  osnos:/# ifconfig -a
  eth0  Link encap:Ethernet  HWaddr 52:54:00:12:34:56
        inet addr:10.0.2.15  Bcast:10.0.2.255  Mask:255.255.255.0
        UP BROADCAST RUNNING MULTICAST  MTU:1500
  lo    Link encap:Ethernet  HWaddr 00:00:00:00:00:00
        inet addr:127.0.0.1  Mask:255.0.0.0
        UP LOOPBACK RUNNING  MTU:65536
  ```
- ✅ alltest 21/21 PASS sin regresiones.
- ✅ **`siginfo_t` real para SA_SIGINFO** (`src/proc/exec.c`): 128 bytes plantados en user stack ENCIMA del sigframe (`siginfo_va = (orig_rsp - 128) & ~15`). Populamos `si_signo + si_errno=0 + si_code=0`. Handler recibe `rsi = siginfo_va`. sys_rt_sigreturn sin cambios. Apps SA_SIGINFO-aware (lighttpd, postgres, sshd) ahora reciben pointer válido en vez de NULL.
- ✅ **`/dev/stderr` + `/dev/stdin`/`/dev/stdout`/`/dev/console`**: 4 entradas más en `devfs`. Backend delega a `tty_dev_read/write` (mismo path que `/dev/tty`). Apps Linux que abren `/dev/stderr` para logs ahora funcionan.
- ✅ **tmpfs en `/tmp`**: `vfs_mount("/tmp", &ramfs_vfs_ops, 0)` en bootstrap. Reusa el backend ramfs pero longest-prefix dispatch envía `/tmp/*` aquí. Verificado: `echo "test" > /tmp/test.txt && cat /tmp/test.txt` → "test".

#### FASE 14-misc-3 — Networking real (DNS resuelve, ping responde) — ✅ **CERRADA**

Punto de partida: `nslookup google.com 10.0.2.3` decía `can't resolve` y `ping <ip>` no existía como ELF runnable. Después de esta fase: `nslookup google.com → 142.251.x.x` y `ping 8.8.8.8 → 64 bytes from 8.8.8.8 ttl=255 time=30ms`. 21/21 alltest siguen PASS.

Tres bugs encadenados rompían el resolver de musl, más SOCK_RAW que no existía:

- ✅ **`sys_socket` acepta `SOCK_CLOEXEC` + `SOCK_NONBLOCK` bundled en `type`**: Linux x86_64 permite `socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)` empaquetando flags (0x80000 + 0x800) en el segundo arg. musl `res_msend.c:123` usa este patrón. Nuestro `sys_socket` rejectaba con `EAFNOSUPPORT` porque `type != SOCK_DGRAM`. Fix (`src/micro/syscall.c`): `type = type_raw & 0xff`; flags se aplican post-creación al fd (`OSNOS_FD_CLOEXEC`, `f->flags |= 0x800`). Sin este fix, `getaddrinfo` no podía abrir el socket UDP y todo el resolver moría con EAFNOSUPPORT.
- ✅ **`sys_recvfrom` UDP path preserva `src_ip`/`src_port`**: la implementación viejo llamaba `sock_recv()` primero, que para UDP delega a `sock_recvfrom` pero **descarta** el peer en variables locales, y después seteaba `*alenp=0`. Resultado: musl recibía el datagram con `from = 0.0.0.0:0` y rechazaba el reply porque no matcheaba el nameserver configurado en `/etc/resolv.conf` (`res_msend.c:216-217`: `for (j=0; j<nns && memcmp(ns+j, &sa, sl); j++); if (j==nns) continue;`). Fix: añadir `sock_type(int sd)` accessor en `socket.{c,h}`, y en `sys_recvfrom` ramificar **antes** del path stream — para SOCK_DGRAM/SOCK_RAW ir directo a `sock_recvfrom` y populate la sockaddr_in.
- ✅ **`recvmsg(2)` + `sendmsg(2)` implementados** (Linux syscalls 46/47): musl `res_msend` usa `recvmsg` (no `recvfrom`) para leer replies DNS. Sin la syscall, returnaba ENOSYS y el resolver hacía giveup silencioso. Implementación single-iovec (suficiente para resolver/wget): parse `struct msghdr` + `struct iovec[1]`, copy_from_user del payload, delegate a `sys_sendto`/`sock_recvfrom`, copy_to_user del `msg_name` + nuevo `msg_namelen`. Stubs adicionales: `getsockname`/`getpeername` (retornan addrlen=0), `getsockopt` (retorna 0 byte para SO_ERROR — el caso post-connect común). Defines nuevos: `SYS_SENDMSG=46 SYS_RECVMSG=47 SYS_GETSOCKNAME=51 SYS_GETPEERNAME=52 SYS_GETSOCKOPT=55` en `syscall.h`.
- ✅ **`SOCK_RAW` + ICMP echo para `ping`**: nueva familia de socket. Cambios:
  - `socket.h`: `OSNOS_SOCK_RAW=3` (Linux-compat); `sock_create_raw(int proto)`, `sock_raw_sendto`, `sock_raw_deliver`.
  - `socket.c`: campo `int protocol` en `sock_t`. `sock_create_raw` similar a sock_create pero type=RAW + protocol stashed. `sock_raw_sendto` llama directo a `ip_send(dst_ip, s->protocol, payload, len)` — kernel arma IP header, user-mode arma ICMP. `sock_raw_deliver(proto, ip_packet, len, src_ip)` itera el pool buscando matches; entrega el **paquete IPv4 entero** (header + payload, Linux behavior). `sock_readable` ahora trata RAW como DGRAM (rx_count > 0). `sock_recvfrom` permite `local_port == 0` para RAW.
  - `ip.c`: después del dispatch normal (icmp_handle / udp_handle / tcp_handle), llama a `sock_raw_deliver(protocol, data, total_len, src_ip)`. Esto da el mirror al raw socket pool sin romper el path ICMP echo-reply estándar que sigue contestando los pings entrantes.
  - `syscall.c` `sys_socket`: acepta `OSNOS_SOCK_RAW` → `sock_create_raw(protocol)`. `sys_sendto` para RAW unpacka sockaddr_in y llama `sock_raw_sendto`. `sys_recvfrom`/`sys_recvmsg` ya routean RAW via `sock_type()`.
- ✅ **BusyBox `FEATURE_FANCY_PING=y`** en `.config`: sin esto, ping solo soporta single-shot (sin `-c N`, sin RTT en ms, sin pretty-print). Rebuild.
- ✅ **Test ELFs diagnósticos**: `elfs/tests/udp_send.c` (sendto/recvfrom DNS query directo) + `elfs/tests/udp_connect.c` (mimics nslookup: socket+bind+connect+fcntl O_NONBLOCK+write+read). Útiles para aislar bugs entre kernel UDP path y musl resolver.
- ✅ **Verificado end-to-end**:
  ```
  osnos:/# nslookup google.com 10.0.2.3
  Server:    10.0.2.3
  Name:      google.com
  Address 1: 142.251.129.174 gru14s32-in-f14.1e100.net
  osnos:/# ping 8.8.8.8
  PING 8.8.8.8 (8.8.8.8): 56 data bytes
  64 bytes from 8.8.8.8: seq=0 ttl=255 time=20.000 ms
  osnos:/# udp_send                 # diag: kernel UDP roundtrip
  sent 28 bytes
  got 44 bytes from 10.0.2.3:53
  ```
- ✅ alltest 21/21 PASS sin regresiones.

**Limitaciones conocidas**:
- `ping -c N` multi-packet a veces se queda en seq=0: la pacing entre pings (BusyBox usa `setitimer` + SIGALRM) probablemente no respeta nuestro intervalo. Single-shot funciona perfecto; multi-packet necesita revisar el path `setitimer`.
- `wget http://example.com/` resuelve DNS pero falla en TCP connect con "Operation in progress" — non-blocking `connect()` returnando EINPROGRESS sin completar después. Out-of-scope para esta fase.
- IPv6 sin soporte (musl pide AAAA, devuelve `Address 2: (null)` benigno).
- `sendmsg`/`recvmsg` solo single-iovec; multi-iovec no implementado.

### FASE 14-pendings — Quality of life remaining
- ❌ Chip-8 emulator (último item pendiente del roadmap original gráfico)
- ❌ `setitimer` real (hoy es stub) — bloquea `ping -c N` y muchos timer-based loops.
- ❌ Non-blocking `connect()` real con `EINPROGRESS` + completion via poll — bloquea wget/curl HTTP outbound.

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
