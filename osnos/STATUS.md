## Estado actual

**Convención de formato** (este documento es histórico, ordenado de
nuevo a viejo en la tabla; las secciones por subsistema son
acumulativas):
- ✓  cerrado / hecho / verificado
- ☐  pendiente / TODO
- **Fase X — CERRADA** = fase entera terminada
- **Fase X — PENDIENTE** = fase futura, plan documentado

### Resumen ejecutivo

OSnOS hoy es un kernel x86_64 hobby con ring-3 ELFs, scheduler
preempt, FAT16 persistente, stack TCP/IP completo, line discipline
POSIX, shell con history + rc files, **ABI POSIX core 100%
completo** (fork + execve + wait/waitpid + sigaction + EINTR +
SIGCHLD automático + process groups + sessions + OFD shared offsets
+ FD_CLOEXEC + PTY pairs + WUNTRACED/WCONTINUED + fan-out Ctrl+C/Z
a fg pgid), **mini terminal emulator funcional** (`/bin/term`
spawnea sub-shell interactivo `/bin/minishell` en PTY),
**consola serial dual-console + `/dev/tty`** (UART 16550 en COM1
con framebuffer + serial en paralelo, `cat foo | less` real,
headless boot para CI vía `./build_and_run.sh headless`), y —
desde **FASE 11.0** — **self-hosting tier**: `/bin/tcc`
(TinyCC 0.9.27 portado) compila programas C desde dentro de osnos
contra `/lib/libc.a` + `/usr/include/`, produce ELFs estáticos
runnable. **FASE 11.1** trajo offset-native VFS reads + true FAT
append (rompió 2 caps silenciosos de 1KB/8KB) + BUFSIZ 4KB +
FAT-sector cache → compile time de TCC pasó de "tarda mucho" a
"instantáneo". **FASE 11.2** portea **Lua 5.4.7** como segundo
lenguaje self-host: `lua` REPL interactivo + ejecución de scripts.
**FASE 11.3** portea **jq 1.7.1** como tercer lenguaje: filter
+ transformer de JSON con language completo (paths, builtins,
arithmetic, pipes funcionales). `tcc hello.c -o hello && ./hello`,
`lua script.lua`, y `cat data.json | jq '.field'` todos
funcionan end-to-end. **FASE 11.4** suma input pointer:
driver PS/2 mouse + `/dev/mouse0` char device (ring de 32
`mouse_event_t`) + `/bin/mousetest` que muestra `dx/dy/buttons`
en vivo — habilitó la línea gráfica. **FASE 12.0** materializa
la GUI: **Ox window system** completo — server `/bin/oxsrv` con
ioctls nuevos `FBIOGET_VSCREENINFO` + `FBIO_BLIT` en `/dev/fb0`
para pixel access, 5 apps GUI (oxnotepad / oxcalc / oxterm /
oxfiles / oxsettings), wallpaper picker, root menu Openbox-style
via right-click, coexistencia limpia con consrv/kbdsrv via nuevos
opcodes `IPC_CONSOLE_SUSPEND/RESUME` + `IPC_KEYBOARD_SUSPEND/
RESUME`. **FASE 12.1** pulió la UX: `/bin/uxsh` mini-shell para
oxterm (cd/pwd/exit + fork-exec), oxnotepad acepta path via argv,
parser ANSI completo en oxterm (SGR truecolor + cursor pos +
erase), libc stdio EAGAIN retry (output largo ya no se trunca),
watchdog auto-resume en consrv/kbdsrv si oxsrv desaparece. **FASE
13.0** trae **musl 1.2.5 opt-in** como segunda libc: vendoreado
clean, kernel agrega `SYS_WRITEV=20` + `SYS_ARCH_PRCTL=158` (TLS
via wrmsr MSR_FS_BASE) + `SYS_SET_TID_ADDRESS=218`, `build_argv_
block` agrega auxv mínimo. ELFs opt-in via `USER_ELF_MUSL_SRCS`;
`hello_musl` smoke test pasa con `snprintf %f` real (que la mini-
libc no soporta). **FASE 13.1** sube **BusyBox 1.36.1 linkeado a
musl como init shell**: `proc_execve("/bin/busybox", "sh -l", envp)`
con `PATH=/bin HOME=/home HISTFILE=/home/.ash_history TERM=linux`.
ash en login mode sourcea `/etc/profile` (exports only, mirror exacto
de Linux), que setea `$ENV=/home/.ashrc` (sourced en cada shell
interactiva, mirror de ~/.bashrc). El `.ashrc` trae el banner ASCII
"OSnOS", PS1 verde `osnos:\w# ` y aliases (`ll la l .. h cls`).
Cerró 4 bugs de kernel para que ash sobreviva: (a) **restart_syscall pattern**
en `sys_read`/`sys_poll` (rebobina iret RIP 2 bytes en lugar de
longjump-con-rax=0 que ash interpretaba como EOF y causaba respawn
loop infinito); (b) **syscall numbers osnos-specific movidos
260-268 → 510-518** para no chocar con Linux x86_64 #262=newfstatat
(musl `stat()` invocaba SERVICE_REGISTER por error); (c) `sys_stat`
copia path **byte-a-byte hasta NUL** en vez de blast de 128 B (que
faulteaba en paths cortos al final de página); (d) `VFS_MAX_MOUNTS`
8 → 16 (con /home era el noveno mount). Nuevos mappings: `SYS_LSTAT
=6`, `SYS_OPENAT=257`, `SYS_NEWFSTATAT=262`, `SYS_EXIT_GROUP=231`.
Verificado: `ls /etc`, `for i in a b c`, `echo $((100*7))=700`,
pipes, redir, glob, `cat /home/* > out`, todo posix. Todo verificado
por **18/18 tests automatizados** vía `/bin/alltest`. Bonus:
`/bin/poweroff` + `/bin/reboot` (ACPI S5 / 8042 reset), `tail -f`
poll-based, `/bin/readelf -a/-l/-S/-h` para inspeccionar binarios.

**Fases cerradas (orden cronológico inverso):**

| Fase | Subsistema | Líneas (≈) |
|------|-----------|------------|
| **FASE 13.1 — BusyBox ash + login mode + .bashrc-style /home/.ashrc** | (1) **`vendor/busybox/`** — BusyBox 1.36.1 vendored, linkeado contra musl 1.2.5 via `osnos-cc-wrapper.sh` (clang frontend → `ld.lld -m elf_x86_64 -static -no-pie --gc-sections crt1.o ... libc.a`). Resulting `busybox_unstripped` ~1.4 MB con ~30 applets enabled: `sh ash echo cat ls ls find sed grep cut tr sort uniq head tail wc cp mv rm mkdir touch chmod md5sum expr cksum cpio basename dirname date dd du tee env pwd which printf seq yes test true false sleep less env uname`. (2) **BUG CRÍTICO #1 — restart_syscall pattern**: `sys_read` y `sys_poll` originalmente loopeaban con `sys_nanosleep()` para esperar input; pero `sys_nanosleep` hace `sched_resume_jump()` (longjmp al scheduler, NEVER returns) y deja al task con `saved_rax=0` apuntando al RIP user-space DESPUÉS del syscall. Resultado: ash llamaba `read(0)`, kernel longjumpeaba, ash recibía `read=0` que interpreta como EOF → exit(0) → watchdog respawn → infinite loop (52+ banners/30s). Fix: nuevo helper `block_restart_syscall(wakeup_ms, syscall_nr)` que stamp el iret frame con `rip -= 2` (rebobina 2 bytes — coincidente para `syscall 0F 05` y `int 0x80 CD 80`) + `saved_rax = syscall_nr` (SYS_READ=0, SYS_POLL=7). Al despertar la CPU re-ejecuta el syscall original con args originales en rdi/rsi/rdx → chequea readiness de nuevo. Esto es el patrón POSIX `restart_syscall` en kernel — sin él, libcs que no auto-retrytan en EAGAIN (musl, fopen-via-write libs) fallan inmediato. (3) **BUG CRÍTICO #2 — colisión de syscall numbers**: osnos-specific syscalls vivían en 260-268 (`SYS_IPC_SEND/RECV/SERVICE_REGISTER/LOOKUP/TTY_INPUT/TASKINFO/SPAWN/SET_FG/RESUME`) — chocaban con Linux x86_64 syscalls 260=fchownat, **262=newfstatat (que musl `stat()` invoca)**, 263=unlinkat, etc. musl `stat("/")` retornaba lo que respondía SERVICE_REGISTER → mode bits basura → ls/find/cp todos rotos. Movidos a 510-518 (kernel `src/micro/syscall.h` + libc `lib/libc/syscall.h` + `lib/libc/include/osnos_ipc.h`). Nuevos mappings agregados: `SYS_LSTAT=6` (alias de sys_stat, osnos no tiene symlinks), `SYS_OPENAT=257` (sys_open con dirfd ignorado), `SYS_NEWFSTATAT=262` (sys_stat con dirfd ignorado), `SYS_EXIT_GROUP=231` (alias de sys_exit). (4) **BUG CRÍTICO #3 — `sys_stat` faulteaba con paths cortos**: `copy_from_user(kpath, path, OSNOS_PATH_MAX)` pedía 128 bytes a partir de `path`, faulteando para strings al final de página como `"/"`. ls llamaba stat("/") → EFAULT → "cannot open /". Fix: copia byte-a-byte hasta NUL terminator. (5) **BUG CRÍTICO #4 — `VFS_MAX_MOUNTS=8` insuficiente**: con `/, /sys, /dev, /sd, /bin, /lib, /usr, /etc, /home` sumamos 9 mounts — `/home` no entraba y `find_mount("/home")` devolvía NULL. Bumpado a 16. (6) **Login shell + split /etc/profile + /home/.ashrc** (estilo `.bashrc`): `proc_execve("/bin/busybox", "sh -l", shell_envp)` con `shell_envp = {PATH=/bin, HOME=/home, HISTFILE=/home/.ash_history, HISTSIZE=500, TERM=linux}` (HISTFILE en envp porque ash lo lee ANTES de cmdloop, donde se sourcearía /etc/profile — fix de un bug que dejaba la HISTFILE no-detectada). Watchdog `server_respawn_tick` usa el mismo envp. Bootstrap seedea **dos archivos separados, mirror exacto del convention bash**: (a) `/etc/profile` (sourced ONCE on login) — solo `export PATH/HOME/HISTFILE/HISTSIZE/TERM` + `export ENV=/home/.ashrc`. NO banner, NO PS1 — esos son interactive concerns. (b) `/home/.ashrc` (sourced en CADA shell interactiva via $ENV, igual que ~/.bashrc) — `export PS1='osnos:\w# '`, aliases (`ll`, `la`, `l`, `..`, `h`, `cls`), `/bin/banner osnos`, mensaje de bienvenida. **Bug fixed**: el seed de `/home/.ashrc` originalmente estaba en el bloque /etc del bootstrap antes de que el aliasfs `/home → /sd/home` estuviera montado → escribía a ramfs sin efecto. Movido al bloque /home post-mount. **Bug fixed**: el `/etc/profile` y `$ENV` apuntaban al MISMO archivo (banner duplicado en cada boot). Split lo elimina — banner aparece EXACTAMENTE 1 vez por shell. Resultado: cada boot muestra el ASCII art "OSnOS" + welcome + prompt verde `osnos:/#`. Usuario puede editar `/home/.ashrc` con `ovi /home/.ashrc` sin recompilar nada. (7) **History line-editing**: `FEATURE_EDITING=y` está en la `.config` del binario actual, así que **up/down arrow recall in-memory funciona out-of-the-box**. **Limitación conocida**: el binario actual de busybox tiene `FEATURE_EDITING_SAVEHISTORY=n` (off en .config original), así que `/home/.ash_history` se setea como HISTFILE pero el archivo nunca se escribe — history NO persiste cross-reboot. Habilitar persistencia requiere rebuild de busybox con SAVEHISTORY=y; varios intentos chocaron con incompatibilidades de cross-compile (clang on macOS detecta `__APPLE__` → busybox toma branch BSD que necesita `<machine/endian.h>`, y musl headers tienen otros conflictos modutils/__NR_*). Deferred a FASE 13.2. (8) **Smoke verificado end-to-end**: `echo HELLO`, `echo arith=$((100*7))` → 700, `for i in a b c; do echo $i; done` → a/b/c, `ls /` → sys/dev/sd/bin/lib/usr/etc/home/, `ls /etc` → passwd group hosts (vía aliasfs), `cat /home/README.TXT`, `echo hi > /home/test.txt` (persiste en FAT16), `ls /bin \| head -n 5`, `tr a-z A-Z`, `sort`, `wc -l`, `uname -srm`. Banner aparece al boot. Default shell de osnos pasa de `shellsrv` (interno) a `busybox sh` (POSIX-compliant). `shellsrv` sigue disponible como fallback si `/bin/busybox` falta (diskless boot). | 800 |
| **FASE 12.1 — Polish UX GUI + watchdog + ANSI completo** | (1) **`/bin/uxsh`** (`elfs/tools/uxsh.c`, ~140 LOC libc-linked): mini-shell para spawn dentro de oxterm. Builtins `cd` (with relative path), `pwd`, `clear` (CSI), `help`, `exit`. Cualquier comando que no sea builtin → fork + execve (`/bin/<name>` si no es absolute). `[exit N]` cuando child sale != 0. (2) **oxnotepad acepta argv[1]**: el path se pasa por `osn_spawn(path, "/home/foo.txt", ...)` desde oxfiles, parse en `main()`, title bar muestra `Notepad — /home/foo.txt`. Default sigue siendo `/home/notepad.txt` si no hay argv[1]. (3) **`/bin/oxfiles`** (`elfs/gui/oxfiles.c`, ~210 LOC): file browser. `opendir(cwd)` + stat para detectar dirs. Click sobre dir → cd; click sobre `.ppm` en `/home/wallpapers/` → setea wallpaper via `.oxrc` + `IPC_OX_RELOAD_SETTINGS`; click sobre otro file → spawn `/bin/oxnotepad <full_path>`. Hover highlight, header bar con cwd, `..` siempre visible salvo en `/`. (4) **Parser ANSI completo en oxterm** (`elfs/gui/oxterm.c`, +140 LOC): state machine `ST_NORMAL → ST_ESC → ST_CSI → final_byte`. Soporta: `ESC[H/f`/`ESC[r;cH` cursor pos absoluta, `ESC[A/B/C/D N` movimiento relativo, `ESC[J`/`[1J`/`[2J` erase display, `ESC[K`/`[1K` erase line, `ESC[m` SGR (reset, reverse 7/27, fg 30-37/90-97, bg 40-47/100-107, **truecolor 38;2;R;G;B + 48;2;...**). Grid de cells `{ch, fg, bg}` en vez de chars planos. Render optimizado: agrupa runs de bg/fg iguales en single draw_rect/draw_text. Cursor amarillo en bottom de cell (era bloque verde). (5) **libc stdio retry on EAGAIN** (`lib/libc/stdio.c`): `drain_write` retry'ea hasta 200 × 1ms en EAGAIN antes de bail con error. Output de comandos largos (TCC compilando, `cat big.txt | less`) ya no se trunca silente cuando el sink (PTY ring, pipe) está saturado. (6) **Watchdog auto-resume en consrv + kbdsrv**: cada ~500ms mientras `suspended`, chequea `ipc_service_lookup(SERVER_OX)`; si <=0 (oxsrv crashed / kill -9), auto-resume + canvas limpio. Defensa contra el escenario donde `kill -9 <oxsrv>` dejaría la shell muerta. (7) **oxsrv coalesce mouse MOVE**: en vez de mandar un `IPC_OX_EVENT_MOUSE` por cada delta del PS/2 (puede ser >100/seg), pendiente flag + envío uno por frame con la posición final. Edge-detect button DOWN/UP siguen siendo per-event. Baja drásticamente IPC pressure bajo mouse storm. (8) **Caps por iter en oxsrv main loop**: max 64 mouse + 32 kbd + 64 IPC reads por iteración — evita que un storm starve el resto del sistema o sature la queue. (9) **`oxfiles` agregado al menú Ox**: ahora son 6 entries (Files / Notepad / Calculator / Terminal / Settings / Reboot). | 600 |
| **FASE 13.0 — musl libc port (segundo libc opcional)** | (1) **`vendor/musl/`** — musl 1.2.5 vendoreado (~140K LOC). `./configure --target=x86_64 --disable-shared --syslibdir=/lib --prefix=/usr` con `CC="clang -target x86_64-unknown-none-elf -fno-stack-protector -ffreestanding -nostdinc -mno-red-zone"`. `make -j4` compila al primer intento — **zero patches al árbol de musl**. Salida: `vendor/musl/build-osnos/lib/{libc.a (8.5 MB), crt1.o, crti.o, crtn.o}`. (2) **Kernel gaps cerrados** para que musl bootee: `SYS_WRITEV=20` (musl stdio escribe exclusivamente via writev, sin esto cualquier output via FILE* se pierde silente; impl loop sobre iov + reuse sys_write), `SYS_ARCH_PRCTL=158` (code 0x1002 ARCH_SET_FS → wrmsr MSR_FS_BASE = TLS pointer; code 0x1003 ARCH_GET_FS via rdmsr; necesario porque musl pone errno en TLS y todo dispara si %fs no apunta a algo válido), `SYS_SET_TID_ADDRESS=218` (musl __init_libc lo llama early; stub returnea pid). (3) **`build_argv_block` extendido** con auxv mínimo `[{AT_PAGESZ=6, 4096}, {AT_NULL=0, 0}]` después del envp NULL — sin esto musl lee bytes random de strings como auxv keys y setea `libc.page_size` a basura. (4) **`elfs/musl.lds`** linker script propio: mantiene `.init_array/.fini_array/.preinit_array` (musl __libc_start_init los recorre), agrega PT_TLS para `.tdata/.tbss`. (5) **`elfs/tests/hello_musl.c`** smoke test: spawna como ELF normal, valida 5 cosas en un solo flow — crt1 boot, auxv parse, TLS wrmsr, argv pass-through correcto (`hello_musl alpha beta gamma` → argv[0..3]), `snprintf` con `%f` (`3.1415926536`) + `%x` (`deadbeef`) + width/padding, exit limpio. **Verificado headless con captura serial**: 6/6 lines correctas. (6) **`GNUmakefile`**: nueva variable `USER_ELF_MUSL_SRCS` + regla pattern para linkear ELFs con musl (crt1 + crti + libc.a + crtn vs. nuestra libc mini). Coexisten: programs pequeños siguen usando `lib/libc/` (footprint reducido); programs que necesitan stdio/printf-%f/locale/pthread completos usan musl. **Limitación pendiente**: `printf` / `puts` via FILE* devuelven -1 en osnos (la cadena __ofl_lock o init de stdout falla antes de llegar a writev — no diagnóstico aún). `snprintf` + `write(2)` directo funcionan. Próximo iter: portear oxterm / oxnotepad a musl una vez que el printf path esté arreglado. (7) `STATUS.md` + `CLAUDE.md` documentados con el path de integración. **Hito**: osnos ahora tiene **dos libc** — la mini hecha en casa para programas chicos, y musl para los serios. Path claro a portear aplicaciones POSIX reales (vi/nano/ncurses) sin pelearse con bugs de libc artesanal. | 200 |
| **FASE 12.0 — Ox mini-X window system** | (1) **Kernel framebuffer ioctls** (`src/drivers/framebuffer.{c,h}`, `src/fs/devfs.c`, `src/micro/syscall.c`): `framebuffer_get_info` (w/h/pitch/bpp) y `framebuffer_blit_kernel` (rect copy de buffer kernel → FB) exportados. Nuevos ioctls Linux-compat sobre `/dev/fb0`: `FBIOGET_VSCREENINFO` (0x4600) llena `struct osnos_fb_var_screeninfo` con xres/yres/bits_per_pixel/line_length + offsets BGRA; `FBIO_BLIT` (0x4680) acepta `osnos_fb_blit_req {x,y,w,h,*src,src_pitch}` y blittea via copy_from_user row-por-row con `kmalloc` scratch (faulting user pointer → EFAULT, sin panic). Detección en `sys_ioctl` vía `f->is_chr && os_streq(f->path, "/dev/fb0")` — no requiere campo nuevo en el OFD. (2) **ABI Ox** (`src/include/osnos_ipc_abi.h`, `src/include/osnos_fb_abi.h` nuevo): `SERVER_OX=5`, rango IPC `0x60-0x7F` reservado para window system, 14 opcodes (`IPC_OX_CONNECT/WINDOW_CREATE/DESTROY/DRAW_RECT/DRAW_TEXT/DRAW_IMAGE/PRESENT/SET_TITLE/EVENT_KEY/MOUSE/EXPOSE/CLOSE/RELOAD_SETTINGS/RESPONSE`). Wire-format documentado: arg0 = win_id, arg1 = color/scalar, data = packed uint32 args + payload. `lib/libc/include/sys/ioctl.h` agrega defs Linux-shape de `fb_var_screeninfo` y `fb_blit_req`; `lib/libc/include/linux/fb.h` nuevo expone los mismos bajo el path que tinyX espera. (3) **Cliente libc** (`lib/libc/include/ox.h`, `lib/libc/ox.c`, `lib/libc/ox_font.c`): API estilo mini-Xlib — `ox_init/ox_window_create/ox_window_destroy/ox_draw_rect/draw_text/draw_image/present/poll_event/wait_event` + macros `OX_RGB/OX_RGBA/OX_EV_*/OX_KEY_*/OX_MOD_*`. Ring local de eventos (32 slots) para que un wait-for-response no descarte eventos asincrónicos. `DRAW_IMAGE` chunkea automático en tiles de 240 px de ancho para encajar en el payload de 1008 B del `ipc_msg_t`. `ox_font.c` mirror del 8×8 bitmap del kernel (`src/include/font.h`) — clientes pueden medir texto sin hop al server. (4) **`/bin/oxsrv`** (`elfs/gui/oxsrv.c`, ~700 LOC libc-linked, embebido en ROM): registra SERVER_OX, abre /dev/fb0 + /dev/mouse0 + /dev/input0, query `FBIOGET_VSCREENINFO`, aloca backbuffer BGRA full-screen + parsea PPM P6 con `load_ppm` (scaler nearest-neighbour a screen size). Loop: drain /dev/mouse0 (acumula cursor 0..scr-1), drain /dev/input0 raw, drain ipc_recv, recomponer (wallpaper blit → window stack back-to-front con title bars + close `[x]` + body backing → menu cuando visible → cursor sprite 12×17) → un solo `FBIO_BLIT` por frame dirty. Eventos: left-click sobre title=focus+drag o cerrar; sobre body=`EVENT_MOUSE` al owner; right-click sobre wallpaper o F1 = root menu (Notepad/Calc/Terminal/Settings/Reboot, estilo Openbox); Alt+F4=close focused; Alt+Left=cycle focus. Settings via `/home/.oxrc` (`current_wallpaper=samurai\|girl`); reload en vivo via `IPC_OX_RELOAD_SETTINGS`. Window pool de 16, backing buffer por ventana (libre al destroy). (5) **Apps GUI** (`elfs/gui/oxnotepad.c oxcalc.c oxterm.c oxsettings.c`, ~250 LOC c/u libc-linked): **oxnotepad** 600×400 — carga/guarda `/home/notepad.txt`, Ctrl+S, autosave al cerrar. **oxcalc** 240×320 — 4×5 grid + display, +-*/ con accumulador + pending op, click o teclado. **oxterm** 640×400 (80×25 chars) — `posix_openpt + fork + execve("/bin/minishell")` con slave como fd 0/1/2 + master non-blocking, multiplexa eventos Ox + bytes del PTY, renderiza grid de chars usando font de libc, drop ESC sequences. **oxsettings** 400×300 — `opendir /home/wallpapers` lista `.ppm`, radio buttons, "Apply" reescribe `.oxrc` + `IPC_OX_RELOAD_SETTINGS` al oxsrv. (6) **Wallpapers** (`tools/gen_placeholder.c` + `tools/gen_wallpapers.sh` + `res/wallpapers/source/`): script host detecta `res/wallpapers/source/<name>.png` + ImageMagick `convert` → PPM 1280×800; si falta source o convert, compila host C generator que emite PPMs procedurales temáticos (samurai = sunset gradient + silueta poligonal con katana; girl = lavender→pink diagonal + silueta con cabello largo + sakura speckles). 100% desatendido: la build SIEMPRE produce wallpapers válidos. (7) **Boot integration** (`src/kernel/main.c`, `src/fs/bootstrap.c`, `src/proc/builtin.c`): `kmain` spawnea `/bin/oxsrv` después de `shellsrv` y registra `SERVER_OX`; watchdog `server_respawn_tick` agrega `respawn_if_dead(SERVER_OX)`. `bootstrap.c` seed `seed_if_absent("/home/.oxrc", "current_wallpaper=samurai\n")` + `vfs_mkdir("/home/wallpapers")`. `builtin.c` agrega `USERELF("oxsrv", ...)` al ROM (consrv/kbdsrv/shellsrv/banner + oxsrv ahora). (8) **GNUmakefile**: 5 GUI ELFs nuevos en `USER_ELF_LIBC_SRCS`, `oxsrv` también en `USER_ELF_ROM_SRCS`. Nueva regla `$(WALLPAPERS)` ejecuta `gen_wallpapers.sh`. `$(SD_IMG)` agrega `mmd ::/home/wallpapers`, `mcopy .oxrc`, `mcopy *.ppm`. **sd.img bumpado 16 → 32 MiB** + `mformat -c 8` (4 KiB clusters) para que mformat siga eligiendo FAT16 (cluster count <65525). (9) **Decisión FAT case-sensitivity**: **DEJAR como está** (case-insensitive + case-preserving via NT flags y LFN). Flipear rompería `exec("/bin/cat")` contra `CAT.ELF` en disco, `.oshrc` lookup, `bootstrap.c seed_disk_bin`. El comportamiento actual es lo que Windows + macOS hacen sobre FAT/VFAT; "case-exact" en display ya está cubierto por LFN. (10) **Path a tinyX/X11**: el protocolo IPC es estilo Xlib stub (CreateWindow/DrawRect/etc), así que cuando llegue file-backed mmap los clientes podrán mapear shared pixmaps sin cambiar el wire; tinyX puede portar contra `lib/libc/include/linux/fb.h` + `<sys/ioctl.h>` shape. **Verificación**: build clean (zero warnings con -Werror), kernel ELF embebe oxsrv (`_binary_oxsrv_elf_start/end` presentes), sd.img tiene `/bin/oxsrv`/`oxnotepad`/`oxcalc`/`oxterm`/`oxsettings` + `/home/wallpapers/{samurai,girl}.ppm` + `/home/.oxrc`. Headless boot ok (shell prompt llega). Verificación visual (mouse, wallpaper, menu) requiere QEMU con display gráfico. | 2200 |
| **FASE 11.4 — PS/2 mouse driver + /dev/mouse0** | (1) **`src/drivers/mouse.{c,h}`** (~140 LOC): driver PS/2 polling — `mouse_init()` envía 0xA8 al 0x64 (enable AUX) + 0xF6 (set defaults) + 0xF4 (enable streaming) por el path `outb(0x64, 0xD4)` + `outb(0x60, cmd)` con ACK loop y timeout (10 ms) — boots cleanly aunque no haya mouse. `mouse_poll(mouse_event_t *)` drena un byte por call (AUX_DATA bit 5 del 0x64 distingue de keyboard), acumula 3-byte packet, decodifica flags/dx/dy con sign extension (bits 4,5 del byte 0), invierte dy (PS/2 da +y = up, osnos quiere +y = down), filtra overflow bits, exporta `{int16_t dx, dy; uint8_t buttons, _pad}`. Sync recovery via bit 3 del byte 0 (siempre 1) — descarta bytes hasta encontrar packet boundary tras boot. (2) **`src/servers/mouse_server.{c,h}`** (~30 LOC): kernel cooperative task — mirror exacto del keyboard feeder. `mouse_server_init()` llama `mouse_init()`; `mouse_server_tick()` poll → `devfs_mouse_push(ev)` hasta 16 events/tick. Spawneado en `kmain` junto al keyboard feeder. (3) **`src/fs/devfs.c`**: `/dev/mouse0` char device nuevo + 32-entry `mouse_ring` (drop-oldest policy si overflow, igual que keyboard). `devfs_mouse_push(ev)` API pública. `mouse0_read` retorna `sizeof(mouse_event_t)` bytes o EAGAIN si ring vacío. `mouse0_write` retorna EROFS. (4) **`lib/libc/include/sys/mouse.h`** (nuevo): `mouse_event_t` + `MOUSE_BTN_LEFT/MIDDLE/RIGHT` para userland — separa el ABI de userland del header del driver ring 0. (5) **`/bin/mousetest`** (~60 LOC, interactivo): abre /dev/mouse0, loop `read(&ev, sizeof)` con EAGAIN → `nanosleep(20ms)` para no quemar CPU, imprime `dx=%+4d dy=%+4d abs=(%5ld,%5ld) L=%d M=%d R=%d` cuando hay movimiento o cambio de buttons, mantiene x/y acumulados. Ctrl+C exit limpio (SIGINT default). No agregado a alltest porque es interactivo. (6) **Verificación end-to-end**: confirmado por usuario en QEMU — movimiento del host + clicks reportan dx/dy/buttons coherentes. **Habilita futura línea gráfica**: cursor overlay, file managers con click, mini window-system, eventual TinyX. Ring 3 user-visible: cualquier app puede leer `/dev/mouse0` igual que `/dev/input0` para keyboard. **PIC IRQ 12 sigue masked** (PIC unwired completo) — polling consume ~1 inb por tick, imperceptible. | 250 |
| **FASE 11.3 — jq 1.7.1 port (tercer lenguaje self-host)** | (1) **`vendor/jq/`** — jq 1.7.1 vendored (~24K LOC, ~25 .c files incluyendo decNumber/). Build via GNUmakefile rule: `builtin/bytecode/compile/execute/jv*/linker/locfile/util/lexer/parser/main + jq_test + decContext + decNumber`. Compilado con `-DWITHOUT_ONIG=1` (sin oniguruma regex → 4 builtins desactivados: test/match/sub/gsub) + `-DIEEE_8087=1` (jv_dtoa endian) + `-DHAVE_ISATTY/SETLOCALE/MEMMEM/ALLOCA_H/DECL_INFINITY/DECL_ISNAN/...`. (2) **Pre-built files generados al vendoring**: `builtin.inc` (sed-encode de `builtin.jq`), `version.h` (TCC_VERSION="1.7.1-osnos"), `config_opts.inc` (#define JQ_CONFIG "x86_64-osnos"). (3) **libc gap-fill significativo** (~250 LOC): `alloca.h` (macro a `__builtin_alloca`), `libgen.h/c` (`dirname/basename` POSIX-spec), `pthread.h/c` (single-thread shim: mutex no-op, TLS = 64-slot global table, pthread_once con flag), `string.c` agrega `memmem`, `math.c` agrega `isnormal`, `stdlib.c` agrega `realpath` (lexical normalize sin symlinks, getcwd-based para paths relativos), `rand/srand` LCG, `time.c` `strptime` stub. (4) **BUG CRÍTICO de libc**: `malloc(0)` retornaba NULL — esto es válido por C standard pero **glibc/musl retornan non-NULL** y código del mundo real (jq, sqlite, ...) lo asume. Fix: `if (size == 0) size = 1` → siempre alocar al menos 1 byte rounded a 16. Sin este fix jq crasheaba inmediato con "cannot allocate memory" porque jq llama `calloc(0, 24)` para arrays vacíos. **El bug también afecta otros ports** que pasamos por suerte — Lua y TCC no calloc(0). (5) **TCC patch para SHT_NOBITS**: ninguno necesario, jq compiló clean. (6) **`/bin/jqtest`** automated end-to-end (8 checks: write fixture + filter `.name`, `.num`, `length`, array index, arithmetic, ascii_upcase, ad-hoc array `[.num,.list[0]]\|add`). alltest extendido a **18 tests**. (7) **`/home/test.json`** seed file shipeado en sd.img para jugar con jq desde el primer boot. (8) **Verificación end-to-end**: `cat /home/test.json \| jq` pretty-print correcto; `jq '.os' /home/test.json` retorna "osnos"; `jq -r '.tools[].name'` lista todos. **Tercer lenguaje self-host** en osnos después de C/TCC + Lua. **Bug known**: shellsrv parser no respeta single quotes alrededor de `\|` operator (split prematuro) — workaround: usar quotes sin spaces (`jq '.a\|.b'`) o evitar pipe inline; fix pendiente como item separado. | 350 |
| **FASE 11.2 — Lua 5.4 port (segundo lenguaje self-host)** | (1) **`vendor/lua/`** — Lua 5.4.7 vendored (~24K LOC, ~32 .c files). Build via GNUmakefile rule: lapi/lcode/lctype/ldebug/ldo/ldump/lfunc/lgc/llex/lmem/lobject/lopcodes/lparser/lstate/lstring/ltable/ltm/lundump/lvm/lzio (core) + lauxlib/lbaselib/lcorolib/ldblib/liolib/lmathlib/loadlib/loslib/lstrlib/ltablib/lutf8lib/linit (libs) + lua.c (CLI driver) → `/bin/lua` (~1.2 MB ELF). Compilado con USER_CFLAGS sin LUA_USE_POSIX ni LUA_USE_WINDOWS → fallback ISO C path: no signal handlers, no readline, no popen, no os.execute. (2) **libc gap-fill** para que Lua compile clean: `locale.h/c` nuevo (setlocale stub C-locale, struct lconv minimal), `strcoll/strxfrm` en string.c (delegan a strcmp por ser C-locale), `sig_atomic_t` typedef en signal.h, math.h con `asin/acos/sinh/cosh/tanh/frexp/modf` (impls bona-fide via Taylor/IEEE-pun), time.h con `clock()` + `mktime()` + `difftime()` + `strftime()` (subset Y/m/d/H/M/S/j/a/A/b/B/p/% que `os.date()` usa), stdlib.h `system()` stub returning -1, stdio.h `tmpnam()` + `remove()` + L_tmpnam. (3) **`/bin/luatest`** automated end-to-end test (7 checks: crear script.lua + escribir + spawn lua + capture stdout + verificar magic + arithmetic + math.sqrt + string.upper). alltest extendido a **17 tests**. (4) **Decisiones de port**: Sin signal handler integration (lua's `signal(SIGINT, laction)` no se llama porque LUA_USE_POSIX no está definido → Ctrl+C cae al SIGINT default = exit task, suficiente). Sin readline → REPL fallback usa fputs("> ") + fgets — funciona con shellsrv's leave_raw() pre-spawn. Sin loadlib dinámico (`require "pkg"` para extensions C falla con ENOSYS, lo cual es correcto sin dlopen). (5) **Verificación**: `lua -v` retorna versión correcta; `lua script.lua` ejecuta; `luatest` PASS 7/7; alltest 17/17 PASS. **Lua self-host**: podés `ovi /home/script.lua` editás Lua, `lua /home/script.lua` corrés. | 200 |
| **FASE 11.1 polish — FAT true append + offset-native + caching** | (1) **`fat_extend_existing`** en `src/fs/fat.c`: reemplaza el O(N) RMW-the-whole-file con cluster-chain extend real — walk hasta el último cluster + write tail (RMW del sector boundary + sequential write para el resto) + alloc + chain new clusters si el chain actual no aguanta + bump dirent size. Cost O(len) por call en vez de O(existing+len). TCC escribiendo un ELF de 50 KB en 6 chunks: ~50 KB de trabajo total vs ~150 KB antes (~3× speedup). (2) **FAT sector cache** en `fat_get_entry` (single-entry cache of most-recent FAT sector + invalidación cuando `fat_set_entry` muta el FAT): para una cluster chain walk de 12 entries que caen todas en el mismo sector FAT, antes era 12 lecturas ATA → ahora 1. (3) **BUFSIZ 512 → 4096** en libc `<stdio.h>`: TCC escribe via fwrite() + fputc(0)-padding entre secciones; con buffer chico → ~100 sys_write por 50 KB output → con 4 KB → ~12 sys_write (**8× fewer syscalls**). Combined con #1+#2: TCC compile time pasó de "tarda mucho" a **instantáneo** (compile + run de hello.c sub-second). (4) **`/bin/readelf -S`** sección headers dump: `Elf64_Shdr` + SHT_*/SHF_* constants agregados a `osnos_elf.h`. Útil para verificar que TCC output limpio (sin .plt/.got/.dynamic post-FASE 11.0 patch). (5) **`/bin/tcctest`** automated end-to-end (5 checks: write source, tcc compiles cleanly, tcc-built binary exit 0, output contains magic). alltest extendido a **16 tests**. **Verificación**: 16/16 PASS + speed perceptible. | 300 |
| **FASE 11.0 — TinyCC port + offset-native VFS reads (self-hosting tier)** | **HITO HISTÓRICO**: osnos compila programas C desde adentro. (1) **`vendor/tinycc/`** — vendored release_0_9_27 (~30K LOC). Build via custom GNUmakefile rule: `libtcc.c` con `-DONE_SOURCE` (unity build pulling tccpp/tccgen/tccelf/tccrun/tccasm/x86_64-gen/x86_64-link/i386-asm) + `tcc.c` con `-DONE_SOURCE=0` (CLI driver). Defines `-DTCC_TARGET_X86_64 -DCONFIG_TCC_STATIC=1 -DCONFIG_TCCBOOT=1`. `vendor/tinycc/config.h` hardcoded para osnos (CONFIG_TCCDIR=/lib/tcc, CONFIG_TCC_SYSINCLUDEPATHS=/lib/tcc/include:/usr/include, CONFIG_TCC_LIBPATHS=/lib:/lib/tcc). Output ~1 MB `tcc.elf` en /sd/bin/tcc. (2) **libc gap-fill** (~150 LOC): nuevo `ldexp/ldexpf` (math.h), `strtod/strtof/strtold/atof` (stdlib.c con parser decimal + exponente), `struct tm` + `localtime/gmtime/localtime_r` (time.c, epoch sintético 2026-01-01), `gettimeofday`, `fdopen` (stdio.c), `mprotect` no-op (mman.c — osnos no enforce W^X), `sscanf` (~100 LOC con %d/%u/%x/%s/%c + width). `strcmp/strncmp` ahora BSD-style NULL-tolerant (TCC's argv parsing pasa NULL en algunos casos). (3) **TCC's own headers** en `vendor/tinycc/include/`: `stdarg.h` osnos-trimmed (struct va_list_struct sin anonymous union, sin line-continuations — TCC's preprocessor se perdía en su propio header upstream), `stdint.h` nuevo TCC-friendly (sin `__UINT*_TYPE__` builtins que TCC no expone, hardcoded LP64 types). Shipeados a `/sd/lib/tcc/include/` con priority sobre `/usr/include/`. (4) **sysroot en sd.img**: GNUmakefile crea `/sd/{lib,lib/tcc,lib/tcc/include,usr,usr/include,usr/include/{sys,arpa,netinet}}` + mcopy de `crt0.S.o → crt1.o`, `crti.o`/`crtn.o` empty stubs (`lib/sysroot/crt[in].S` con .init/.fini section markers — osnos crt0 no usa init/fini arrays pero TCC's link recipe los busca), `libosnos_c.a → libc.a`, `libtcc1.a` (compiled from `vendor/tinycc/lib/libtcc1.c` — 64-bit divmod helpers etc), libc headers + freestnd-c-hdrs (stdint/stdarg/stddef freestanding del kernel-deps). (5) **Kernel aliasfs mounts** (`src/fs/bootstrap.c`): `/lib → /sd/lib` y `/usr → /sd/usr` agregados al bootstrap_fs (antes solo `/bin` y `/home` se aliaseaban). Sin esto TCC abría `/lib/crt1.o` y VFS daba ENOENT aunque el archivo existía en `/sd/lib/`. (6) **BUG CRÍTICO #1 — sys_read truncaba files > 1024 bytes**: `src/micro/syscall.c` tenía `char tmp[1024]` stack scratch + `vfs_read` whole-file → si filesize > 1KB silenciosamente perdía bytes. Solo afloraba con TCC porque tests/utils usan files chicos. Fix: **offset-native VFS reads**. Nuevo `vfs_ops_t.read(priv, path, off, buf, n, &got)` con offset arg en todos los backends (ramfs/fat/devfs/sysfs/binfs/aliasfs). `vfs_read_at(off)` API pública. sys_read llama directo con `f->offset` → O(count) en vez de O(file_size). aliasfs delega a `vfs_read_at`. fat_vfs_read pasa offset directo a `fat_read_file(de, off, ...)` (que ya soportaba offset). 10× más rápido para reads incrementales. (7) **BUG CRÍTICO #2 — fat_append_path truncaba writes > 8192 bytes**: `static char scratch[8192]` capeaba file size silenciosamente. TCC escribía ELFs de ~50 KB en chunks de 8KB; segundo chunk → FAT_ENOSPC → TCC ignoraba el error → ELF truncado mid-header (`section header table goes past EOF`). Fix: `kmalloc(existing+len)` heap scratch con cap 4 MiB. (8) **TCC patch crítico — PLT32 → PC32 directo en static-link**: TCC's `tccelf.c::build_got_entries()` aunque `static_link=1` seguía emitiendo PLT/GOT stubs con relocations no-resueltas (NULL GOT entries). Resultado: cada call a libc function via PLT saltaba a `*NULL` → RIP=garbage al primer call. Patch: cuando `static_link && output_type=EXE && st_shndx != SHN_UNDEF`, convertir `R_X86_64_PLT32` → `R_X86_64_PC32` direct relocation. ELF resultante tiene solo `LOAD R E` + `LOAD RW`, sin .plt, sin .dynamic, sin GOT. (9) **Defaults TCC para osnos**: `s->static_link = 1` cuando `CONFIG_TCCBOOT` (libtcc.c::tcc_new) — usuarios no tienen que tipear `-static`. (10) **`/bin/readelf`** ELF inspector (~150 LOC): muestra header (type/machine/entry/...) + program headers (LOAD/INTERP/DYNAMIC/...) con flag string `RWE`. Útil para debuggear TCC output desde dentro. (11) `crt[in].S` stubs en `lib/sysroot/` + sysroot copy en sd.img. (12) Headers Linux-only que TCC pedía pero osnos no tenía: skipped en stdio.h `#include <stdarg.h>` resolving via priority path. **Verificación end-to-end**: `tcc /home/hello.c -o /home/hello && /home/hello` → "hello from tcc on osnos!" funciona desde dentro del guest. `readelf -a /home/hello` muestra ELF limpio (solo 2 LOAD, sin DYNAMIC/PLT). | 900 |
| **FASE 10.7 polish — poweroff/reboot + tail -f** | (1) **`SYS_REBOOT` (#169 Linux ABI)** en `src/micro/syscall.c`: acepta `RB_POWER_OFF=0x4321FEDC` → `outw(0xB004, 0x2000)` ACPI S5 (QEMU `-M pc`) con fallbacks a 0x604 (q35), 0x4004 (VBox), 0x501 (isa-debug-exit) — uno de los cuatro responde según host. `RB_AUTOBOOT=0x01234567` → `outb(0x64, 0xFE)` 8042 keyboard reset (universal, funciona en HW real). `RB_HALT_SYSTEM=0xCDEF0123` → `cli; hlt` loop directo. Sin magic1/magic2 cookies (osnos es trusted). (2) **libc `<sys/reboot.h>`** + `lib/libc/reboot.c`: `int reboot(unsigned cmd)` wrap a SYS_REBOOT. (3) **`/bin/poweroff`** y **`/bin/reboot`** ELFs (~15 LOC c/u): llaman a `reboot(RB_POWER_OFF)` / `reboot(RB_AUTOBOOT)`. En QEMU `poweroff` cierra la ventana / sale del modo serial limpio — exit code propagable a CI scripts (`./build_and_run.sh headless <<< "alltest; poweroff"` ahora termina cleanly). (4) **`/bin/tail -f`** flag agregado: poll loop con `nanosleep` 200ms entre reads, EAGAIN/EINTR loop sin romper, Ctrl+C exit limpio (default SIGINT). Multi-file `-f` rechazado con error (KISS — GNU tail multiplexa con headers; nosotros mantenemos simple). **Verificación: confirmado por usuario que `poweroff` cierra QEMU correctamente y vuelve a host shell**. | 130 |
| **FASE 10.7 — Serial console + /dev/tty (v0.10.7)** | (1) `src/drivers/serial.{c,h}` (~80 LOC): UART 16550 polling driver — `serial_init(0x3F8)` programa 38400 8N1 + FIFOs + IRQs disabled, `serial_putc` spin en LSR THRE con CRLF expansion automático, `serial_puts` bulk, `serial_try_getc` non-blocking RX poll. (2) **Dual-console tee**: `framebuffer_write_bytes` invoca `serial_puts(buf, n)` al final — TODO output kernel/shellsrv/ELFs/ovi/less va automáticamente a fb0 Y ttyS0 sin tocar consrv ni shellsrv. Panic handlers (`put_str` en idt.c) también tee a serial — backtrace persistente aunque el framebuffer esté corrupto. (3) **`/dev/ttyS0` + `/dev/tty`** char devices en devfs (`src/fs/devfs.c`): ttyS0 read=`serial_try_getc` loop con EAGAIN si vacío, write=`serial_puts`. tty fallback hace `tty_read` + `framebuffer_write_bytes` (no usado en práctica — fd-routing path es preferido). (4) **`/dev/tty` special-case en sys_open** (mimético a `/dev/ptmx` de FASE 10.6 s3): aloca un OFD nuevo con `is_special=true` → read/write/ioctl naturalmente caen en el path fd 0/1/2 default (stdin_pop / write_to_console / TCGETS-etc). NO requiere campo nuevo en task_t — la kernel TTY ring es singleton. (5) **sys_close relaxed**: refuse en is_special solo si fd < 3 (default stdio). Higher slots con is_special (típicamente /dev/tty opens) son closeable — necesario para que `dup2(open("/dev/tty"), 0); close(opened_fd)` funcione. (6) **`src/servers/serial_input_server.{c,h}`** (~30 LOC): kernel task cooperativo, mismo patrón que `keyboard_server_tick`. Drena hasta 64 bytes/tick de UART RX, `tty_input(b)` directo (sin syscall hop). `\r` → `\n` para compat con Enter de host terminals. Resultado: bytes serial → kernel TTY ring → shellsrv fd 0. Spawneado en kmain junto al keyboard feeder. (7) **`build_and_run.sh` headless mode**: nueva acción `./build_and_run.sh headless` lanza QEMU `-nographic -serial mon:stdio`. Modo gráfico default agrega `-serial file:serial.log` para captura persistente sin interrumpir la ventana. (8) **less pipe-mode** (`elfs/tools/less.c`): si `argc<2`, drena fd 0 a buffer (mismo line-parser), después `open("/dev/tty") + dup2(tty, 0)`. Habilita `cat foo \| less` real. `slurp_fd` helper refactorizado del load_file existente. (9) **`/bin/serialtest`** (~50 LOC, 5 checks): smoke de `/dev/ttyS0` open/write/close + `/dev/tty` open/tcgetattr. alltest extendido a **15 tests** (kerntest..libctest + serialtest). **Verificación end-to-end pendiente**: kernel builda limpio + 79 ELFs en disco; reboot user-side. **Habilita CI automation** (`./build_and_run.sh headless` → captura `alltest` output por stdio), **debug post-mortem** (panic captura en serial.log incluso con FB roto), y **pipe-mode pagers** (`cat foo \| less` antes imposible). | 450 |
| **FASE 10.6 sesión 5 — Mini terminal emulator (FASE 10.6 CERRADA)** | (1) `/bin/minishell` (~40 LOC): programa interactivo demo — banner + prompt `mini$ ` + read line + echo `you said: <line>` + exit on "exit" o EOF. Funciona como child del terminal emulator. (2) `/bin/term` (~180 LOC): terminal emulator real. `posix_openpt` → master + `ptsname_r` + `fork`. Child: `open(slave)` + `dup2(s, 0/1/2)` + `setsid` + `execve(minishell)`. Parent: switch own stdin a raw mode (`tcgetattr` + `tcsetattr` con `ICANON|ECHO|ISIG` off), `select(stdin, master)` loop relay, Ctrl+D (0x04) termina, EOF en master termina. `waitpid` + `tcsetattr` restore. **Integra TODO el stack POSIX**: PTY (sesión 3) + dup2 OFD (sesión 2) + fork+execve (POSIX core) + setsid (job control sesión 1) + select/termios + waitpid/W* (sesiones 1+4). (3) `/bin/termtest` (~120 LOC, 13 checks): automated end-to-end test del pipeline completo. Spawnea minishell en PTY, escribe `"hola\n"` al master, lee `"you said: hola"` de respuesta, escribe `"exit\n"`, lee `"bye"`, waitpid → exit 0. Cubre ptmx/ptsname/fork/dup2/execve/PTY canon mode/waitpid en un solo flow. (4) libc: agrega `EPIPE=32` en `<errno.h>` (necesario para term detectar slave-closed). (5) **Bugfix PTY EOF race**: `pty_pair_t.slave_was_opened` flag (latched en primer `pty_slave_ref`). `pty_master_read`/`pty_master_readable` solo reportan EOF si `slave_was_opened && slave_refs==0 && s2m_level==0`. Sin el flag, el parent's `select()` veía EOF inmediato post-`posix_openpt` porque slave aún no había llegado a `open(/dev/pts/N)` — term mataba al child antes de empezar (síntoma: "child killed by signal 15" al instante). Linux semantics ahora correctas: pre-open state → EAGAIN (block), no EOF. (6) alltest extendido a **14 tests**. **Verificación end-to-end: 14/14 PASS** + showcase manual `term` → `mini$ hola` → `you said: hola` → `exit` → `bye` confirmado en hardware. **FASE 10.6 CERRADA COMPLETA** (5 sesiones, ~3500 LOC, 23+ POSIX features, 14 tests automatizados). | 350 |
| **FASE 10.6 / 11-prep sesión 4 — WUNTRACED + WCONTINUED + fan-out Ctrl+C/Z + shellsrv migrate** | (1) `task_t.wait_change` enum (WAIT_NONE/STOPPED/CONTINUED) trackea state transitions reportables por WUNTRACED/WCONTINUED. (2) **sys_wait4 extendido**: `find_zombie_child` → `find_waitable_child` detecta ZOMBIE + STOPPED-pending (con WUNTRACED) + CONTINUED-pending (con WCONTINUED). Status encoding: `encode_stopped_status(sig)` produce `(sig<<8)\|0x7f` (WIFSTOPPED), WCONTINUED retorna magic `0xffff` (WIFCONTINUED). Child STAYS en STOPPED tras report (no transición), wait_change cleared para evitar re-report. (3) **`notify_parent_stop_continue`** helper público en syscall.h: despierta parent BLOCKED en wait4 cuando child entra STOPPED o CONTINUED. Escribe status vía `vmm_lookup(parent->pml4, ...)`, set parent->saved_rax + state=READY + clear wait_change. (4) **Hook STOPPED transitions** en 3 sitios: `user_task_trampoline.stop_pending` (exec.c), `tty_stop_signal` (Ctrl+Z), y nuevo path en `user_task_resume` SIG_DFL para SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU. (5) **Hook CONTINUED transitions**: `sys_resume` (fg/bg builtins) + `sys_kill` con SIGCONT\|SIGKILL a STOPPED task. (6) **Fan-out Ctrl+C/Z a fg pgid**: `tty_signal` y `tty_stop_signal` walk task table broadcasting a tasks con `pgid == fg_pgid`. Pipeline-en-mismo-pgid muere coordinado. Backwards compat: si pgid==pid (no setpgid), broadcast hits exactly fg pid (mismo comportamiento que antes). (7) **shellsrv migrate**: `wait_pid_capture` pasa de polling `sys_taskinfo` + nanosleep 20ms a `waitpid(pid, &status, WUNTRACED)` directo. Detecta WIFSTOPPED/WIFEXITED/WIFSIGNALED. EINTR loop. Removidas 20+ líneas de polling code, no más race vs reaper. (8) libc: `<sys/wait.h>` agrega `WCONTINUED=8` flag + `WIFCONTINUED(s)` macro. (9) **`/bin/jobtest`** nuevo (12 checks: SIGSTOP→WUNTRACED, SIGCONT→WCONTINUED, SIGTERM→WIFSIGNALED, kill(-pgid, sig) sobre 3 children comunes pgid). alltest extendido a 13 tests. **Verificación end-to-end: 13/13 PASS** (kerntest + forktest 6/6 + waittest 8/8 + sigtest 9/9 + sigchldtest 6/6 + pgrouptest 10/10 + spawntest 5/5 + exectest + ofdtest 16/16 + ptytest 14/14 + fdedgetest 12/12 + jobtest 12/12 + libctest). | 500 |
| **FASE 10.6 / 11-prep sesión 3 — PTY pairs + fdedgetest edge cases** | (1) `src/micro/pty.{h,c}` (~280 LOC): pool de 8 `pty_pair_t`, cada uno con `m2s_buf[4096]` (master→slave) + `s2m_buf[4096]` (slave→master) + termios per-pair + `canon_buf[256]` accumulator + master_refs/slave_refs. ICANON line-buffered slave_read, raw mode pass-through, ECHO copia master_write a s2m_buf, VERASE genera BS-space-BS, EOF cuando master_refs=0 (slave_read returns 0), EPIPE cuando slave escribe sin master. (2) `osnos_ofd_t` extension: `is_pty`, `pty_ref`, `pty_side`. `ofd_unref` dispatch a `pty_master/slave_unref` cuando refcount=0. `pty_pair` libera al pool cuando AMBAS sides llegan a 0. (3) `sys_open` special-case: `/dev/ptmx` → `pty_alloc` + master OFD; `/dev/pts/N` parse + lookup + slave OFD. (4) `sys_read/sys_write` PTY dispatch via pty_side. (5) `sys_ioctl` PTY branch: TCGETS/TCSETS per-pair, TIOCGPTN (master only) retorna pty index, TIOCSPTLCK no-op, TIOCGWINSZ (0x0 placeholder). (6) `sys_close` cleanup: removido double-close legacy (era latent bug post-OFD — sys_close llamaba pipe_close/sock_close ANTES de fd_free que también lo hacía via ofd_unref). Ahora SOLO fd_free, cleanup canonical via OFD refcount. (7) `fd_readable` PTY hook para select. (8) libc `pty.c` con `posix_openpt/grantpt/unlockpt/ptsname_r/ptsname` + decls en `<stdlib.h>`. (9) `kmain` invoca `pty_init`. (10) Tests: `/bin/ptytest` (14 checks: open ptmx/ptsname/slave/canon line buffering/raw mode/multiple pairs/master close→slave EOF) y `/bin/fdedgetest` nuevo (12 checks de edge cases OFD: dup2(fd,fd) no-op, close original con dup vivo, fork+close en parent/child, FD_CLOEXEC per-fd no shared via dup, classic pipe+fork+EOF after all writers gone, execve closes CLOEXEC only, spawn MOVE sin OFD leak en 10 cycles). (11) alltest extendido a 12 tests. **Verificación: 12/12 PASS** (kerntest + forktest 6/6 + waittest 8/8 + sigtest 9/9 + sigchldtest 6/6 + pgrouptest 10/10 + spawntest 5/5 + exectest + ofdtest 16/16 + ptytest 14/14 + fdedgetest 12/12 + libctest). | 700 |
| **FASE 10.6 / 11-prep sesión 2 — Open File Description refactor + FD_CLOEXEC** | (1) **Nuevo `osnos_ofd_t`** en `src/micro/fd.h` con todos los backend fields (is_pipe/socket/chr/dir, pipe_ref/side, sock_idx, path, offset, flags, is_append) + refcount + used. Pool global `ofd_pool[128]` en `src/micro/fd.c`. (2) **`osnos_fd_slot_t` thin**: per-task slot ahora es `{used, ofd_idx, fd_flags}` (12 B vs 150+ B antes). `task_t.fds[16]` reduce ~2.4 KB → ~192 B por task. (3) **Backwards compat**: `osnos_fd_t` mantenida como typedef alias de `osnos_ofd_t` — código existente `osnos_fd_t *f = fd_get(t, fd); f->offset` sigue funcionando. (4) **API helpers**: `ofd_alloc/ref/unref/get` + `fd_alloc/free/dup/dup2/dup_min` + `fd_get_flags/fd_set_flags`. `ofd_unref` con refcount=0 dispara `pipe_close_reader/writer` o `sock_close` automáticamente — backend cleanup unified. (5) **sys_fork POSIX-strict**: quitamos `pipe_dup_reader/writer`; ahora `ofd_ref(parent.ofd_idx)`. Parent+child apuntan al MISMO OFD que cuenta como UN reader/writer del pipe. Cuando uno cierra, ofd_unref no llega a 0 (queda el otro), pipe intacto. Cuando ambos cierran, refcount=0 → pipe_close. (6) **sys_spawn MOVE**: caller's slot → child's slot transfer atomic con conservación de refcount (sin ref/unref). (7) **proc_execve_pipeline** aloca OFDs frescos para pipe endpoints en stdin/stdout slots (era acceso directo a fields, no compatible con slot thin). (8) **proc_exit_current_user** simplified: barre slots con fd_free, backend cleanup automático via OFD refcount. (9) **FD_CLOEXEC** completo: `OSNOS_FD_CLOEXEC=1` en fd.h, `FD_CLOEXEC` en libc `<fcntl.h>`. `fcntl(F_SETFD/F_GETFD)` opera sobre slot's `fd_flags`. `proc_execve_replace` cierra fds 3..MAX con CLOEXEC. dup/dup2 **clears** CLOEXEC (POSIX). (10) Tests: nuevo `/bin/ofdtest` (13 checks: shared offset via dup/dup2/fork, FD_CLOEXEC roundtrip, dup clears CLOEXEC). libctest actualizado para semántica shared offset (era stale: asumía dup gave copied offset). (11) alltest extendido a 10 tests con ofdtest. **Verificación end-to-end: 10/10 PASS** (kerntest 27/27 + forktest 6/6 + waittest 8/8 + sigtest 9/9 + sigchldtest 6/6 + pgrouptest 10/10 + spawntest 5/5 + exectest + ofdtest 13/13 + libctest ~115). | 600 |
| **FASE 10.6 / 11-prep sesión 1 — SIGCHLD + process groups + alltest runner** | (1) **SIGCHLD automático** en `proc_exit_current_user`: tras la transición a ZOMBIE, set `parent_t->sig_pending \|= 1u << (17-1)`. Default disposition es ignore (SIG_DFL path en user_task_resume ya skipea SIGCHLD), programas sin handler no ven cambio. Los que sí instalan reciben SIGCHLD en el próximo iretq. (2) **task_t agrega `uint64_t pgid; uint64_t sid;`**, init `pgid=pid; sid=pid;` en task_create_user_elf (top-level → own pgid + own session, como Linux). `sys_fork` hereda ambos del parent (POSIX). `proc_execve_replace` preserva ambos (POSIX). (3) **6 syscalls nuevos** con números Linux x86_64 exactos: SYS_SETPGID=109, SYS_GETPPID=110, SYS_GETPGRP=111, SYS_SETSID=112, SYS_GETPGID=121, SYS_GETSID=124. setpgid honra restricciones POSIX (no cross-sessions, no session leader). setsid falla con EPERM si caller ya es pgrp leader. (4) **sys_kill extendido** con semánticas POSIX completas: `pid > 0` específico, `pid == 0` broadcast a own pgid, `pid == -1` broadcast a tasks ring-3 vivas, `pid < -1` broadcast a `pgid = -pid`. Helper `kill_one_task` factoriza el delivery. (5) **`task_create_user_elf` setea parent_pid = caller's pid** si caller es ring-3 — habilita getppid() correcto para tasks spawneadas via osn_spawn. Kernel-spawn (kmain) deja parent_pid=0. (6) **`copy_string_from_user`** nuevo helper en uaccess.{c,h}: copy byte-a-byte con stop en NUL + extable fault recovery. Reemplaza fixed-size copy en sys_execve (path/argv/envp) — fixea EFAULT pre-existente en strings cortos cerca de page boundary. (7) **shellsrv ZOMBIE handling**: wait_pid_capture ahora detecta `OSNOS_TASK_ZOMBIE` además de DEAD, llama waitpid() para reapear el slot. Sin esto el fix #5 dejaba loop infinito de polling. bg_jobs ignora ZOMBIE. (8) libc: `pid_t getppid/getpgrp/setsid(void)`, `pid_t getpgid/getsid(pid_t)`, `int setpgid(pid_t, pid_t)` en unistd.{h,c}. SYS_* constants en lib/libc/syscall.h. (9) **`/bin/alltest` runner**: ELF que fork+execve los 9 tests en orden, waitpid, summary banner-delimited al final con PASS/FAIL/ERROR + lista de fails al pie. setsid() en child para que cada test sea session leader limpio. (10) Tests nuevos: `/bin/sigchldtest` (6/6), `/bin/pgrouptest` (10/10). libctest signal-non-default-enosys check actualizado (stub → real signal()/sigaction roundtrip). **Verificación end-to-end via alltest: 9/9 PASS** (kerntest 27/27 + forktest 6/6 + waittest 8/8 + sigtest 9/9 + sigchldtest 6/6 + pgrouptest 10/10 + spawntest 5/5 + exectest + libctest). | 450 |
| **wait(2) + sigaction(2) — ABI POSIX core 100% COMPLETO** | (1) Nuevo estado **TASK_ZOMBIE** (después de DEAD en task_state_t, espejo `OSNOS_TASK_ZOMBIE=6`). proc_exit_current_user transiciona a ZOMBIE si parent vivo, DEAD si orphan (reaper recoge directo). task_t agrega `parent_pid`, `waiting_for_pid`, `wait_options`, `wait_status_ptr`, `sa_handler[32]`, `sa_restorer[32]`, `sig_pending` (uint32 bitmap). (2) **SYS_WAIT4 (#61)**: walk children (parent_pid match), si ZOMBIE consume + ZOMBIE→DEAD + status a user via vmm_lookup en parent's pml4. WNOHANG → 0. ECHILD si no hay children. Sino block (snapshot iret + saved_rax=0 + state=BLOCKED). Wake-up desde proc_exit_current_user: escribe status al user_va via vmm_lookup, saved_rax = child_pid, state=READY. (3) **SYS_RT_SIGACTION (#13)** / **SYS_RT_SIGRETURN (#15)**: copy_from_user struct sigaction, instala en t->sa_handler/sa_restorer. SIGKILL/SIGSTOP rechazados con EINVAL. rt_sigreturn pop el sigframe del user stack, restaura saved_iret_*/saved_*. (4) **Signal delivery en user_task_resume** (exec.c): antes del iretq, si sig_pending != 0, encuentra lowest bit; SIG_IGN → clear+continue; SIG_DFL → exit con 128+sig (excepto SIGCHLD/SIGURG/SIGWINCH = ignore); user handler → construye sigframe (5 iret + 15 GPRs = 160 B) en user stack via `write_other_user(t->pml4,...)` (page-walk con vmm_lookup), redirige buf[4]=handler, buf[1]=restorer_slot, buf[10]=signum. (5) sys_kill / tty_signal migran a `sig_pending |= 1<<(sig-1)`. (6) **EINTR**: sys_kill/tty_signal sobre BLOCKED task con saved_valid escriben `saved_rax = -EINTR` antes de wake, así syscall retorna -1 + errno=EINTR tras el handler. (7) libc surface: `<sys/wait.h>` (nuevo) con W* macros + wait/waitpid; `<signal.h>` extendido con `struct sigaction` + sigset_t helpers + sigprocmask stub; `signal.c` reimplementado sobre sigaction; nuevo `sigtramp.S` con __sigtramp epilogue (10 LOC asm). (8) Tests: `/bin/sigtest` (sigaction install/SIG_IGN/SIGKILL-EINVAL/SIG_DFL via fork+SIGTERM) y `/bin/waittest` (3 children con códigos distintos via wait, WNOHANG, ECHILD, waitpid-by-pid). forktest migrado a waitpid. **Hito histórico**: ABI POSIX core 100% de verdad — fork/execve/wait/sigaction/EINTR, libc surface completa. | 700 |
| **fork(2) real — ABI POSIX core (sin wait/signals)** | (1) `address_space_clone` en vmm.c — deep-copy del user pml4 via walk pml4[0..255] → pdpt → pd → pt → leaf, allocate new phys page + memcpy via HHDM, vmm_map en child. Full copy (no COW yet). (2) `pipe_dup_reader/writer` en pipe.c — bump refcounts cuando un fd se duplica (fork, dup, dup2). (3) **SYS_FORK (#57)** en syscall.c — clone AS + alloc kstack + task_create + copy fds (con pipe bumps) + copy cwd/redirects/heap/mmap/fpu + snapshot del syscall context del parent (iret frame + GPRs) en `child->saved_*` con `saved_rax = 0`. Parent retorna child_pid. Child wakes via user_task_trampoline's saved_valid path → ambos resumen en la instrucción siguiente al syscall con valores rax distintos. (4) libc `pid_t fork(void)` + decl en unistd.h. (5) `/bin/forktest` ELF — verifica pid distinct, stack copy independencia, fd inheritance (open antes del fork visible al child), exit code propagation via wait_pid_capture pattern. **Hito**: ABI POSIX core 100% — read/write/open/close/pipe/dup/fork/execve/exit/kill/wait. | 350 |
| **Fase 2 disk-resident FINAL + execve + shell polish** | (1) **GNUmakefile popula sd.img al build via mtools** (mformat + mmd + mcopy) — los 64 ELFs en `::/bin/<name>` (sin .elf). (2) **Slim embedded blobs**: kernel solo embebe consrv/kbdsrv/shellsrv/banner + user_hello como ROM recovery. **Kernel binary: 7.6 MB → 1.1 MB (85% reducción)**. (3) **SYS_EXECVE (#59)**: `proc_execve_replace` en exec.c hace in-place ELF swap — load new pml4, address_space_destroy old, sched_resume_jump. Preserva pid + fds + cwd. libc `execve`/`execv`/`execvp`. (4) shellsrv `exec` builtin con `osn_set_fg(getpid())` antes del execve para que Ctrl+C llegue al nuevo image. (5) **init-respawn watchdog** kernel task — cada ~100ms checkea los 3 servers, respawn si murieron (resuelve el cuelgue post `exec /bin/foo`). Sleep via TASK_BLOCKED + wakeup_at_ms. (6) shellsrv `;` `&&` `\|\|` operators con tracking de `$?` (exit_code agregado a osnos_taskinfo_t). (7) shellsrv **glob `*`** (matcher recursivo + walk dir + push a argv). (8) `do_ls` POSIX multi-arg (files + dirs con headers). (9) **FAT16 NT case-bits respetados** (byte 0x0C del dirent) — lowercase SFNs `hello`/`head`/`httpd` ya no vuelven en mayúsculas → glob matchea. (10) **opendir valida ENOTDIR** — antes hacía open() en un file y readdir devolvía 0 silenciosamente. (11) **task_reap_dead grace 4 pasadas** — slot queda DEAD ~40-80ms para que waiter capture exit_code (race que rompía `false && echo`). (12) **kernel_fg_pid clear en proc_exit_current_user**. (13) `/bin/exectest` ELF (verifica failure path + success handoff). | -1500 |
| **Polish + Fase 2.1 inicial** | (1) shellsrv `cd` con resolución de paths relativos `..` `.`. (2) `ls /` muestra mount points (/bin /dev /sys /home /sd) sintéticos via vfs_readdir. (3) `/bin/clear` + `/bin/tree` ELFs. (4) ovi output buffering (16KB single-write per render) — evita saturar IPC console queue. (5) **FAT16 dir-chain extension** — extend_dir_chain alloca nuevo cluster y lo encadena cuando ENOSPC; resuelve el cap de ~9 entries por subdir. (6) Disk seed completo re-habilitado. (7) Coreutils: env wc pwd uname basename dirname tail seq yes tee date printf grep sort uniq cut tr banner which clear tree. (8) **Framebuffer chunk safe-split** — framebuffer_write_bytes ya no parte CSI sequences al medio (causa raíz del `~H~` en ovi). (9) **task->name inline** (32B copy en task_create) — sys_taskinfo ya no faulta con user-pointer leaks vía proc_execve VFS path. (10) shellsrv $VAR / ${VAR} expansion + export/unset/setenv + .oshrc auto-load. kerntest 27/27 fix | 450 |
| **Disk-resident /bin (Fase 1)** | bootstrap_fs dump de ELFs a /sd/bin (FAT16) en primer boot. aliasfs /bin → /sd/bin. exec.c prefiere VFS sobre builtin (embedded como recovery ROM). shellsrv soporta PATH search | 200 |
| **FASE 10.4 chunk 5 shellsrv ES EL SHELL** | shellsrv reemplaza shell_server.c en kmain (proc_execve("/bin/shellsrv")). Registra SERVER_SHELL. SYS_SET_FG=267 + SYS_RESUME=268. tty.c routea signals via kernel_fg_pid (sin más shell_fg_pid). wait_pid_or_stop maneja Ctrl+Z (retorna stopped + agrega a bg_jobs). fg/bg/kill builtins. **Borrado**: src/servers/shell_server.{c,h} (-3375 LOC). **Bugfix crítico**: ipc_send ahora traduce SID→pid en el queue (sys_ipc_recv filtra por t->pid). kbdsrv ya no envía IPC_KEY_EVENT (shellsrv lee via raw TTY) — evita saturar queue de 64 | -3050 |
| FASE 10.4 chunk 4 shellsrv background | `cmd &` (trailing ampersand) spawn-without-wait; pid + cmd recordados en bg_jobs[]. `jobs` builtin barre sys_taskinfo, lista live tasks con state, compacta dead. | 100 |
| FASE 10.4 chunk 3 shellsrv pipes + redir | Multi-stage `|` (hasta 4) + `< > >>` redirects via osn_spawn fd inheritance. Builtins solo en single-stage no-redir; con redirects/pipes cae a `/bin/<name>` ELF. Fix sys_read/sys_write fd 0/1: cuando el slot fue overrideado via spawn, ya no atrapa todo en TTY/console — fall through al path file/pipe correcto. `test` builtin → `/bin/kerntest` | 220 |
| FASE 10.4 chunk 2 shellsrv line editor | Raw-mode TTY (ICANON+ECHO off, ISIG kept) + manual line editor con ↑↓ history nav + ←→ cursor edit + DEL/Ctrl+C. Cursor visible via SGR reverse. History ring de 16 + persistencia en /home/.history (load/save). Re-render con cursor block correcto al submit (Enter limpia el block). Builtins: help, exit, pwd, cd, ls, cat, echo, history | 250 |
| FASE 10.4 chunk 1 shellsrv skeleton | **Tercer server ring-3 (proof of concept)**. elfs/osn-server/shellsrv.c (~280 LOC): prompt + read(0) canónico + dispatch table + builtins + fallback osn_spawn para /bin/*. Coexiste con shell kernel (sub-shell mode — usuario invoca `shellsrv`, `exit` vuelve). No registra SERVER_SHELL todavía | 280 |
| FASE 10.4-prep SYS_SPAWN | SYS_SPAWN=266 con fd inheritance (stdin_fd/stdout_fd del caller → child fds[0]/[1], slots MOVED). libc osn_spawn() wrapper. /bin/spawntest valida pipe+spawn+read child output 5/5 PASS. Habilita al shell ring-3 (10.4) a wirear pipes + redirects atómico estilo posix_spawn. Sigue siendo útil junto a fork(2) cuando no se quiere el costo de un fork-exec separado. | 200 |
| FASE 10.3 fs_server eliminado | shell_send_fs1/fs2 ahora llaman vfs_* sincronamente + imprimen amarillo inline (sin IPC roundtrip). IPC_FS_RESPONSE handler removido del shell. **Borrados**: src/servers/fs_server.{c,h} (-240 LOC) + src/servers/console_server.{c,h} (-50 LOC) + int80 console_server_tick hack (-7 LOC). kmain sin fs_pid ni fs_server_init. SERVER_FS ID reservado en ABI pero no se registra | -290 |
| FASE 10.2 keyboard_server ring-3 | **Segundo server en ring 3** (checkpoint pasado). SYS_TTY_INPUT=264 con guard solo-SERVER_KEYBOARD. Kernel keyboard_server.c reducido a feeder mínimo (poll + devfs_input_push). elfs/osn-server/kbdsrv.c (~90 LOC): lee /dev/input0, dispatch CSI arrows + sys_tty_input + ipc_send a shell. ipc_tty_input wrapper en libc. Comportamiento user-visible idéntico — typing, Ctrl+C/Z, arrow keys, ovi raw mode | 200 |
| FASE 10.1 console_server ring-3 | **Primer server en ring 3**. SYS_IPC_SEND/RECV/SERVICE_REGISTER/LOOKUP (260-263). lib/libc/include/osnos_ipc.h con ipc_send/recv/recv_block/service_*. elfs/osn-server/consrv.c (~70 LOC): open(/dev/fb0)+register+loop. Framebuffer parser CSI extendido a truecolor SGR 38;2;R;G;B. kmain pre-registra SERVER_CONSOLE→pid del ELF antes de spawn shell. ps muestra consrv BLOCKED con dispatches > 1000 | 350 |
| FASE 10.0.d + 10.0.e ABI + kerntest | osnos_ipc_abi.h (ipc_msg_t/SERVER_*/opcodes) compartido kernel↔ring-3; service.h+ipc.h forwardean. Build add `-I src/include` a USER_CFLAGS para que ELFs vean los headers ABI. SYS_TASKINFO=265 + osnos_taskinfo.h. /bin/kerntest con 22 PASS sobre sys_taskinfo, /dev/fb0/input0, pipe roundtrip, dup-pipe-share, /sys/meminfo | 220 |
| FASE 10.0.c /dev/fb0 + /dev/input0 | devfs char devices nuevos: fb0 (write → framebuffer via framebuffer_write_bytes) e input0 (read → keyboard_event_t via ring de 32). keyboard_server fan-outs a TTY + IPC + ring sin perder eventos. osnos_fd_t.is_chr para bypass de offset-slicing en streams; sys_write acepta CHR (antes EISDIR). /bin/fbtest + /bin/inputtest | 180 |
| FASE 10.0.b pipe(2) syscall | SYS_PIPE=22 expuesto a userland; osnos_fd_t extendido (is_pipe/pipe_ref/pipe_side); sys_read/sys_write reconocen pipes via fd; pipeline migrado de task_t.pipe_in/pipe_out a fds; cleanup en exit barre fd 0..MAX; libc pipe() wrapper + /bin/pipetest (9/9 pass) | 240 |
| FASE 10.0.a-1 per-task fd tables | `osnos_fd_t fds[16]` dentro de task_t; fd_get/alloc/free/dup* explícitos con `task_t *`; 36 call sites mecánicos; arregla Ctrl+Z-sobre-BLOCKED y kill-sobre-STOPPED; filtra control chars (<0x20) leak vía IPC_KEY_EVENT | 250 |
| Ctrl+Z / fg / bg / jobs | TASK_STOPPED + stop_pending + VSUSP, shell job-control cmds | 250 |
| mmap/munmap anónimo | SYS_MMAP/MUNMAP, bump-allocator en `0x20000000`, libc + sys/mman.h + 12 tests | 250 |
| Pipes + redirects mezclados | `a < in.txt \| b \| c > out.txt`, middle-stage redir validado, `echo -e` shell builtin | 200 |
| O_NONBLOCK runtime | libc cachea O_NONBLOCK per-fd; read() respeta sin syscall extra | 50 |
| Multi-stage pipes | `a \| b \| c \| d` (hasta 4) — proc_execve_pipeline + shell N-split | 180 |
| FXSAVE/FXRSTOR per-task | 512 B fpu_state aligned 16 en task_t, save/restore en task_run_next, printf %f | 120 |
| Shell pipes `a \| b` | pipe ring-buffer kernel object (4 KiB × 16), task pipe_in/out, concurrent stages, /bin/head ELF | 350 |
| Shell redirection | `cmd > file`, `>>`, `< file` parser + proc_execve_redir + per-task stdin/stdout paths | 250 |
| TCC prep (1-6) | write+offset, exec VFS, stack 64 KiB, FPU init, ctype/limits/float/signal/math.h, ramfs bin fix | 900 |
| Libc gaps 4+5+6 | dup/dup2 (#32/#33), fcntl básico (#72), mkstemp/tmpfile | 220 |
| Libc gaps 1+2+3 | strerror completo (33 errnos), stat(path)/access, time/clock_gettime, fix nlink_t ABI | 250 |
| Arrow keys + getcwd/chdir | keyboard_server → ESC[A-D al TTY; SYS_GETCWD/CHDIR per-task | 120 |
| /bin/ovi | editor modal vim-style (hjkl + flechas, i/a/o, x/dd, :w/:q/:wq) + VT100 mínimo + TIOCGWINSZ + SGR reverse | 500 |
| libc path resolve | relativos resueltos vs cwd kernel (getcwd) — fallback $PWD | 50 |
| Shell rc + history | `.history`, `.oshrc` con guard anti-recursión | 100 |
| /home alias | aliasfs (bind-mount VFS) → `/home`=`/sd/home` | 200 |
| libc exec family | execv/execve/execvp con PATH walk | 80 |
| env passing + PATH | shell env, envp end-to-end, getenv/setenv | 250 |
| Reorg elfs/ | shell/tools/net/tests/osn-server subdirs | (refactor) |
| TTY 1+2 | termios ABI + canonical/raw + ISIG | 400 |
| kheap Fase B | slab allocator 8 buckets power-of-2 | 200 |
| kheap Fase A | growth dinámico cap 4 MiB | 60 |
| FASE 8.5.x | networking completo (RTL8139→TCP→DNS→HTTP) | ~2300 |
| FASE 9 | scheduler preempt CPL=3 + sleep/Ctrl+C/bg | 600 |
| FASE 8 | ATA PIO + FAT16 r/w + persistencia | 1100 |
| FASE 7 | libc + ELF ring-3 + crt0 + 25 tools | 1500 |
| FASE 2-6 | VFS + microkernel + IPC + paging + ELF | 2500 |

**Tests**: **640/640 kernel pass** (KHEAP + SLAB + SOCK + VFS + libc
+ ramfs + FAT + STAT/ACCESS + TIME/CLOCK + DUP/DUP2 + FCNTL +
WRITE_OFFSET + EXEC_VFS). Idempotente — `test` se puede correr N
veces seguidas sin falsos negativos. **libctest user-side** ~110
checks cubre strerror/stat/access/time/clock_gettime/dup/fcntl/
mkstemp/tmpfile + ctype/limits/signal/math libc-side, todos en
verde. Ver **AUDIT.md** para checklist de verificación end-to-end.

**Demos que funcionan end-to-end** (compilan código upstream sin
modificar): `selectserver.c` de Beej, curl/Firefox contra
`/bin/httpd`, `tcpclient google.com 80` con DNS real, ttytest
canonical/raw, /home persistente cross-reboot.

**Pendientes no-bloqueantes** (todo polish / feature work, sin bugs
abiertos):
- ~~PTY pairs~~ → **cerrado** en FASE 10.6 sesión 3 (`/dev/ptmx` +
  `/dev/pts/N` con pool de 8, canon/raw mode, ECHO, EOF/EPIPE, libc
  posix_openpt/ptsname, 14/14 PASS via /bin/ptytest)
- ~~Fan-out de Ctrl+C/Z a la fg process group~~ → **cerrado** en
  FASE 10.6 sesión 4 (tty_signal y tty_stop_signal walk task table
  broadcasting a `pgid == fg_pgid`)
- ☐ Pipelines con más de 4 stages (hoy MAX_PIPELINE_STAGES=4)
- ☐ Shell builtins (`cat` / `ls` / …) que respeten redirects sin
  short-circuit a ELF (`echo` ya lo hace vía `echo_unescape`)
- ~~Open file description shared offsets~~ → **cerrado** en FASE 10.6 sesión 2 (OFD pool de 128, refcount, dup/dup2/fork shared via OFD, sys_spawn MOVE semantics, FD_CLOEXEC)
- ☐ tmpfile unlink-on-close (necesita anonymous FD layer)
- ☐ RTC real (hoy time/clock_gettime = segundos desde boot)
- ☐ mmap file-backed + MAP_FIXED + partial unmap (hoy solo anónimo
  con bump-allocator y addr-exact munmap)
- ☐ mremap (no hay)
- ☐ Signals POSIX avanzados: `sa_mask` / `sigprocmask` reales (hoy
  stub), SA_SIGINFO + siginfo_t, signals para faults (SEGV/FPE —
  hoy `proc_exit_current_user` duro). SIGCHLD automático YA cerrado
  en FASE 10.6 sesión 1.
- ☐ Copy-on-write para fork (hoy full page copy)
- ☐ IPv6, TLS, DHCP, TinyCC self-hosting (todo en ROADMAP_APENDICE.md)

---

### Inventario ring 0 vs ring 3 (snapshot 2026-05-22, post-FASE 10.6 cierre completo)

**Ring 0 — código C linkeado dentro de `build/kernel`:**

| Capa | Archivos / módulos |
|------|--------------------|
| Boot + entry | `src/kernel/main.c`, `panic.c` |
| Micro core | `src/micro/`: scheduler, ipc, task, fd, fpu, pmm, vmm, kmalloc, pipe, idt, gdt, tss, extable, uaccess, reaper, service, timer, tty, syscall(+entry/msr/int80) |
| Drivers | `src/drivers/`: framebuffer, keyboard (PS/2), mouse (PS/2 AUX), block_ata, pci, pic, lapic, rtl8139, serial (UART 16550) |
| VFS + backends | `src/fs/`: vfs, ramfs, ramfs_vfs, fat, fat_vfs, devfs, sysfs, binfs, bootstrap, aliasfs |
| Network stack | `src/net/`: eth, arp, ip, icmp, udp, tcp, socket |
| Process lifecycle | `src/proc/`: builtin, elf (loader), exec |
| Lib freestanding | `src/lib/`: memory, printf, string |
| Kernel-side feeders (no son servers) | `src/servers/keyboard_server.c` (poll PS/2 → /dev/input0), `src/servers/mouse_server.c` (poll PS/2 AUX → /dev/mouse0), `src/servers/serial_input_server.c` (poll UART RX → tty_input). Quedan en ring 0 hasta FASE 11 (driver migration) |

**Ring 3 — ELFs separados: ROM recovery embebido + disk-resident en /sd/bin:**

| Categoría | ELFs | Cantidad | Detalle |
|-----------|------|----------|---------|
| Servers (ring-3, spawneados al boot) | `consrv`, `kbdsrv`, `shellsrv`, `banner` | **4** | TODO el frontend del OS corre en ring 3: consola (10.1), keyboard policy (10.2), shell (10.4), boot banner. kmain solo spawn + scheduler. Estos 4 son los únicos embebidos en el kernel (ROM recovery set + bare `user_hello`). |
| Shell script | `osh` | 1 | intérprete de scripts |
| Coreutils | ls cat cp mv rm mkdir rmdir touch echo true false init head tail wc grep sort uniq cut tr seq yes tee env pwd which printf date uname basename dirname clear tree calc top kill sleep ovi less banner hello readelf poweroff reboot | ~40 | en `/bin/` (FAT) |
| Self-hosting | `tcc` (TinyCC 0.9.27, ~1 MB) compila C nativo, ELFs estáticos runnable (FASE 11.0). `lua` (Lua 5.4.7, ~1.2 MB) interprete + REPL + scripts (FASE 11.2). `jq` (jq 1.7.1, ~1.1 MB) filter/transformer JSON con paths + builtins + arithmetic + pipes (FASE 11.3). | 3 | osnos self-compila C, ejecuta Lua, y filtra JSON con jq |
| Net | tcpclient, udptest, echotcp, selecttest, selectserver, httpd | 6 | clientes/servidores TCP/UDP |
| Tests | hello_libc, libctest, ttytest, envtest, fptest, mmaptest, pipetest, fbtest, inputtest, kerntest, spawntest, exectest, forktest, waittest, sigtest, sigchldtest, pgrouptest, ofdtest, ptytest, fdedgetest, jobtest, termtest, serialtest, tcctest, luatest, jqtest, alltest, user_hello (bare) | 28 | sanidad + ABI |
| PTY showcase | minishell, term | 2 | /bin/term spawnea /bin/minishell en PTY — sub-shell interactivo (demo) |
| Input demo | mousetest | 1 | abre /dev/mouse0 + loop read+print dx/dy/buttons (interactivo, no en alltest) |
| **Total ELFs en disco** | | **~87** | en `/sd/bin/` poblado al build via `mtools` |
| Runner consolidado | `/bin/alltest` | 1 | corre los 9 tests POSIX core + summary banner-delimited |

**Distinción clave**: solo `consrv` + `kbdsrv` + `shellsrv` + `banner`
+ `user_hello` viven embebidos en el kernel binario (ROM recovery
set para boot diskless). Los ~62 restantes viven únicamente en disco
(`sd.img`). **Kernel binary: 7.6 MB → 1.1 MB (-85%)** desde Fase 2
disk-resident final.

**Lo que NO migra a ring 3 (queda ring 0, futuro FASE 11):**
- Todos los drivers (framebuffer pixels, PS/2 hardware, ATA PIO, RTL8139, PIT, LAPIC)
- Todo el VFS core + backends
- IPC + scheduler + task + paging + ELF loader
- Network stack
- libc kernel (src/lib/)

---

### Boot + drivers
✓ boot con Limine + QEMU
✓ framebuffer + font bitmap
✓ Limine memmap + HHDM requests parseados al boot
✓ teclado PS/2 con Shift, punto, mayúsculas
✓ teclado: flechas up/down (scancode extendido E0 48/50)
✓ teclado: Ctrl tracking + Ctrl+C → ASCII 0x03
✓ backspace visual
✓ boot sequence: pmm_init -> vmm_init -> kheap_init -> gdt_init -> tss_init
   -> idt_init -> uaccess_init (extable) -> syscall_msr_init (EFER.SCE +
   STAR/LSTAR/FMASK) -> pic_init (remap 8259) -> lapic_init (LINT0=ExtINT
   para q35) -> timer_init (PIT @ 100 Hz) -> ipc_init -> task_init ->
   reaper_init -> scheduler_init -> syscall_init -> ramfs_init ->
   bootstrap_fs -> task_create per server -> service_register ->
   server _init -> sti -> scheduler_loop (longjmp host)

### Microkernel
✓ drivers separados (framebuffer, keyboard)
✓ frontend servers en ring 3: consrv (console), kbdsrv (keyboard), shellsrv (shell)
✓ fs_server eliminado (FASE 10.3) — VFS corre sin IPC roundtrip
✓ SERVER_FS reservado en ABI, no registrado
✓ IPC con queue de 64 slots, payload 1024B
✓ IPC blocking + wakeup por task_unblock
✓ IPC contract documentado en ipc.h (rangos de opcode, convención de respuesta)
✓ ipc_send retorna osnos_status_t (OK / EAGAIN / ESRCH); shell propaga errores al usuario
✓ IPC_PROC_EXITED (rango 0x40) para parent-notification de child death
✓ service registry (SERVER_FS/KEYBOARD/SHELL/CONSOLE)
✓ scheduler round-robin con preemption por timer (CPL=3, 50 ms quantum) + yield/block voluntario en ring 0 + task.dispatches counter
✓ GDT propia (kcode/kdata/ucode/udata + TSS slots)
✓ IDT 256 entries con exception handlers (#PF, #GP, #DF, #UD, etc.)
✓ TSS instalado (RSP0 al kernel stack), ltr OK, IOPB cerrado
✓ PMM bitmap + VMM 4-niveles + kheap free-list + address_space_create/destroy
✓ copy_from_user / copy_to_user con fault recovery via extable (FASE 6.3c)
✓ syscalls Linux x86_64 con números exactos (read=0, write=1, open=2, close=3,
   fstat=5, lseek=8, brk=12, exit=60, rename=82, mkdir=83, rmdir=84,
   unlink=87, getdents=217, +isatty=201 osnos-specific)
✓ syscall ABI dual: int 0x80 (legacy) y syscall (LSTAR fast path),
   mismo dispatcher + misma syscall_frame_t
✓ osnos_status_t con valores Linux errno (EPERM=1, ENOENT=2, EIO=5, EEXIST=17,
   EINVAL=22, ENOSPC=28, EROFS=30, ENOTEMPTY=39, etc.)
✓ osnos_keys.h con valores Linux input-event-codes (KEY_UP=103, KEY_DOWN=108)
✓ osnos_dirent_t layout-compatible con linux_dirent64
✓ osnos_stat_t layout-compatible con Linux x86_64 struct stat

### Shell
✓ shell interactivo con tabla de comandos + auto-help
✓ help / ls / cat / neof / clear / cls (alias) / banner / uname / version / whoami
✓ pwd
✓ cd
✓ relative paths con make_absolute_path() (retorna bool si truncó)
✓ ls / ls [PATH]
✓ mkdir
✓ rmdir (rechaza dir no vacío)
✓ tree / tree [PATH] (DFS iterativo con stack explícito)
✓ cat FILE
✓ touch FILE
✓ rm FILE
✓ rm PATTERN (wildcard *)
✓ cp SRC DST
✓ mv SRC DST (atómico ante overflow de nombres)
✓ echo "texto"
✓ echo "texto" > FILE
✓ echo "texto" >> FILE
✓ paths tipo /home/readme.txt
✓ history (16 entradas, dedupea consecutivos)
✓ navegación con flechas up/down sobre history (con scratch line)
✓ Ctrl+C → cancela input line + prompt fresco

### RAMFS
✓ RAMFS en memoria (32 slots × 128B path × 512B data)
✓ dirs explícitas (is_dir flag)
✓ move recursivo de directorios (renombra hijos atómicamente)
✓ no compacta el array al borrar: punteros a slots permanecen válidos
✓ ramfs_iter_child para iteración encapsulada (slot interno no escapa)

### VFS
✓ src/fs/vfs.h con contrato: vfs_node_type_t (Linux S_IF* values), vfs_stat_t, vfs_dirent_t, vfs_ops_t, vfs_mount_t
✓ src/fs/vfs.c con mount table + longest-prefix dispatch
✓ vfs_init / vfs_mount / vfs_stat / vfs_readdir / vfs_read / vfs_write / vfs_append / vfs_mkdir / vfs_rmdir / vfs_unlink
✓ vfs_copy / vfs_move (con fast-path rename si el backend lo expone)
✓ vfs_touch (stat-then-write-empty)
✓ vfs_list_dir / vfs_tree (DFS iterativo, max depth 16) sobre vfs_readdir
✓ vfs_path_has_wildcard / vfs_glob_list / vfs_glob_read / vfs_glob_unlink (glob '*' en última componente)
✓ src/fs/ramfs_vfs.{c,h}: adapter que expone const vfs_ops_t ramfs_vfs_ops
✓ bootstrap_fs: vfs_init + vfs_mount("/", &ramfs_vfs_ops, 0) + vfs_mkdir + vfs_write
✓ fs_server migrado: 0 llamadas a ramfs_* directo; todo va por vfs_*
✓ wildcard * en ls / cat / rm (vfs_glob_*)

### Build / saneamiento
✓ lib/string.c con strlcpy / strlcat / strncmp / strstarts / strchr / strrchr
✓ constantes en src/include/osnos_limits.h (OSNOS_PATH_MAX, OSNOS_NAME_MAX, OSNOS_INPUT_MAX)
✓ _Static_assert verifica que OSNOS_PATH_MAX*2 + slack <= IPC_DATA_SIZE
✓ -Werror para src/ (cc-runtime sigue permisivo)
✓ cero warnings en build limpio

---

## SIGUIENTE

### Saneamiento pre-VFS — CERRADA
✓ invariantes documentados (ramfs.h, ARCH.md, CLAUDE.md refresh)
✓ mount points extraídos a src/fs/bootstrap.{c,h}
✓ osnos_path_t definido en src/include/osnos_path.h (skeleton)

### FASE 2 — VFS real — CERRADA
✓ 11a. VFS dispatch + mount table (vfs.c)
✓ 11b. Adapter ramfs como backend (ramfs_vfs.{c,h})
✓ 11c. fs_server consume vfs_*, no llama ramfs_* directo
✓ 11d.1 sysfs read-only en /sys: version, tasks, mounts (synthetic)
✓ 11d.2 devfs en /dev: /dev/null, /dev/zero (char devices, mode 0666)
✓ 12. vfs_stat_t con type/size/inode/mode (atime/mtime/uid pendientes hasta FASE 9: clock)
✓ 13. permisos en stat (0755 dirs ramfs, 0644 files ramfs, 0444 sysfs, 0666 chr devfs) — expuestos sin enforcement
✓ 14. mount table interna (VFS_MAX_MOUNTS=8, longest-prefix dispatch)

### Comando test integrado
✓ ~217 asserts cubriendo:
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
✓ 15. ps (lee /sys/tasks vía vfs_read; sin atajos al kernel)
✓ 16. /bin/top — viewer live (cerrado tras FASE 9.4 + ANSI). Ver detalle
   en la entrada de FASE 9.4 abajo.
✓ 17. mem info (/sys/meminfo: tasks, ipc, ramfs, mounts used/total) + comando mem
✓ 18. /sys/tasks (pid, name, state, dispatches contador)
✓ 19. /sys/version (synthetic en sysfs)
✓ extra: comando mount (lee /sys/mounts)
✓ extra: scheduler_get_ticks() + task.dispatches contador instrumentado
✓ extra: /sys/uptime (segundos.ms reales desde FASE 9.1; antes era
   proxy de scheduler ticks)
✓ extra: /sys/cpuinfo (CPUID vendor + brand vía leaves 0 y 0x80000002-4)
✓ extra: /sys/services (id -> name -> pid)
✓ extra: /sys/build (__DATE__ + __TIME__ + __clang_version__)

### FASE 4 — Syscalls — CERRADA
✓ 4.1 fd table (stdin/stdout/stderr + fd>=3, offset, flags, is_dir)
✓ 4.2 sys_write (stdout/stderr -> console IPC; fd>=3 -> vfs_append)
✓ 4.3 sys_open / sys_close (O_CREAT, O_TRUNC, O_EXCL, O_APPEND, O_RDONLY/WRONLY/RDWR)
✓ 4.4 sys_read (con offset propio del fd, EOF al final)
✓ 4.5 sys_fstat (osnos_stat_t layout-compatible con Linux x86_64 struct stat)
✓ 4.5 sys_lseek (SEEK_SET / SEEK_CUR / SEEK_END)
✓ 4.5 sys_isatty (1 para 0/1/2)
✓ osnos_fcntl.h / osnos_stat.h con valores Linux exactos
✓ syscall_dispatch frame-based (entry point para ring3 futuro)
✓ sys_read sobre stdin (ring buffer 256B, keyboard_server pushea printables)
✓ sys_exit conectado al task lifecycle (marca current task DEAD)
✓ sys_mkdir / sys_rmdir / sys_unlink / sys_rename (wrappers VFS)
✓ sys_getdents (drena vfs_readdir, layout linux_dirent64)
✓ sys_open de directorios en O_RDONLY (read directo -> EISDIR; usar getdents)
✓ write con offset real (cerrado en TCC prep Path 1 — sys_write hace
  read-modify-write con scratch kmalloc'd cuando offset != EOF)
✓ errno exportado a userland (libc convierte -errno → errno + return
  -1 en cada wrapper; FASE 7)

### FASE 4.5 — Memory management — CERRADA
Pre-requisito de FASE 6 (ring3) y FASE 7 (libc).

✓ physical memory manager
  - parsea limine_memmap_request + hhdm_request
  - bitmap (1 bit/página de 4KB) en la primera región USABLE grande
  - pmm_alloc_page / pmm_free_page con hint O(1) amortizado
  - /sys/meminfo expone mem total/used/free kB
✓ virtual memory manager + paging propio
  - clona el PML4 de Limine en kernel_pml4 + switch CR3 (control de paging)
  - vmm_map / vmm_unmap / vmm_lookup walk de 4 niveles (PML4 -> PDPT -> PD -> PT)
  - intermediate tables P|W|U allocadas on-demand desde PMM
  - invlpg después de cada modificación
  - /sys/meminfo expone pml4 entries used/total
✓ heap kernel real
  - 16 páginas iniciales (64 KB) mapeadas en KHEAP_VIRT_BASE = 0xffffc00000000000
  - free-list singly-linked first-fit con split + coalesce con next/prev
  - kmalloc 8-byte aligned; kfree(NULL) y kmalloc(0) edge cases
  - /sys/meminfo expone kheap total/used B
  - **kheap growth** (Fase A — CERRADA): cuando find-fit falla, mapea
    más páginas al final del heap (chunks de 16 = 64 KB, o lo que
    pida la request si es más grande), construye un free block en la
    región nueva, coalesce con el tail si quedó libre. Cap a 4 MiB.
    Tracking: grow_events, grow_oom, peak_used. /sys/meminfo expone
    todo. Tests: 14 asserts KHEAP en `test` shell command (96 KiB
    big alloc, burst de 200 × 128 B, verifica baseline después de
    free-all, peak high-water-mark, grow_oom=0 bajo carga normal).
  - **slab allocator** (Fase B — CERRADA): 8 buckets power-of-2
    (16/32/64/128/256/512/1024/2048) con VA dedicada
    SLAB_VIRT_BASE=0xffffc00100000000 (cap 4 MiB). Cada bucket
    mantiene LIFO free list. Cuando vacío, pide 1 página al pmm,
    pone slab_hdr_t {magic, bucket_idx} al inicio y trocea el
    resto en slots del bucket size; todos van a la free list.
    O(1) alloc/free. Detección slab vs first-fit por range check
    sobre el ptr. kmalloc(N≤2048) → slab; N>2048 → first-fit.
    Defense in depth: magic + bucket_idx en cada slab page rechaza
    pointers ajenos en kfree. Counters per-bucket
    (slots_used/total, alloc_total, free_total) + globales
    (slab_used_bytes, pages, grow_events/oom). /sys/meminfo dump
    compacto: `16:0/255 32:0/0 ... 2048:0/1`.
    Tests: ~14 asserts SLAB en `test` shell command (dispatch
    correcto en boundary 2048/2049, VA-range check, burst de 100
    × 64 B verifica grow + slots_used, baseline restaurado tras
    free-all, slab_grow_oom=0). Total: **603 tests pass / 0 fail**.
✓ address spaces por proceso
  - address_space_create: clona high-half de kernel_pml4, low-half vacío
  - address_space_destroy: walk low half + free user pages + intermediate tables + PML4
  - kernel siempre visible (high half compartido por todas las AS)
  - task_t.pml4 (NULL = usa kernel_pml4); switch CR3 viene en FASE 6 con ring3
✓ copy_from_user / copy_to_user (skeleton)
  - validan rango: user pointer debe quedar por debajo de OSNOS_USER_VIRT_MAX = 0x0000800000000000
  - hoy son memcpy directo; falla bonita ante unmapped page necesita fault handler (FASE 6)
☐ reemplazar arrays estáticos en ramfs/ipc/fd con allocations dinámicas
✓ page fault handler con extable (cerrado en FASE 6.3c — copy_*_user
  ya no triple-faulta; extable.{c,h} redirige a recovery_rip)

(SMAP/SMEP, KPTI, NX everywhere, COW, ASLR -> ROADMAP_APENDICE.md)

### FASE 5 — Userland (mínima, todavía ring0) — CERRADA
✓ 24. pseudo-userland: tasks que corren un trampoline + builtin_main
✓ 25. builtins built-in en /bin (binfs synthetic RO; hello, echo, true, false, init)
✓ 26. exec interno: proc_exec("/bin/PROG", "args") + cmd_exec en shell
✓ 27. mini init: builtin /bin/init exposed (kmain sigue siendo el init real)
✓ IPC_PROC_EXITED desde la trampoline -> shell defiere prompt hasta que el child termina
✓ builtin /bin/cat (open + read loop + write + close — ejercita la syscall ABI completa)

✓ ABI de filesystem completa para builtins:
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
✓ 29a. GDT propia: null, kcode (0x08), kdata (0x10), ucode (0x18|3), udata (0x20|3)
       reload con far return; ds/es/ss/fs/gs apuntando a kdata
✓ 29b. IDT con 256 entries (4 KiB), gates de interrupción long-mode (type 0x8E)
       handlers en clang __attribute__((interrupt))
       generic catch-all + específicos para #PF (con CR2), #GP, #DF, #UD, etc.
       panic header al framebuffer en rojo, hlt (ya no triple-fault)
       /sys/meminfo expone exceptions count
✓ 29c. TSS instalado (16-byte descriptor en GDT slots 5-6, selector 0x28)
       RSP0 apunta a kernel_stack[16384] alineado a 16
       IOPB offset = sizeof(tss) (sin permisos de I/O desde ring 3)
       ltr ejecutado; tr_now == 0x28 verificado
       tss_set_rsp0 listo para per-task switching en FASE 6.3
✓ 6.3a primer salto a ring 3 (comando `ring3` one-shot) — VERIFICADO en QEMU
   - address_space_create + 2 user pages (code RX, stack RW)
   - kstack 16KB vía kmalloc -> tss_set_rsp0
   - iretq frame (SS=0x23, CS=0x1b, RFLAGS=0x202) -> CPL=3
   - int3 (0xCC) llena la code page; trap inmediato (rip=0x400001 = RIP+1 porque int3 es trap)
   - exc_3 handler imprime "from USER mode (ring 3)" + rip/cs/rsp -> hcf
   - IDT[3] y IDT[4] con DPL=3 (int3/into callable desde ring 3, convención Linux)
   - Validado: GDT user selectors / TSS.RSP0 / IDT desde user / AS user-kernel split

✓ 6.4a syscall ABI desde ring 3 vía `int 0x80` (Linux legacy ABI) — VERIFICADO en QEMU
   - IDT[0x80] DPL=3 → handler `int80_entry` (file-scope asm en src/micro/int80.c)
   - stub pushea syscall_frame_t (rax, rdi, rsi, rdx, r10, r8, r9) en orden del struct
   - alinea stack a 16 (rbp save), llama int80_dispatch_wrapper(frame)
   - wrapper llama syscall_dispatch + console_server_tick (drain stdout antes de iretq)
   - retval del syscall sobreescribe rax saved; restore + iretq → user ve resultado en rax
   - registros caller-saved no salvados (RCX/R11), igual que SYSCALL → user-stub portable
   - User stub `user_hello_start..end`: write(1,"hello from ring3\n",17) + exit(0)
   - Comando `ring3hello` arma el AS, entra a ring 3, imprime desde CPL=3 vía VFS,
     vuelve al shell limpio (gracias a 6.3b.1)

✓ 6.3b.1 scheduler resume primitive (setjmp/longjmp ad-hoc) — VERIFICADO en QEMU
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

✓ 6.3b user tasks integrados al scheduler de primera
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

✓ 6.3c page fault handler con extable — VERIFICADO en QEMU
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
✓ 6.3d reaper de DEAD tasks
   - src/micro/reaper.{c,h}: cola de 16 kstacks pendientes + task_reap_dead
   - task_t.kernel_stack_base guardado al crear el user task; pasado al
     reaper en proc_exit_current_user (sys_exit y fault path)
   - scheduler_tick llama reaper_drain al inicio: kfree de cada kstack
     pendiente + reset de slots DEAD → UNUSED (ps/ /sys/tasks limpios)
   - /sys/meminfo expone "reaped tasks" y "reaper leaks" counters
   - test: N exec /bin/ring3hello sin crecimiento monotónico de kheap
✓ 6.4b SYSCALL/SYSRET MSRs (STAR, LSTAR, FMASK) — verificado en QEMU
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

✓ 28 ELF64 loader simple — verificado en QEMU
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
✓ SYS_BRK = 12 (Linux number) en syscall.h + sys_brk en syscall.c
   - task_t.heap_start / heap_brk; USER_HEAP_BASE = 0x10000000
   - grow: pmm_alloc + vmm_map (PTE_W|PTE_U) zero-filled; rollback on OOM
   - shrink: vmm_unmap + pmm_free pages above new_brk
   - brk(0) query; out-of-range refusal returns current break
   - 8 asserts nuevos en cmd_test (query, grow 1 page, grow 2 pages,
     shrink, refused below heap_start, refused into kernel)
✓ lib/libc/ — mini-libc local linked into user ELFs
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
✓ toolchain Makefile
   - USER_ELF_SRCS (bare) y USER_ELF_LIBC_SRCS (con libc) separados
   - pattern rule específico para user_hello.elf (sin libc); pattern
     genérico para *.elf con libc + crt0
✓ /bin/hello_libc — primer ELF que usa libc: printf con varargs +
   malloc/strcpy/puts/free + formatos %x %o %d %u %p %s

✓ FASE 7.6: /bin/calc + /bin/osh — primeros programas de verdad
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

✓ FASE 7.5: argv passing + migración de tools a libc ELF
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

✓ FASE 7.7 — libc Tier 1 (pre-networking) — VERIFICADO en QEMU
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

✓ 8.1 ATA PIO block driver — VERIFICADO en QEMU
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

✓ 8.1 wiring QEMU + sd.img — VERIFICADO en QEMU
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

✓ 8.2 FAT16 parser (read-only) — VERIFICADO en QEMU
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

✓ 8.3 VFS adapter — VERIFICADO en QEMU
   - src/fs/fat_vfs.{c,h}: const vfs_ops_t fat_vfs_ops. Strip de "/sd"
     antes de delegar al parser. Esconde "." y ".." en readdir (convención
     POSIX/VFS).
   - bootstrap_fs: `if (fat_init() == 0) vfs_mount("/sd", &fat_vfs_ops, 0)`.
     Sin disco el mount no aparece; el resto del FS sigue intacto.
   - `cat /sd/README.TXT` imprime "hola desde el disco".
   - `ls /sd` lista README.TXT + HELLO.TXT.

✓ 8.4 FAT write support — VERIFICADO en QEMU (round-trip + reboot persistence)
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

✓ 8.5 fsck minimal — VERIFICADO en QEMU (read-only audit)
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

☐ opcional fsck repair-mode — pendiente
   - Hoy es read-only audit. Una pasada que (a) libere clusters leaked,
     (b) trunque chains con cross-links, (c) escriba FAT[0] sobre los
     espejos divergentes sería el siguiente paso natural.

✓ 8.6 rename in-place — VERIFICADO en QEMU
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

✓ 8.7 LFN read-side — VERIFICADO en QEMU
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

✓ 8.7.1 bugfix shell: strip de outer quotes en make_absolute_path
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

✓ 8.8 LFN write-side — VERIFICADO en QEMU (258 PASS / 0 FAIL en cmd_test)
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

✓ 8.8 shell quote stripping
   - make_absolute_path strippea pair de outer quotes (single o double)
     + trailing whitespace antes del check absoluto/relativo. Permite
     `cat "/sd/My Long Filename.txt"` desde el shell. Beneficia todos
     los handlers que pasan args a make_absolute_path (cat/rm/touch/
     mkdir/rmdir/tree/ls/cd).

✓ 8.9 self-test extendido — VERIFICADO en QEMU
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

### FASE 8.5 — Networking (post-FAT) — CERRADA
   (Sub-items individuales detallados abajo; ítems opcionales como
   IPv6 / DHCP / TLS movidos a ROADMAP_APENDICE.md.)


✓ 8.5.1 PCI scan + RTL8139 driver — VERIFICADO en QEMU
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

✓ 8.5.2 Ethernet + ARP — VERIFICADO en QEMU
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
✓ 8.5.3 IPv4 + ICMP echo — VERIFICADO en QEMU
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
✓ 8.5.4a UDP layer + kernel socket API — VERIFICADO en QEMU
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

✓ 8.5.4b Linux socket syscalls + libc + udptest ELF — VERIFICADO en QEMU
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

✓ 8.5.5a TCP handshake (passive) — VERIFICADO en QEMU
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

✓ 8.5.5b TCP data transfer + graceful close — VERIFICADO en QEMU
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
✓ 8.5.5c TCP listen+accept + child sockets + /bin/echotcp — VERIFICADO en QEMU
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
✓ 8.5.6 select() + setsockopt + cooperative yield — VERIFICADO en QEMU
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
✓ 8.5.7 getaddrinfo no-DNS + /bin/selectserver — VERIFICADO en QEMU
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

✓ 8.5.8 TCP connect() / SYN_SENT / outbound — VERIFICADO en QEMU
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
✓ 8.5.9 DNS resolver + getaddrinfo con nombres — VERIFICADO en QEMU
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

✓ 8.5.10 /bin/httpd HTTP/1.0 server — VERIFICADO en QEMU
   Multi-cliente: probado con curl en loop (5+), F5 spam en
   Firefox/Safari, cliente browser real renderizando el HTML con CSS.
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

✓ 8.5.11 TCP retransmisión + RTO + tests — VERIFICADO en QEMU
   - sock_t extiende con buffer de retx por socket: uint8_t
     retx_buf[TCP_MSS], uint16_t retx_len, uint32_t retx_seq,
     uint64_t retx_sent_ms, uint8_t retx_count. ~12 KiB de BSS extra
     (8 sockets × 1.5 KiB).
   - sock_send guarda copia del último segmento enviado (seq, len,
     timestamp). cli/sti protege la actualización contra reentrancia
     del IRQ.
   - En ACK handling de ESTABLISHED: si el ACK cubre retx_seq +
     retx_len, retx_len = 0 (segmento confirmed).
   - sock_tick(now_ms): hookeado al timer_handle a 100 Hz. Walka
     todos los sockets; los que tienen retx_len > 0 y elapsed >
     TCP_RTO_MS (500ms) retransmiten el mismo segmento con la misma
     seq number, bump retx_count, g_retx_total++. Tras
     TCP_MAX_RETX=5 intentos sin ACK → RST, state CLOSED, drop la
     conexión, g_retx_drops++.
   - Estilo simple "single-segment go-back-1" en vez de go-back-N
     completo. Para QEMU localhost (drop rate ≈ 0) es transparente;
     bajo packet loss real recupera al menos el último segment.
   - Sin RTT smooth estimator (Jacobson/Karn) — RTO fijo en 500ms.
     Congestion control / slow start quedan como TODO futuro.
   - **High-entropy ISN**: alloc_child_for_syn ahora computa el ISN
     como timer_ms * 0x9E3779B1 ^ (i * 0x1234ABCD). Antes era
     timer_ms + i*1000 — conexiones consecutivas reusando el mismo
     slot tenían ISNs muy parecidos, lo que confunde a slirp/NATs
     stateful (interpretan la nueva como retransmisión vieja → RST).
   - sys_sendto error map: si sock_send falla con state==CLOSED
     pero el slot todavía vivo, retorna ECONNRESET (errno 104) en
     vez de EBADF. EBADF queda para "fd o socket inválido". Ayuda
     al diagnóstico de connection-reset vs slot-freed.
   - /sys/net extendido con línea "tcp retx/drops: N / M".
   - Diagnósticos públicos (getters): sock_tcp_state_int,
     sock_tcp_retx_len, sock_tcp_retx_count, sock_local_port,
     sock_tcp_get_local_port. Usados por los nuevos tests.
   - **Tests nuevos en `test` shell command** (17 asserts SOCK):
     create UDP/STREAM, bind UDP/TCP, state CLOSED→LISTEN
     transition, fresh retx buffer empty, retx_count=0, listen
     sin bind rechazado, sd inválido devuelve -1, close LISTEN,
     /sys/net retx counters accessibles. Todos los asserts pasan;
     puro state-machine inspection (no se requiere red real).
   - **Bug pendiente del httpd**: ya RESUELTO. Ver 8.5.10b.

✓ 8.5.10b httpd multi-curl bug post-mortem — RESUELTO
   Después de mucha investigación con instrumentación granular,
   resultó que NO era ni state machine, ni FD lifecycle, ni close
   prematuro. Era el **driver del RTL8139**.
   - rtl8139_tx solo miraba dev.tx_cur (el slot "actual") y
     retornaba false si ese slot específico estaba en uso. Con 4
     slots y bursts (handshake + ACKs + body chunked), el slot
     que tocaba para la SEGUNDA conexión podía estar todavía
     ocupado por la cola de TX de la primera → ip_send → tcp_send
     → sock_send fallaban en cascada → user veía EBADF.
   - Fix: rtl8139_tx ahora hunt-loop sobre los 4 slots, buscando
     uno con OWN=1. Solo retorna false si los CUATRO están
     ocupados (saturación real — improbable en QEMU localhost,
     y TCP se encargaría de retransmitir).
   - Diagnostic mileage que dejó el camino:
     - g_free_* counters en /sys/net contando freeings por path.
     - g_last_send_fail_* counters mostrando el estado del slot
       cuando sock_send rechaza.
     - g_sendto_fail_* counters para el path !f || !f->is_socket
       en sys_sendto (early return).
     - Diagnostic sentinel state=-100-N cuando tcp_send mismo
       falla (vs state machine rechaza).
     Estos counters quedan visibles en /sys/net y son útiles para
     futuras investigaciones de la pila TCP/eth.
   - Verificado: `for i in 1..5; do curl --http1.0 ... ; done`
     baja el HTML 5 veces. F5 spam en Firefox/Safari muestra la
     página renderizada con CSS sin problemas, recargas múltiples.
   - **Lección**: cuando todo arriba (state machine, FD, ARP)
     parece correcto, mirá el driver. Especialmente si los counters
     IP tx ≠ NIC tx (señal de eth_send fallando).

✓ 8.5.13 task-suspend para blocking syscalls — VERIFICADO en QEMU
   El problema: el busy-poll del kernel monopolizaba el CPU en CPL=0,
   los servers cooperativos (keyboard, shell) nunca podían correr, y
   por lo tanto Ctrl+C nunca llegaba al fg_pid durante accept/recv/
   connect que se quedaban pegados forever.
   El patrón: mover el loop a userspace. Los syscalls se hacen
   non-blocking (single-shot poll, retornan EAGAIN/EINPROGRESS), y
   la libc loopea con nanosleep entre intentos. Mismo modelo que
   ya aplicamos en select().
   - osnos_status.h: + OSNOS_EINPROGRESS=115 (Linux value).
   - errno.h libc: + EINPROGRESS=115.
   - sock_connect refactorizado para re-entry:
     - state CLOSED + remote_port==0 → fire SYN, state SYN_SENT,
       devuelve -2 (in progress).
     - state SYN_SENT → devuelve -2.
     - state ESTABLISHED → devuelve 0.
     - state CLOSED después de SYN previo (remote_port != 0) →
       devuelve -1 (refused / RST).
     Ya no busy-polla — single step y devuelve.
   - sys_accept: pasa timeout=0 a sock_accept. -2 → EAGAIN.
   - sys_recvfrom: pasa timeout=0. -2 → EAGAIN. Stream y datagram.
   - sys_connect: 0 → 0; -2 → EINPROGRESS; -1 → ECONNREFUSED.
   - libc accept/recv/recvfrom/connect: loop calling syscall.
     - EAGAIN o EINPROGRESS → osnos_yield_ms(10 o 20) entre intentos.
     - Otro errno → propaga normal.
     - 0/n>0 → success.
   - osnos_yield_ms helper hace nanosleep que vía sys_nanosleep
     suspende el task vía sched_resume_jump (FASE 9.3). Otros tasks
     READY corren. Cuando el timer dispara wakeup, scheduler nos
     re-dispatcha.
   - Verificado: `exec /bin/httpd` + 8 curls en sucesión (todas
     funcionando), luego Ctrl+C en QEMU → httpd matado limpiamente,
     vuelta al shell. Ya no se cuelga el sistema.
✓ 8.5.14 fd cleanup en kill/exit path — VERIFICADO en QEMU
   Cuando un task termina (sea por sys_exit, fault recovery, o Ctrl+C
   vía kill_pending), proc_exit_current_user ahora itera fds 3..MAX-1
   y los cierra antes del address-space teardown:
   - is_socket → sock_close (que dispatcha a sock_close_tcp en TCP,
     emitiendo FIN o liberando directo si LISTEN/CLOSED).
   - todos → fd_free (libera slot del fd table).
   Sin esto, matar httpd con Ctrl+C dejaba el listen socket vivo,
   reusando el puerto causaba EADDRINUSE al re-bindear.
   Verificado: exec httpd → Ctrl+C → exec httpd otra vez → bind OK,
   sirve nuevas curls. La tabla fd queda completamente limpia.

✓ 8.5.X cleanup de instrumentación post-mortem — VERIFICADO en QEMU
   Removidos los counters diagnostic introducidos durante la caza del
   bug 8.5.10b (RTL8139 tx-slot saturation):
   - g_free_udp_close / g_free_*_zombie / g_free_close_listen /
     g_free_tick_maxretx (8 counters, ~64 bytes BSS).
   - g_last_send_fail_* (5 ints + 5 getters).
   - g_sendto_fail_* en syscall.c (4 ints + 4 getters).
   - fd_peek_raw (helper sólo usado por el diag de syscall.c).
   - Sentinel `state = -100 - tcp_state` en sock_send cuando tcp_send
     fallaba.
   - Dump verboso en /sys/net (~30 líneas de format) — vuelve a ser
     compacto con solo tcp retx/drops.
   - Build ~7 KB más chico.
   Mantenidos: sock_tcp_retx_total/drops (métrica legítima), getters
   públicos sock_tcp_state_int / sock_tcp_retx_len / sock_tcp_retx_count
   / sock_tcp_get_local_port (usados por los 17 tests SOCK del
   `test` command).

(IPv6 movido a ROADMAP_APENDICE.md como item de "Networking
 avanzado" — no bloquea nada del roadmap principal.)

### FASE kheap — Allocator robusto — CERRADA
✓ Fase A: kheap growth — VERIFICADO en QEMU
   - First-fit free list crece dinámicamente en chunks de 64 KiB
     (KHEAP_GROW_PAGES=16) cuando find-fit no encuentra block.
   - Cap a 4 MiB (KHEAP_MAX_BYTES). Si una request es mayor que el
     chunk default, alloca lo que pida la request.
   - Coalesce con tail al crecer si el tail estaba libre.
   - Backout en falla parcial (vmm_unmap lo que ya se mapeó).
   - Counters: grow_events, grow_oom, peak_used. /sys/meminfo
     expone todo.
   - Tests: 14 asserts KHEAP (96 KiB big alloc forzando grow, burst
     200×128B, verifica baseline tras free-all, peak high-water-mark).

✓ Fase B: slab allocator — VERIFICADO en QEMU
   - VA dedicada SLAB_VIRT_BASE=0xffffc00100000000, cap 4 MiB.
   - 8 buckets power-of-2: 16/32/64/128/256/512/1024/2048.
   - Slab page (4 KiB) con slab_hdr_t {magic, bucket_idx} al inicio,
     resto troceado en slots del bucket size. Todos los slots a la
     free list LIFO del bucket.
   - kmalloc(N): N≤2048 → slab path (O(1)). N>2048 → first-fit
     fallback (que ya crece via Fase A).
   - kfree(ptr): range-check sobre ptr; in-slab → slab_free
     (recovery por page-aligned header + magic). Else → first-fit.
   - Defense in depth: magic check rechaza pointers ajenos.
   - Counters per-bucket + globales en /sys/meminfo:
     `slab buckets: 16:U/T 32:U/T ... 2048:U/T`.
   - Tests: 14 asserts SLAB (dispatch boundary 2048/2049, VA-range
     check, burst 100×64B verifica grow, baseline tras free-all).
   - **603 tests pass / 0 fail** combinando KHEAP + SLAB.

### FASE TTY — Line discipline + termios (1+2) — CERRADA
✓ TTY layer (src/micro/tty.{c,h}) — VERIFICADO en QEMU
   - struct osnos_termios layout-compatible con Linux x86_64 kernel
     ABI (NCCS=19). flags ICANON/ECHO/ECHOE/ISIG, c_iflag con
     ICRNL/IGNCR/INLCR, c_cc[VINTR/VQUIT/VERASE/VKILL/VEOF/...].
   - tty_input(c) (llamado por keyboard_server en lugar de
     stdin_push):
     - ISIG: VINTR → SIGINT (kill_pending) al fg user task vía
       shell_fg_pid(). Kernel tasks (shell) jamás targeted.
     - ICANON: line buffer interno (256 B). Backspace edita la
       línea actual con ECHOE; '\n' flushea todo al read buffer.
       VKILL borra la línea.
     - Raw mode (ICANON off): cada char directo al read buffer.
     - ECHO: framebuffer_draw_string directo, sólo si hay un user
       task fg (evita doble echo con el shell).
   - stdin_push/pop/readable/clear ahora son thin shims al tty.
     fd.c queda más chico.
   - sys_read(fd=0): single-shot. Si vacío retorna -EAGAIN. Libc
     read() loopea con nanosleep (mismo patrón que select/accept/
     recv): yieldea al scheduler, otros tasks corren, eventualmente
     bytes llegan. read() bloquea correctamente sin preempción CPL=0.
   - sys_ioctl (SYS_IOCTL=16): TCGETS / TCSETS / TCSETSW / TCSETSF.
     Copy via copy_from/to_user (faulting → EFAULT, no panic).
     Otras requests → ENOTTY (errno 25, agregado al status enum).
   - Shell deja de procesar IPC_KEY_EVENT cuando fg_pid != 0. Evita
     double-echo + line editor robando keys del ELF + Ctrl+C
     cancelando input del shell ADEMÁS de matar el child.
   - libc: <termios.h> con struct termios (NCCS=19), flags y
     c_cc constants Linux-compat. tcgetattr/tcsetattr/ioctl
     wrappers. <sys/ioctl.h> con TCGETS/TCSETS.
   - elfs/tests/ttytest.c demo:
     1) Lee termios actual.
     2) Stage canonical: read() bloquea hasta Enter, echo via TTY.
     3) Stage raw (ICANON|ECHO off): read() char-by-char.
     4) Ctrl+C durante raw → ISIG entrega SIGINT, task muere.
     5) Restaura termios original.

### FASE env passing + PATH — CERRADA
✓ Auto /bin/ prefix + ELF fallback en el shell
   - cmd_exec: si path no empieza con '/', prepende "/bin/".
     `exec httpd` = `exec /bin/httpd`. Path absoluto sigue
     funcionando.
   - run_command: si no matchea un comando builtin, chequea
     builtin_find(first_token); si es un ELF registrado, auto-exec
     (`httpd<Enter>` corre `/bin/httpd`). Typos siguen mostrando
     "unknown command".

✓ envp end-to-end (kernel + libc + shell)
   - **Kernel**: build_argv_block extendido para empaquetar argc /
     argv[] / envp[] / strings en la user stack (Linux SysV init
     layout). MAX_ENVP=32, total block cap 2 KiB. proc_execve
     (nuevo) toma `const char *const *envp`; proc_exec wrapper
     pasa NULL.
   - **Shell**: shell_env[16] static array de "KEY=VAL" lines.
     Defaults seteados en shell_server_init: PATH=/bin,
     HOME=/home, PWD=/home, SHELL=/bin/osh, TERM=osnos. cmd_cd
     ahora actualiza PWD. shell_env_snapshot() arma char**
     NULL-terminado en la stack para pasar a proc_execve.
   - **Shell commands**: `env` (lista todas), `export KEY=VAL`,
     `unset KEY`.
   - **crt0.S**: lee envp del stack (rsp+16+8*argc), lo asigna al
     global `environ` con `movq %rdx, environ(%rip)` ANTES de
     llamar main. La global vive en .data inicializada a NULL.
   - **libc**: `extern char **environ;` declarado en stdlib.h.
     - getenv(name): walk de environ, busca match "name=", devuelve
       el puntero a value.
     - setenv(name, value, overwrite): malloc'a "KEY=VAL", llama
       putenv.
     - putenv(kv): on first mutation hace `env_takeover` (malloc
       un array fresco, dup todos los strings del kernel-supplied
       envp). Luego inserta o reemplaza.
     - unsetenv(name): shift down (sin free para no liberar memoria
       de origen incierto, fuera del scope hoy).
   - **elfs/tests/envtest.c**: dump environ + setenv/unsetenv smoke
     test. `envtest PATH` imprime "PATH=/bin". `envtest` solo lista
     todo lo inherited.

✓ libc exec family + execvp PATH walk
   - **lib/libc/unistd.h**: declara `execv`, `execve`, `execvp` con
     la signatura POSIX.
   - **lib/libc/unistd.c**:
     - `execve(path, argv, envp)` → SYS_EXECVE (#59). Hoy el kernel
       no dispatcha esa syscall así que retorna -ENOSYS, pero la
       surface API queda lista para cuando el syscall aterrice.
     - `execv(path, argv)` envuelve execve con `environ`.
     - `execvp(file, argv)`: si `file` tiene '/' va directo. Sino,
       walkea `getenv("PATH")` (default "/bin") colon-separated y
       prueba execve en cada "<dir>/<file>". POSIX: el primer error
       != ENOENT gana.
   - **TODO sys_execve real**: necesita coordinación con el
     fg_pid tracking del shell (mantener el mismo pid al
     reemplazar la imagen). Postponed — no bloquea nada inmediato
     porque el shell ya hace proc_execve kernel-internal para
     spawn de niños.

### FASE shell rc + history persistente — CERRADA
✓ .history (historial persistente) + .oshrc (startup script)
   - **history_save** ahora hace vfs_append("/home/.history", line+"\n")
     después del push al ring en memoria. Guard `history_persistent`
     evita que el flag se grabe a sí mismo durante el load inicial.
   - **load_persistent_history()** se ejecuta al inicio del shell:
     vfs_read del archivo, parsea por '\n', cada línea va por
     history_save (con flag off) reusando dedup + ring shift.
     Soporta hasta 8 KiB de buffer; las últimas HISTORY_MAX (=16)
     entries quedan disponibles en flechas up/down.
   - **run_oshrc()**: lee /home/.oshrc al boot, trim leading
     whitespace, ignora líneas vacías y comments (`#`). Cada línea
     se pasa por run_command como si la hubiera tipeado el user.
     Guard `oshrc_running` previene reentrancia. `silent_mode` flag
     hace `prompt()` no-op durante el replay → el boot queda limpio.
   - **bootstrap_fs**: seed_if_absent crea /home/.oshrc con sample
     comentado en primer boot (FAT path). En diskless seedea uno
     mínimo en ramfs. .history se crea al primer comando que
     ejecute el usuario (vfs_append crea el archivo si falta).
   - **Naming**: .history (compat semantic con bash/zsh: nombre
     común, contenido propio), .oshrc (claramente del osnos shell).
   - Survivability: con FAT, ambos persisten via /home → /sd/home.
     Tipo en un boot, recuperá con flecha ↑ tras reboot. .oshrc
     queda como el lugar natural para "cd a tu workdir" o
     "export VARS" automáticos.

### FASE Job control (Ctrl+Z / fg / bg / jobs) — CERRADA
✓ TASK_STOPPED + stop_pending
   - Nuevo estado `TASK_STOPPED` en `task_state_t`. Scheduler
     ya lo skipea (chequeo existente `state != TASK_READY`).
   - `task_t.stop_pending` (int): set por el TTY cuando entra
     Ctrl+Z. user_task_trampoline lo chequea ANTES de
     user_task_resume y, si está set, transiciona state→STOPPED
     y `sched_resume_jump()` — la task queda con `saved_iret_*`
     intacto, listo para reanudar al pasar a READY.

✓ VSUSP en TTY (Ctrl+Z = 0x1A por default)
   - `TTY_VSUSP = 10` agregado a c_cc indices.
   - `tty_init`: `c_cc[VSUSP] = 26` (^Z). Configurable via
     `tcsetattr` como cualquier control char.
   - `tty_input` (ISIG path): detecta VSUSP y llama
     `tty_stop_signal()` que: set fg task's stop_pending,
     emite IPC_PROC_STOPPED al shell, bump signal counter.

✓ IPC_PROC_STOPPED + IPC_PROC_CONTINUED
   - Nuevos opcodes 0x41 / 0x42 en `ipc.h`.
   - shell_server handler: si arg1 == fg_pid, drop fg_pid +
     pipeline_clear. Imprime "[pid] stopped" en gris claro.

✓ Shell builtins: jobs / fg / bg
   - `jobs`: walk task table, lista user tasks con pml4 != NULL
     y state != UNUSED/DEAD, formato bash-like:
     `[pid] state  name`. Empacado en un IPC.
   - `fg [PID]`: encuentra stopped task (PID explícito o highest-
     pid stopped) → state=READY + fg_pid=pid. NO redibuja
     prompt — el child es foreground; el prompt vuelve cuando
     exit/se stops de nuevo.
   - `bg [PID]`: misma búsqueda → state=READY pero fg_pid
     intacto. Prompt sí vuelve.
   - `find_stopped_task` helper compartido: parsea PID arg,
     valida que sea user task STOPPED, fallback al "más reciente"
     (highest-pid heuristic).

✓ Probado en QEMU
   - `sleep 30` + Ctrl+Z → `[N] stopped`.
   - `jobs` → lista entry.
   - `fg` → reanuda, sigue durmiendo.
   - Ctrl+Z → stop otra vez.
   - `bg` → reanuda en background, prompt vuelve.

### FASE mmap anónimo — CERRADA
✓ SYS_MMAP (#9) + SYS_MUNMAP (#11) — anonymous only
   - `sys_mmap(addr, len, prot, flags, fd, off)`:
     - Acepta `MAP_ANONYMOUS | MAP_PRIVATE` (o SHARED, mismo trato).
     - File-backed (`fd != -1` o sin MAP_ANONYMOUS): -ENOSYS.
     - MAP_FIXED y `addr` hint: ignorados — la ubicación es el
       bump cursor del task.
     - Bump-allocator entre USER_MMAP_BASE (`0x20000000`) y
       USER_MMAP_LIMIT (`0x40000000`, 1 GiB window).
     - Zero-fills cada página antes de mapear (POSIX guarantee).
     - PROT_WRITE → PTE_W; PROT_NONE/READ son default.
     - Unwind clean: si falla pmm_alloc o vmm_map en medio,
       libera lo ya mapeado.
   - `sys_munmap(addr, len)`: busca región por addr exacto en
     `task_t.mmap_regions[16]`. Si match, unmap + free pages,
     marca slot vacío. Partial unmaps NO soportados (POSIX
     permite; complicaría bookkeeping).
   - `task_t.mmap_regions[16]` + `mmap_next` cursor. Slot
     recyclable después de munmap pero VA no se reusa (bump
     allocator simple).

✓ Cleanup en proc_exit_current_user
   - Las páginas físicas las libera `address_space_destroy`
     porque viven en el pml4 del task. Bookkeeping se zerea
     para que un recycle del slot vea task limpio.

✓ libc + headers
   - `lib/libc/include/sys/mman.h` con PROT_* / MAP_* / MAP_FAILED.
   - `lib/libc/mman.c` wrappers thin sobre osnos_syscall6/2.

✓ Test /bin/mmaptest (12 checks)
   - mmap básico 12 KiB + zero-init verificado en 3 offsets.
   - Scribble + read-back todos los bytes.
   - munmap → re-mmap fresh memory está cero otra vez.
   - File-backed mmap rechazado con ENOSYS.

### FASE Multi-stage pipes + O_NONBLOCK runtime — CERRADA
✓ `proc_execve_pipeline(paths[], args[], n, envp, pids_out)` kernel
   - Reemplaza `proc_execve_pipe` (2-stage) por versión N-stage
     hasta MAX_PIPELINE_STAGES=4. Crea N-1 pipes, spawnea N tasks
     en orden, wira pipe_in/pipe_out por slot. Partial-failure
     teardown: kill_pending para los ya spawneados, close ambas
     refs de pipes creados.

✓ Shell parser N-stage en run_pipeline
   - Reemplaza el split bipartito por NUL-separation de la línea
     completa en cada `|`. Tokeniza cada stage (cmd + args), valida
     que cada cmd resuelva a un ELF (mensaje claro con el nombre
     fallando), y llama proc_execve_pipeline.
   - `pipeline_upstream_pids[MAX-1]` reemplaza el `pipeline_lpid`
     scalar — `pipeline_owns_upstream(pid)` chequea contra el array
     en el IPC_PROC_EXITED handler para swallow silencioso.

✓ O_NONBLOCK runtime real (libc-side, sin syscall extra)
   - `lib/libc/unistd.c`: array static `_libc_nonblock[32]`. open()
     setea según flag; fcntl(F_SETFL) actualiza; close() limpia.
   - read() chequea el bit ANTES de loopear en EAGAIN. Si está
     set, sale al primer EAGAIN con errno=EAGAIN. Si no, loop
     con nanosleep como antes.
   - write() no loopea — ya devuelve EAGAIN directo del kernel,
     que el caller maneja.
   - Caveat: el cache vive en libc del task. Si dos tasks
     comparten el mismo fd global (que pasa hoy), cada uno tiene
     su cache independiente. Real per-task flags están en
     osnos_fd_t.flags; el cache es una optimización para evitar
     fcntl(F_GETFL) en cada read.

✓ echo -e / -n (POSIX behavior)
   - `elfs/tools/echo.c` ahora parsea flags coreutils-style.
   - `-e`: interpreta `\n`, `\t`, `\r`, `\b`, `\\`, `\"`, `\a`, `\0`.
     Sin esto el shell no podía emitir multilínea via echo, así que
     pipes con `\n` separators no funcionaban.
   - `-n`: suprime el newline trailing (`echo -n hola > f` deja
     "hola" sin el `\n` final).
   - `-E`: forzar literal (default, compat).
   - Default sin flag → comportamiento previo intacto.
   - `cmd_echo` shell-builtin replica la lógica (`-e` / `-n`)
     porque sin `|`/`<`/`>` el dispatcher va al builtin, no al ELF.
     `echo_unescape()` helper compartido entre el path "no
     redirect" y "redirect a archivo".

✓ Pipes + redirects mezclados — run_pipeline extendido
   - **Primer stage** puede tener `< file` (stdin desde archivo).
   - **Último stage** puede tener `> file` o `>> file` (stdout a
     archivo). Pre-truncate el archivo de output cuando es `>` y
     ya existe.
   - **Middle stages** NO permiten redirects: validación pre-spawn
     reporta "pipe: middle stage cannot redirect" sin tocar nada.
   - **Path resolve** a absoluto vs cwd ANTES de pasar al task —
     `cat < x.txt` desde /home busca /home/x.txt.
   - **Post-spawn** mete `stdin_redir` en pids[0] y `stdout_redir`
     en pids[n-1]. La prioridad pipe > file > TTY en sys_read/
     write deja todo coherente: primer stage no tiene pipe_in
     (redirect gana), último stage no tiene pipe_out (redirect
     gana). Nada conflicta.
   - Casos validados manualmente:
     `cat < x.txt | head -n 2`,
     `head -n 2 > z.txt | head` (debería rechazar middle),
     `cat < a.txt | head -n 3 | head -n 1 > b.txt` (3-stage
     con ambos extremos redirected).

### FASE FXSAVE/FXRSTOR per-task — CERRADA
✓ Task struct field — task_t.fpu_state[512] aligned 16
   - Buffer FXSAVE/FXRSTOR-compatible embebido en task_t. El
     atributo `__attribute__((aligned(16)))` propaga a task_t
     mismo y el array static, así que cada slot del pool queda
     naturalmente alineado.

✓ Helpers en src/micro/fpu.{c,h}
   - `fpu_save(state)` — `fxsaveq (state)`.
   - `fpu_restore(state)` — `fxrstorq (state)`.
   - `fpu_state_init(state)` — FNINIT + LDMXCSR(0x1F80) + FXSAVE
     a `state`. Captura una imagen "default FPU" para sembrar el
     primer FXRSTOR de un task recién creado (sino FXRSTOR de
     bytes uninitialised podría tirar #XM).

✓ Hook en task_run_next
   - ANTES de buscar el próximo READY: snapshot el task saliente
     vía `fpu_save(prev->fpu_state)`. Los HW regs siguen con lo
     que el user dejó al entrar en kernel (timer IRQ o syscall);
     hay que capturarlo antes de que FXRSTOR del nuevo task los
     pisotee.
   - DESPUÉS de encontrar el READY: si current_index cambió,
     `fpu_restore(task->fpu_state)`. Same-task dispatch salta el
     restore (HW regs ya correctos).

✓ Seed en task_create_user_elf
   - `fpu_state_init(t->fpu_state)` después de task_clear. Garantiza
     que la primera dispatch de un task fresh carga "FPU limpio"
     en vez de bytes uninitialised.

✓ printf %f / %g (libc enhancement)
   - `lib/libc/stdio.c`: agregado parse de precision (`.N`) y
     handler para `%f %F %g %G`. Round-half-up a `precision`
     dígitos (default 6). Split int / fractional vía `(long
     long)`. Sin notación exponencial (— %g degenera a %f para
     magnitudes razonables).
   - Sin esto, /bin/fptest mostraba literal "a=%.6f" porque el
     vararg de double se consumía pero no se imprimía.

✓ ELF /bin/fptest para validación
   - `elfs/tests/fptest.c`: N rondas (default 200) de sin/cos/
     sqrt/fabs sobre valores semilla, con nanosleep(1ms) entre
     iteraciones para forzar preemption.
   - **Test crítico**: correr 5 instancias en paralelo:
     `fptest 500 & fptest 500 & fptest 500 & fptest 500 & fptest 500`.
     Sin FXSAVE per-task, los outputs divergen (FP regs se
     contaminan entre context switches). Con FXSAVE, los 5 dan
     el mismo `a=1.481451 b=1.001098 c=50.477837` bit-exact.

### FASE Shell pipes `cmd1 | cmd2` — CERRADA
✓ Pipe kernel object — src/micro/pipe.{c,h}
   - Pool estático de 16 `pipe_t`. Cada uno: ring buffer 4 KiB
     (POSIX PIPE_BUF mínimo), head/tail/level cursores, refcounts
     `ref_w` y `ref_r`.
   - `pipe_create()`: reserva slot con ref_w=ref_r=1. Las refs
     pertenecen a las dos tasks que el shell va a spawnear.
   - `pipe_write(p, buf, n)`: copia bytes al ring. Retorna `n`
     bytes aceptados (puede ser menos que pedido si lleno).
     -EAGAIN si lleno + readers vivos. -EPIPE si no quedan
     readers (writer escribiendo a pipe huérfano).
   - `pipe_read(p, buf, n)`: drena del ring. Retorna 0 (EOF) si
     buffer vacío + writers cerrados. -EAGAIN si vacío + writers
     vivos.
   - `pipe_close_writer/_reader`: dec refcounts. Cuando ambos
     llegan a 0, el slot se libera al pool.
   - `pipe_init()` llamado desde kmain después de ipc_init.

✓ Task pipe endpoints — task_t.pipe_in / pipe_out
   - Punteros opcionales en `task_t`. Quien tiene `pipe_out !=
     NULL` es upstream (escribe al pipe); `pipe_in != NULL` es
     downstream (lee del pipe).
   - `sys_write(fd=1)`: prioridad **pipe_out → stdout_redir →
     console**. La primera no-NULL gana.
   - `sys_read(fd=0)`: prioridad **pipe_in → stdin_redir → TTY**.
   - `proc_exit_current_user` llama pipe_close_*er por cada
     endpoint para que el peer vea EOF/EPIPE en lugar de loop
     infinito en EAGAIN.

✓ proc_execve_pipe — helper kernel para spawnear pipeline
   - `proc_execve_pipe(left_path, left_args, right_path, right_args,
     envp, left_pid_out)`:
     1. pipe_create() → pipe.
     2. proc_execve(left) → lpid. Setea lt->pipe_out = pipe.
     3. proc_execve(right) → rpid. Setea rt->pipe_in = pipe.
     4. Retorna rpid (downstream — su exit marca fin pipeline).
   - Si left fail → cierra ambas refs (pipe libre). Si right fail
     → mata left + cierra ambas refs. Ningún recurso queda colgado.

✓ Shell parser y dispatch
   - `run_command`: short-circuit por `|` antes que redirects o
     builtins. Split en el primer `|` (greedy), trim ambos lados,
     llama `run_pipeline`.
   - `run_pipeline`: parse cada lado en (cmd, args). Verifica que
     ambos sean ELFs registrados (sino "pipe: at least one stage
     is not an ELF" — typos atrapados temprano). Resolve_bin
     mapea "cat" → "/bin/cat". Llama proc_execve_pipe.
   - `pipeline_lpid` static guarda el upstream pid. El handler
     `IPC_PROC_EXITED` lo descarta silenciosamente cuando muere
     (no imprime "[pid] done"), mientras espera al downstream
     (que es el fg_pid).

✓ Limitaciones documentadas
   - **Solo 2-stage** (`a | b`). `a | b | c` reporta error —
     necesitaría multi-stage pipeline scheduler.
   - Pipe sin redirects: `a < in.txt | b > out.txt` no parsea
     todavía (parser de `|` corre antes que el de redirects).
   - Sin pipe(2) syscall expuesto: pipes son internal del shell.
     Cuando lleguen per-task fd tables, exponer pipe(2) será un
     wrapper trivial.

✓ /bin/head ELF para probar pipes
   - `head [-n N] [FILE]`: prime N líneas (default 10) de stdin
     o files. Útil para `cat /home/x.txt | head -n 2`.

### FASE Shell redirection (> >> <) — CERRADA
✓ Per-task stdin/stdout redirect path en task_t
   - `task_t` agrega `stdin_redir[OSNOS_PATH_MAX]`,
     `stdout_redir[OSNOS_PATH_MAX]`, `stdout_append`,
     `stdin_redir_off`, `stdout_redir_off`. Vacíos por default →
     comportamiento normal TTY/console.
   - `sys_write(fd=1)`: si task->stdout_redir set, escribe vía
     `vfs_append` al archivo. Sino, va al console_server como
     siempre.
   - `sys_read(fd=0)`: si task->stdin_redir set, lee del archivo
     con offset tracking + retorna 0 (EOF) al exhaurirse.

✓ proc_execve_redir helper en src/proc/exec.{c,h}
   - Wrapper sobre `proc_execve` que: pre-trunca/crea el stdout
     file (honra `>` vs `>>`), valida que stdin_path exista,
     spawnea via proc_execve, y mete los paths en el task struct
     del child antes de que arranque.
   - Fast path: si ningún redirect, mismo costo que proc_execve.

✓ Parser en cmd_exec del shell
   - `extract_redir(buf, op, out)`: tokenizador in-place. Busca
     `>>` antes que `>` para evitar swallowing. Soporta:
     `cmd > file`, `cmd >> file`, `cmd < file`, combinaciones.
   - `absolutize_redir`: paths relativos resueltos contra cwd
     del shell.
   - Background (`&`) y redirects coexisten:
     `httpd > /home/log.txt &` funciona.

✓ Short-circuit en run_command
   - Si la línea contiene `<` o `>`, salta el dispatch de shell
     builtins (que escriben al console directo, no honran
     redirects) y va al exec ELF. Solo activa si el primer token
     matchea un ELF registrado — typos siguen surfaceando
     "unknown command" claro.

✓ Fix de /bin/cat para leer stdin
   - POSIX cat sin args lee stdin → stdout. Antes era "usage:
     cat FILE". Sin esto, `cat < file > other` no podía
     funcionar.

### FASE TCC prep (items 1-6) — CERRADA
✓ Path 1 — sys_write con offset real
   - **src/micro/syscall.c**: refactor de `sys_write`. Si
     `f->offset == existing_size` (o O_APPEND): fast path
     `vfs_append`. Else: read-modify-write con scratch kmalloc'd:
     vfs_read full file, zero-fill sparse hole entre EOF y offset,
     copia bytes, vfs_write con tamaño total. Cap 4 MiB → EFBIG.
   - **Bug colateral arreglado**: `ramfs_vfs_write/append` usaba
     `strlen` (string-oriented) — sparse-hole writes con NULs se
     truncaban. Nuevas APIs `ramfs_write_file_bin` /
     `ramfs_append_file_bin` con tamaño explícito.

✓ Path 2 — proc_execve acepta paths arbitrarios
   - **src/proc/exec.c**: si path no es /bin/X (o no hay builtin),
     `vfs_stat` + `vfs_read` el ELF a un scratch kmalloc'd y
     pasa a `task_create_user_elf` como blob in-memory. Cap 2 MiB.
     EISDIR/ENOEXEC/E2BIG según corresponda.
   - Permite `exec /sd/home/myprog` ahora que TCC genera ELFs en
     disco.

✓ Path 3 — User stack 64 KiB
   - **src/proc/elf.c**: `USER_STACK_PAGES=16`, allocates 16
     páginas contiguas en `[0x7FFF0000, 0x80000000)`. Antes era
     1 página (4 KiB) — TCC necesita decenas de KiB para parsing
     recursivo.

✓ Path 4 — FPU init para ring-3
   - **src/micro/fpu.{c,h}** (nuevo): `fpu_init()` setea
     CR0.EM=0/MP=1/NE=1/TS=0 + CR4.OSFXSR=1/OSXMMEXCPT=1 + FNINIT +
     LDMXCSR(0x1F80). Llamado desde `kmain` post syscall_msr_init.
   - **GNUmakefile USER_CFLAGS**: removido `-mno-sse -mno-sse2
     -mno-80387 -mno-mmx`. Kernel sigue `-mno-*` (compiled per-file).
     User ELFs ahora tienen x87+SSE2.
   - **Caveat documentado**: sin FXSAVE per-task, dos user tasks
     concurrentes con FP pueden corromper regs entre sí. Single-task
     FP funciona.
   - **Bonus**: `lib/libc/math.c` puede usar double real ahora —
     antes fallaba en undefined symbols __adddf3 etc.

✓ Path 5 — Headers libc faltantes
   - `lib/libc/include/limits.h` (nuevo): CHAR_BIT, INT_MAX,
     LONG_MAX, PATH_MAX, NAME_MAX, ARG_MAX, ...
   - `lib/libc/include/float.h` (nuevo): FLT_MAX, DBL_EPSILON,
     LDBL_MANT_DIG, ...
   - `lib/libc/include/signal.h` (nuevo): SIG* constants Linux +
     signal() / raise() decls.
   - `lib/libc/signal.c` (nuevo): signal() retorna SIG_ERR/ENOSYS
     para non-default; raise() → kill(getpid(), sig).
   - `lib/libc/include/ctype.h`: + isblank, isgraph, ispunct, isascii.

✓ Path 6 — math.h básico
   - `lib/libc/include/math.h` (nuevo): M_PI, M_E, M_LN2, INFINITY,
     NAN, isnan/isinf/isfinite, fabs/floor/ceil/trunc/round/fmod,
     sqrt/pow/exp/log/log2/log10, sin/cos/tan/atan/atan2, +
     float variantes.
   - `lib/libc/math.c` (nuevo, ~200 LOC): impls iterativas/Taylor.
     sqrt vía Newton, exp/log con range reduction, sin/cos con
     reducción mod 2π + Taylor. Accuracy "good enough" — no
     IEEE-precise.

✓ ELF /bin/tcc — placeholder (no es port real)
   - `elfs/tools/tcc.c`: stub que demuestra la cadena libc + stack
     big + FPU + exec funciona end-to-end. **NO compila C**. Real
     port movido a **ROADMAP_APENDICE.md → Self-hosting → TinyCC**
     porque requiere mmap, FXSAVE/FXRSTOR per-task, signal
     handlers reales, y un bootstrap tooling — ninguno bloquea el
     día a día del kernel. Las piezas que sí están listas para el
     futuro port (stack 64 KiB, FPU ring-3, write+offset, exec
     desde VFS, math/limits/float/signal headers) son útiles
     INDEPENDIENTEMENTE de TCC.

✓ Tests para los 6 items
   - **cmd_test (kernel)**: 11 nuevos:
     - WRITE_OFFSET × 8: open/write base, lseek+overwrite mid,
       read-back, sparse-hole past EOF, length post-extend.
     - EXEC_VFS × 3: blob written, pid allocated, missing → fail.
   - **libctest (user)**: 49 nuevos:
     - ctype × 9 (isalpha, isdigit, isblank, ispunct, isgraph,
       tolower, toupper, ...).
     - limits × 4 (INT_MAX, CHAR_BIT, PATH_MAX, LONG_MAX).
     - signal × 3 (sig numbers, SIG_DFL noop, non-default ENOSYS).
     - math × 18 (fabs, floor, ceil, fmod, sqrt×2, pow, sin×3,
       cos, exp×2, log×2, isnan/isinf/isfinite).
     - seek × 5 (write con offset libc-side).
     - stack × 2 (16k y 32k arrays automáticos — fail si stack
       sigue siendo 4 KiB).
   - **`test` ahora suma ~643**, **libctest ~110**. AUDIT.md
     documenta el flujo de verificación end-to-end.

### FASE Libc gaps 4+5+6 — CERRADA
✓ dup / dup2 — Linux syscalls #32 / #33
   - **src/micro/fd.{c,h}**: tres helpers nuevos sobre la fd table
     existente:
     - `fd_dup(src)` — alloca el primer slot libre >= 3 y copia el
       struct.
     - `fd_dup_min(src, min_fd)` — variante para F_DUPFD: busca el
       slot más bajo >= min_fd.
     - `fd_dup2(src, target)` — escribe en un slot específico;
       cierra el target si estaba abierto. `src == target` es
       no-op POSIX.
   - **Limitación documentada**: el clon recibe una COPIA de
     `path/flags/offset`. POSIX-strict dup requiere "open file
     description" compartida (mismo offset). Eso necesita un
     refcount separado de la fd table — TODO. En la práctica
     todos los user-mode redirection patterns (e.g. `dup2(fd, 1)`
     antes de exec) funcionan porque escriben/leen secuencialmente
     vía el mismo path.
   - **sys_dup/sys_dup2** envuelven los helpers y devuelven EBADF
     en caso de fd inválido.

✓ fcntl básico — Linux syscall #72
   - **Comandos soportados**: F_DUPFD (0), F_GETFD (1), F_SETFD (2),
     F_GETFL (3), F_SETFL (4). Cualquier otro → -EINVAL.
   - F_GETFD/F_SETFD: aceptan/devuelven 0 (sin FD_CLOEXEC todavía).
   - F_GETFL: retorna `f->flags` (los flags pasados a open).
   - F_SETFL: actualiza solo el mutable_mask = O_APPEND | O_NONBLOCK.
     Los flags de access mode + O_CREAT/O_EXCL/O_TRUNC quedan
     inmutables (POSIX correcto).
   - **Caveat documentado**: hoy O_NONBLOCK se almacena pero NO
     afecta el runtime de sys_read/sys_write. Para que tenga efecto
     real, los syscalls tendrían que chequear `f->flags & O_NONBLOCK`
     y devolver EAGAIN inmediato sin loop. Esto es trivial de
     agregar más adelante; por ahora la API queda lista.

✓ mkstemp / tmpfile (libc)
   - **lib/libc/stdlib.c**: `mkstemp(char *tmpl)` rewrites the
     trailing "XXXXXX" con un sufijo base-36 (seed = `time() *
     65537 + getpid() * 7919 + counter`). Loop hasta 100 attempts
     con O_RDWR | O_CREAT | O_EXCL; primera EEXIST → retry con
     nuevo seed; otro error → return -1 + errno.
   - **lib/libc/stdio.c**: `tmpfile()` llama mkstemp con template
     "/tmp/tmpf-XXXXXX" + wrap_fd() helper compartido con fopen.
   - **Limitación documentada**: el archivo NO se borra
     automáticamente al fclose (POSIX exige unlink-on-close vía
     "open file description anónima"). Cleanup queda como
     responsabilidad del caller — pair con `remove()` cuando
     se necesite.
   - **errno.h**: O_NONBLOCK (04000) agregado al fcntl.h libc.

✓ Test suite +28 checks
   - **cmd_test (kernel)**: 9 nuevos para DUP/DUP2/FCNTL (open base,
     dup fresh fd, dup2 target/self, fcntl F_GETFL/F_SETFL roundtrip,
     bad fd / cmd cases).
   - **elfs/tests/libctest.c**: 19 nuevos para dup/dup2/fcntl/
     mkstemp/tmpfile (dup-read both ways, dup-bad-fd EBADF,
     fcntl-roundtrip O_NONBLOCK, mkstemp-rewrote/stat,
     mkstemp-einval, tmpfile-write).
   - Total ahora: **624 / 624 pass**, idempotente.

### FASE Libc gaps 1+2+3 — CERRADA
✓ strerror completo (33 entries)
   - **lib/libc/string.c**: agregados 16 cases nuevos al switch:
     EINTR, E2BIG, EBUSY, ENFILE, ENOTTY, ERANGE, ENAMETOOLONG,
     ENOTSOCK, EPROTONOSUPPORT, EAFNOSUPPORT, EADDRINUSE,
     EADDRNOTAVAIL, ENETDOWN, ECONNRESET, ETIMEDOUT, ECONNREFUSED,
     EINPROGRESS.
   - **lib/libc/include/errno.h**: agregados EINTR (4), ENOTTY (25),
     ERANGE (34) que faltaban.
   - Fallback "errno=N" itoa para códigos desconocidos sigue intacto.

✓ stat(path) + access — Linux syscalls #4 y #21
   - **SYS_STAT (#4)**: `sys_stat(const char *path, void *out)` —
     mismo flujo que sys_fstat pero recibe path. copy_from_user
     del path, vfs_stat, llena osnos_stat_t con type|mode/size/
     blocks, copy_to_user. Devuelve 0 / -ENOENT / -EFAULT.
   - **SYS_ACCESS (#21)**: `sys_access(path, mode)` — wrap thin de
     vfs_stat. Mode argument (R_OK/W_OK/X_OK/F_OK) aceptado pero
     ignorado (no enforcement de perms aún). Solo distingue
     "existe" vs ENOENT.
   - **libc**: `stat(path, struct stat *out)` y `access(path, mode)`
     en unistd.c. Ambos usan resolve_path para relativos.
     `unistd.h` define F_OK / R_OK / W_OK / X_OK.

✓ time(NULL) + clock_gettime — Linux syscalls #201 y #228
   - **SYS_TIME (#201)**: `sys_time(int64_t *t)` — retorna
     `timer_ms() / 1000`. Si `t != NULL`, copia el mismo valor ahí
     vía copy_to_user. Sin RTC: segundos desde boot, no desde epoch.
   - **SYS_CLOCK_GETTIME (#228)**: `sys_clock_gettime(clk_id, void *tp)` —
     acepta CLOCK_REALTIME=0 y CLOCK_MONOTONIC=1 (ambos devuelven
     timer_ms hoy; sin RTC). Otros clk_id → -EINVAL. Llena
     `struct osnos_timespec {tv_sec, tv_nsec}` con tv_sec=ms/1000
     y tv_nsec=(ms%1000)*1000000.
   - **libc/time.c** (nuevo): `time(t)` y `clock_gettime(clk_id, tp)`
     wrappers thin.
   - **CLOCK_REALTIME=0 / CLOCK_MONOTONIC=1** en time.h.
   - **SYS_ISATTY** movido de 201 → **250** (osnos-only) porque
     colisionaba con Linux SYS_TIME=201.

✓ Fix de ABI nlink_t — bug oculto que afectaba stat real
   - **sys/types.h**: `nlink_t` de `uint32_t` → **`uint64_t`** para
     matchear `osnos_stat_t.st_nlink` (Linux x86_64 layout).
   - Antes: libc struct stat tenía nlink en 4 bytes, kernel en 8 →
     `st_mode` quedaba 4 bytes desalineado → S_ISREG retornaba false
     siempre.

✓ Fix uaccess para kernel callers — habilita tests internos
   - **src/micro/uaccess.c**: `copy_from_user` / `copy_to_user` ahora
     detectan caller kernel (`task_current()->pml4 == 0`) y saltean
     el chequeo de rango. Antes, syscalls que usaban copy_*_user
     fallaban con EFAULT cuando se invocaban desde el shell
     (kernel task) con string literals (que viven en high half).
   - Esto es un workaround para los tests integrados; user tasks
     siguen pasando por la validación completa.

✓ Test suite ampliado a 615 + idempotente
   - **cmd_test (kernel)**: 12 nuevos checks (STAT existing/missing,
     mode regular, ACCESS, TIME positive + out-param, CLOCK
     REALTIME/MONOTONIC + sane + bogus id).
   - **elfs/tests/libctest.c**: 23 nuevos checks user-side
     (strerror×11, stat×3, access×2, time×2, clock_gettime×4,
     fallback errno=N).
   - **Fix idempotencia**: KHEAP "total grew past baseline" y
     "grow_events incremented" eran one-shot. Ahora son `>=`
     (monotonically non-decreasing) → el comando `test` se puede
     correr N veces seguidas. Anteriormente la 2da corrida
     reportaba 2 falsos negativos.

### FASE Arrow keys + getcwd/chdir — CERRADA
✓ Keycode passthrough: arrow keys via TTY como VT100 CSI
   - **keyboard_server**: cuando llega un keycode != 0 (special key),
     traduce a `ESC [ A/B/C/D` (3 bytes) y los pushea a `tty_input`
     en orden atómico. KEY_UP→A, KEY_DOWN→B, KEY_RIGHT→C, KEY_LEFT→D.
   - El shell sigue recibiendo IPC_KEY_EVENT (su history nav usa
     keycodes directos via IPC, no via TTY). Cuando un user ELF es
     fg, el shell descarta IPC_KEY_EVENT (FASE shell rc) y los
     bytes ESC fluyen via TTY al ELF.
   - **/bin/ovi**: nuevo helper `peek_byte_nonblock` usa
     `select(stdin, timeout=0)` antes de leer el byte siguiente
     después de un ESC. Sin esto, ESC solo (sin follow-up) bloquearía
     esperando. Con peek: detecta ESC + [ + letter atómicamente
     (keyboard_server los empuja juntos), o cae a "ESC solo →
     NORMAL" si no hay bytes pendientes.
   - Flechas funcionan tanto en NORMAL (mapeadas a hjkl uniformemente)
     como en INSERT (sin abandonar el modo).

✓ SYS_GETCWD (#79) + SYS_CHDIR (#80) — per-task cwd
   - **task_t** agrega `char cwd[OSNOS_PATH_MAX]`. Default tras
     `task_clear`: "" (vacío). Cero-overhead para kernel tasks que
     no lo usan.
   - **task_create_user_elf** ahora walka envp buscando "PWD=" al
     spawn y siembra `t->cwd` con ese valor; sino, "/". Eso preserva
     herencia POSIX-like del cwd vía la env var clásica.
   - **sys_getcwd(buf, size)**: copia `t->cwd` a userland via
     `copy_to_user`. Retorna `len + 1` (incluye NUL, como Linux).
     ERANGE si `size <= len`. EFAULT si pointer inválido.
   - **sys_chdir(path)**: copy_from_user, vfs_stat, verifica
     `type == VFS_NODE_DIR`. ENOTDIR si archivo regular, ENOENT
     si no existe. Si OK, `os_strlcpy` a `t->cwd`.
   - **libc** wrappers: `getcwd(buf, size)` y `chdir(path)`.
   - **libc resolve_path** ahora prefiere getcwd; fallback getenv("PWD")
     si syscall falla (kernels viejos); fallback final "/".
   - Resultado: `ovi .oshrc` desde cualquier cwd resuelve correcto.
     `chdir` dentro de un user task afecta al cwd persistentemente
     hasta el exit del task.
   - **ERANGE = 34** agregado al status enum.

### FASE /bin/ovi — Editor modal vim-style — CERRADA
✓ /bin/ovi — primer editor visual de osnos
   - **Framebuffer VT100 extendido**:
     - `ESC[2J` / `ESC[J` — clear screen.
     - `ESC[H` — cursor home.
     - `ESC[<row>;<col>H` — cursor positioning (1-based).
     - `ESC[K` — clear from cursor to EOL.
     - `ESC[7m` / `ESC[27m` / `ESC[0m` — SGR reverse video / reset
       (usado por ovi para pintar el cursor sobre la celda actual).
   - **Dimensiones terminal expuestas**:
     - `framebuffer_cols()` / `framebuffer_rows()` — área usable en
       chars (descuenta márgenes).
     - `ioctl(TIOCGWINSZ=0x5413, &struct winsize)` syscall — ovi y
       futuros TUIs leen el tamaño real al boot.
   - **/bin/ovi** (elfs/tools/ovi.c, ~500 LOC):
     - 3 modos: NORMAL (default) / INSERT / COMMAND.
     - Normal: `hjkl` navegación, `0`/`$` line-start/end, `gg`/`G`
       file-start/end, `i`/`a`/`o`/`O` insert variants, `x` delete
       char, `dd` delete line, `:` enter command mode.
     - Insert: tipear inserta, `Esc` vuelve a Normal, `\b` borra.
     - Command: `:w` save, `:q` quit (refusa si dirty), `:wq` save+
       quit, `:q!` force quit.
     - Buffer: 4096 líneas × 1024 cols en BSS (4 MiB).
     - Cursor visible: render() pinta el char bajo el cursor con
       SGR reverse on. Sin parpadeo pero sí destacado.
     - Status line con `[MODE] file *  row/total  col N` + mensajes
       transitorios (cargo / guardado / errno).
     - Lee termios al boot, switch a raw + ECHO off + ISIG off (Ctrl+C
       lo matamos vía save → no, sigue funcionando ISIG porque
       tcsetattr no la apaga). Restaura al exit.
   - **libc path resolution** (lib/libc/unistd.c):
     - `resolve_path(in, out)` helper estático: si `in` empieza con
       `/`, copia tal cual; sino, antepone `$PWD` (con `/`
       intermedio si hace falta).
     - `open`, `mkdir`, `rmdir`, `unlink`, `rename` ahora resuelven
       paths relativos antes de la syscall.
     - Sin esto, `ovi .oshrc` desde `/home` fallaba con EINVAL (el
       kernel VFS rechaza paths no-absolutos). A futuro: getcwd/
       chdir syscalls per-task para POSIX-correctness.

### FASE /home alias — CERRADA
✓ aliasfs (bind-mount VFS backend) + /home → /sd/home
   - **src/fs/aliasfs.{c,h}**: nuevo backend con un translate()
     interno que sustituye `mount_prefix` por `target_prefix` en
     cualquier path entrante. Todas las ops (stat/readdir/read/
     write/mkdir/rmdir/unlink/append/rename) traducen + delegan
     vía vfs_*, así que cualquier backend abajo sirve (FAT16 en
     este caso). Lifecycle: aliasfs_t en BSS del bootstrap, vive
     forever junto con su mount slot.
   - **src/fs/bootstrap.c**:
     - Con FAT montado: vfs_mkdir("/sd/home"); aliasfs_init mapea
       /home → /sd/home; vfs_mount("/home", &aliasfs_ops, slot).
       Seed de README.TXT / HELLO.TXT solo si ausentes
       (seed_if_absent: stat-then-write) — preserva ediciones
       entre reboots.
     - Sin FAT: ramfs como antes (path corto).
   - Efecto user-visible: `echo persistente > /home/note` queda
     en /sd/home/note y sobrevive reboots. `ls /home` y `ls
     /sd/home` muestran lo mismo.

### Reorganización elfs/ (desde tests/)
   tests/ se renombró a elfs/ con subcategorías:
     elfs/shell/     osh
     elfs/tools/    ls, cat, cp, mv, rm, mkdir, rmdir, touch, echo,
                     true, false, sleep, kill, top, calc, init, hello
     elfs/net/       tcpclient, udptest, echotcp, selecttest,
                     selectserver, httpd
     elfs/tests/     libctest, ttytest, hello_libc, user_hello
                     (+ user_hello.lds)
     elfs/osn-server/ placeholder FASE 10 (servers a ring 3)
     elfs/libc.lds   linker script compartido
   GNUmakefile: pattern rule `$(BUILD)/elfs/%.elf: elfs/%.c ...`
   maneja cualquier subdirectorio. objcopy cd's al dir de salida
   así los símbolos siguen siendo `_binary_<basename>_elf_start`
   (basenames únicos entre categorías).

### FASE 9 — Scheduler real — CERRADA
✓ 9.1 timer IRQ infra — VERIFICADO en QEMU
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
✓ 9.2 SYS_NANOSLEEP (kernel-mode hlt-loop) — superado por 9.3
   - quedaba como hlt-loop solo en la rama kernel-mode (tests).
   - el resto migró a suspend real con context save/restore (9.3).

✓ 9.3a — Voluntary suspension via saved state — VERIFICADO en QEMU
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

✓ 9.3 bugfix CRITICO: sti en sched_resume_jump
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

✓ 9.3 diagnósticos
   - task_wakeups_fired() counter
   - /sys/timer incluye "wakeups fired" + lista de BLOCKED tasks con
     pid + wakeup_at_ms + saved_valid (útil para debug de sleep)

✓ 9.3b — preempción timer-driven (CPL=3 only) — VERIFICADO en QEMU
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
✓ 9.4 Ctrl+C live + /bin/kill — VERIFICADO en QEMU
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

✓ 9.4 extensión: kill sobre BLOCKED + shell builtin — VERIFICADO en QEMU
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

✓ 9.4 background jobs (`&`) — VERIFICADO en QEMU
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

✓ 9.4 SYS_GETPID + libc getpid — VERIFICADO en QEMU
   - SYS_GETPID = 39 (Linux number). syscall_dispatch case sin args.
   - sys_getpid retorna task_current()->pid; 0 si no hay task
     (kernel-mode caller, no debería ocurrir desde user).
   - libc: pid_t getpid(void) en unistd.h.
   - Usado por /bin/top para mostrar su propio pid en el header.

✓ 9.4 /bin/top — viewer live — VERIFICADO en QEMU
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

### FASE 10 — Servers a ring 3 — CERRADA
Pre-reqs cerrados (10.0 entera, sesión 2026-05-21):
- 10.0.a per-task fd tables (osnos_fd_t fds[16] dentro de task_t)
- 10.0.b pipe(2) syscall + pipeline migrado a fds reales
- 10.0.c /dev/fb0 + /dev/input0 char devices
- 10.0.d osnos_ipc_abi.h compartido kernel↔ring-3
- 10.0.e SYS_TASKINFO + /bin/kerntest ELF

Servers migrados / eliminados:
- ✓ 10.1 console_server → elfs/osn-server/consrv.c
- ✓ 10.2 keyboard_server → elfs/osn-server/kbdsrv.c
- ✓ 10.3 fs_server ELIMINADO (-290 LOC)
- ✓ 10.4 shell_server → elfs/osn-server/shellsrv.c (-3375 LOC)

**EL OS ENTERO** (consola + teclado + shell) corre en ring 3. kmain solo arranca drivers + spawn de ELFs + scheduler.

Polish + cleanup ya cerrado:
- ✓ shellsrv `cd` con `..` / `.` / paths relativos (path_normalize)
- ✓ `ls /` muestra mount points sintéticos (vfs_readdir injection)
- ✓ /bin/clear + /bin/tree ELFs agregados
- ✓ ovi: output buffering 16KB + framebuffer chunk safe-split (CSI sequences no se parten)
- ✓ FAT16 dir-chain extension (extend_dir_chain) — supera el cap de ~9 entries
- ✓ FAT16 NT case-bits (byte 0x0C) respetados — lowercase SFNs como hello/head/httpd
- ✓ task_t.name inline 32B (sys_taskinfo ya no faulta con user-pointer leaks)
- ✓ ipc_send rewrite SID→pid (ring-3 receivers filtran por pid)
- ✓ kbdsrv no envía IPC_KEY_EVENT (shellsrv lee via raw TTY)
- ✓ shellsrv $VAR / ${VAR} expansion + export/unset/setenv + .oshrc autoload
- ✓ shellsrv `;` `&&` `||` operators con `$?` tracking
- ✓ shellsrv glob `*` (matcher + walk dir + push argv)
- ✓ shellsrv `do_ls` POSIX multi-arg (files + dirs con headers)
- ✓ shellsrv `exec` builtin con osn_set_fg para Ctrl+C post-execve
- ✓ opendir valida ENOTDIR (antes silent no-output en files)
- ✓ task_reap_dead grace 4 pasadas (waiters capturan exit_code de fast tasks)
- ✓ kernel_fg_pid clear en proc_exit_current_user
- ✓ init-respawn watchdog (consrv/kbdsrv/shellsrv auto-restart)
- ✓ Disk-resident /bin (Fase 1) — bootstrap dumpea ELFs a /sd/bin + aliasfs
- ✓ **Fase 2 disk-resident FINAL** — sd.img poblado al build via mtools (64 ELFs), kernel binary 7.6 MB → 1.1 MB (-85%)
- ✓ **SYS_EXECVE (#59)** + libc execve/execv/execvp + /bin/exectest
- ✓ **SYS_FORK (#57)** + address_space_clone + pipe_dup_reader/writer + libc fork() + /bin/forktest.
- ✓ **SYS_WAIT4 (#61)** + TASK_ZOMBIE state + libc wait/waitpid + W* macros + /bin/waittest.
- ✓ **SYS_RT_SIGACTION (#13) + SYS_RT_SIGRETURN (#15)** + signal delivery en user_task_resume + libc sigaction + sigtramp.S + /bin/sigtest. EINTR en blocking syscalls. **ABI POSIX core 100% COMPLETO** verificado por tests reales:
    - **sigtest 9/9 PASS** — sigaction install / SIG_IGN / SIGKILL-EINVAL / signal() BSD wrapper / fork+SIGTERM → WIFSIGNALED+WTERMSIG=15
    - **waittest 8/8 PASS** — 3 children con códigos distintos, WNOHANG/ECHILD, waitpid-by-pid
    - **forktest 6/6 PASS** — migrado a waitpid real; pid distinct + fd inheritance + stack independence
    - Total: **23/23 PASS**. Fix tardío: `int80_dispatch_wrapper` y `user_task_trampoline` hardcodeaban exit 130 (SIGINT) en kill_pending; ahora leen `sig_pending` para usar `128+actual_sig`.
- ✓ Coreutils completos (~20 ELFs nuevos): env wc pwd uname basename dirname tail seq yes tee date printf grep sort uniq cut tr banner which clear tree
- ✓ README.md + CREATE_ELF.es.md + STATUS.md + ARCH.md actualizados

Lo que sigue (feature work, no bugs):

> **Nota de re-numbering**: las "FASE 11/12/13" del roadmap original
> abajo NO son las mismas FASE 11.0–11.4 / 12.0–12.1 / 13.0–13.1 que
> aparecen en la tabla cronológica de arriba. El roadmap original
> reservaba esos números para drivers/TUI/gráfico; en la práctica
> las fases 11.x del log se usaron para self-hosting (TCC/Lua/jq) +
> mouse, 12.x para Ox window system, 13.x para musl + busybox. Como
> resultado, partes del "FASE 13 — Gráfico" original ya están hechas
> en FASE 12.0/12.1 (window server, terminal en ventana, compositor),
> mientras que el "FASE 11 — Drivers a ring 3" original sigue
> íntegramente pendiente. Marcado a continuación.

### FASE 11 (roadmap original) — Drivers a ring 3 — PENDIENTE
Toda esta fase sigue 100% pendiente. PS/2 (keyboard + mouse),
framebuffer, ATA, RTL8139, PIT y serial siguen en ring 0 dentro de
`src/drivers/`. Lo único que pasó a ring 3 son los SERVIDORES de
políticas (consrv/kbdsrv/shellsrv/oxsrv), no los drivers de hardware.
- ☐ IRQ delegation por IPC desde kernel-side handlers
- ☐ MMIO mapping per-task con permisos especiales
- ☐ Port-IO delegation (syscall whitelist o IOPB en TSS)
- ☐ DMA bouncing via kernel-mediated buffer pool
- ☐ Portar PS/2, framebuffer, ATA, RTL8139, PIT a `elfs/osn-driver/`

### FASE 12 (roadmap original) — TUI potente — PARCIAL
- ☐ mini Norton Commander / mini-mc TUI (dos paneles texto). El
  file browser actual es **`/bin/oxfiles`** (FASE 12.1) pero corre
  como app GUI Ox, no como TUI texto-only sobre consrv.
- ✓ **file viewer paginado** — `/bin/less` con search `/pattern`
  highlight + `n`/`N` next/prev + pipe-mode (`cat foo | less`
  drena stdin + dup2 /dev/tty para keyboard). Ver tabla de fases
  cerradas arriba.
- ⚠ editor `ovi` con syntax highlighting — `/bin/ovi` existe (modal
  vim-style: hjkl, i/a/o, x/dd, :w/:q) PERO sin syntax highlighting.
- ⚠ copy/move/delete "visual" — están como comandos (`cp mv rm`) y
  como acciones de oxfiles via doble click; no hay un TUI commander
  con marca-mueve-pega texto-only.

### FASE 13 (roadmap original) — Gráfico — DONE (excepto Chip-8)
- ✓ **window_server** — `/bin/oxsrv` (FASE 12.0), ~700 LOC libc-linked,
  posee `/dev/fb0` via ioctls `FBIOGET_VSCREENINFO` + `FBIO_BLIT`,
  full compositor con wallpaper + z-order de 16 ventanas + cursor
  overlay + root menu Openbox-style + 5 apps cliente (oxfiles,
  oxnotepad, oxcalc, oxterm, oxsettings).
- ✓ **terminal en ventana** — `/bin/oxterm` (FASE 12.0/12.1) con PTY
  + child shell `/bin/uxsh` + parser ANSI completo (SGR truecolor,
  cursor positioning, erase display/line). 80×25 chars 8×16 font.
- ☐ Chip-8 emulator — pendiente, side quest.
- ✓ **mouse + compositor simple** — driver PS/2 mouse + `/dev/mouse0`
  (FASE 11.4) + compositor full-redraw on damage en oxsrv (FASE 12.0)
  + coalesce mouse MOVE eventos a 1/frame para no inundar IPC
  (FASE 12.1). Mouse acumula cursor 0..scr-1 a nivel oxsrv.

### Optimizaciones / futuro lejano — PENDIENTE
- ☐ SMP (multi-core)
- ☐ Copy-on-write para fork (hoy full page copy funciona pero usa más RAM)
- ☐ FASE 13.2 — Rebuild de BusyBox con `FEATURE_EDITING_SAVEHISTORY=y`
  para persistir `/home/.ash_history` entre reboots. Bloqueado por
  cross-compile issues (clang on macOS detecta `__APPLE__` → branch
  BSD de `include/platform.h` requiere `<machine/endian.h>`; musl
  no provee bswap/__NR_*module compatibles via headers default).
  Posible workaround: dropear el detect via `-U__APPLE__ -D__linux__`
  más wrapper que filtre `-xc -E` link-mode falses.
