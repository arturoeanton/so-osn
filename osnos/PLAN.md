# PLAN.md — playbook para la próxima sesión (FASE 8 / FAT)

Pensado para abrir la próxima sesión y ejecutar sin re-pensar. Las
decisiones de diseño ya están tomadas; sólo hay que escribir el código.

---

## Estado al cierre de esta sesión

Cerradas:

- **FASE 1–7** boot, microkernel, VFS, syscalls, paging, ELF/ring3, libc
- **FASE 7.7** libc Tier 1 (FILE\*, qsort, setjmp, inet_pton, htons,
  arpa/inet.h, netinet/in.h, sys/socket.h con stubs ENOSYS)
- **FASE 3** introspección + `/bin/top` (con ANSI mínimo)
- **FASE 9** scheduler real: timer @ 100Hz, sleep real con suspend,
  preempción CPL=3, Ctrl+C live, /bin/kill, background jobs (`&`),
  `getpid`

Verificado vía `exec /bin/libctest`: 42/42 PASS.

Pendiente fuera de fases (no bloqueantes para FASE 8):

- `write` con offset real (hoy todo write es append)
- Reemplazar arrays estáticos en ramfs/ipc/fd con kmalloc dinámico
- `kheap` growth (hoy si te pasás de 64 KiB devuelve NULL)
- NX / PF_X enforcement (activar bit NX en EFER + PTE_NX)
- SMAP/SMEP/KPTI/COW/ASLR (ver `ROADMAP_APENDICE.md`)

---

# FASE 8 — Persistencia real (block driver + FAT)

## Decisiones tomadas (no re-discutir)

| Decisión              | Elegido            | Por qué                                  |
|-----------------------|--------------------|------------------------------------------|
| Block driver          | **ATA PIO**        | Simple, funciona con `-hda sd.img` plano |
|                       |                    | Sin config QEMU especial. VirtIO queda   |
|                       |                    | para FASE 8.5 si necesitamos perf.       |
| Filesystem            | **FAT16**          | ~400 LOC vs 600+ para FAT32. 2GB cap     |
|                       |                    | nos sobra (sd.img será 16MB).            |
| Read first, write 2nd | Sí                 | Aterriza la lectura, persistencia,       |
|                       |                    | mount visible. Write llega en 8.2.       |
| Mount point           | **/sd**            | Único disco. `/dev/sd0` cuando haya N.   |
| Imagen                | 16 MB, seed files  | `mkfs.fat -F 16 -C sd.img 16384` + copy  |
|                       |                    | hello.txt y README al mount loop.        |
| Write-back / cache    | Write-through      | Cada sector escrito hace PIO immediate.  |
|                       |                    | Sin buffer cache hasta 8.3.              |

## Sub-fases con deliverables

### 8.1 — ATA PIO block driver

**Archivos nuevos**:

- `src/drivers/block_ata.{c,h}`
  - `block_ata_init()`: detecta drive en Primary IDE (port 0x1F0/0x3F6)
    via IDENTIFY. Lee model/serial/sector count en variables internas.
  - `block_ata_read_sector(uint64_t lba, void *buf)`: PIO read 512 bytes
  - `block_ata_write_sector(uint64_t lba, const void *buf)`: PIO write
  - `block_ata_sector_count()`: total sectors descubiertos via IDENTIFY
- (Opcional 8.1b) `src/include/osnos_block.h`: contrato `block_ops_t`
  con read/write/count para el adapter VFS futuro. Si vas con un único
  driver podés saltarlo y exponer las funciones directo.

**Modificaciones**:

- `src/kernel/main.c` (kmain): `block_ata_init()` después de
  `lapic_init()`, antes de mounts.
- `src/fs/sysfs.c`: agregar `/sys/disks` con info (model, sectors).
  Útil para verificar que IDENTIFY funcionó.

**Test**:

```c
/* dentro de cmd_test: */
uint8_t buf[512];
ASSERT(block_ata_read_sector(0, buf) == 0);
ASSERT(buf[510] == 0x55 && buf[511] == 0xAA);  /* boot signature */
```

### 8.2 — FAT16 parser (read-only)

**Archivos nuevos**:

- `src/fs/fat.{c,h}` — el parser puro, sin VFS.
  - `fat_init(block_ops_t *block)`: lee sector 0, parsea BPB, valida
    FAT16, computa offsets (FAT start, root dir start, data start).
  - `fat_state_t`: bytes_per_sector, sectors_per_cluster, num_fats,
    fat_size_sectors, root_entries, root_dir_lba, data_lba.
  - `fat_lookup(const char *path) -> fat_dirent_t*`: walk path,
    devuelve raw dirent. Soporta short 8.3 names; LFN sin soportar
    en 8.2 (queda para 8.4).
  - `fat_read_file(dirent, off, buf, len)`: lee a través de FAT chain.
  - `fat_readdir(dirent, idx, out)`: iter sobre cluster chain de un
    dir. Devuelve 0 cuando ya no hay más.

**Layout FAT16** (referencia rápida, no buscar):

```
sector 0       : BPB / boot
sector 1..N    : FAT (entries de 16 bits)
sector N+1..   : root directory (root_entries * 32 bytes)
data area      : clusters indexados desde cluster 2
```

Cluster siguiente: `fat[cluster] != 0xFFFF` → there, sino end-of-chain.

**Test**: leer un README pre-creado, comparar bytes.

### 8.3 — VFS adapter

**Archivos nuevos**:

- `src/fs/fat_vfs.{c,h}` — adapter al estilo `ramfs_vfs.c`.
  - `extern const vfs_ops_t fat_vfs_ops;`
  - `fat_stat`, `fat_readdir`, `fat_read`, todos delegan a `fat.c`.
  - `fat_write/mkdir/rmdir/unlink/rename` → `OSNOS_EROFS` en 8.3.
  - Para 8.4 (write) reemplazás los stubs.

**Modificaciones**:

- `src/fs/bootstrap.c` (`bootstrap_fs`): tras montar `/`, llamar
  `fat_init(...)` y `vfs_mount("/sd", &fat_vfs_ops, &fat_state)`.
- `src/fs/sysfs.c`: `/sys/mounts` ya lista `/sd` automáticamente
  (longest-prefix dispatch).

**Test**:

```
osnos:/home> ls /sd
README.txt   hello.txt
osnos:/home> cat /sd/README.txt
hola desde el disco
osnos:/home>
```

### 8.4 — Write support (FAT update + FAT mirror)

Una vez que read funciona end-to-end:

- `fat_write_file(dirent, off, buf, len)`: marca cluster nuevo en FAT
  (free → cluster), update dirent size, escribe sectors.
- `fat_create(parent, name, size)`: nueva entry en dir, asigna cluster.
- `fat_unlink(dirent)`: marca FAT chain como FREE, set dirent[0] = 0xE5.
- `fat_vfs_ops` deja de devolver EROFS en mkdir/write/unlink.

**Test**: round-trip `echo hello > /sd/test` → reboot → `cat /sd/test`.

### 8.5 (opcional) — LFN support

Long File Names — entries especiales de tipo 0x0F antes de la 8.3 entry.
Si todos los nombres son 8.3 podés saltearlo.

## QEMU wiring

**Parent `GNUmakefile`** (`/home/osn/osnso/GNUmakefile`):

```make
# Crear sd.img con seed files
sd.img:
	dd if=/dev/zero of=$@ bs=1M count=16
	mkfs.fat -F 16 $@
	mkdir -p mnt
	sudo mount -o loop $@ mnt
	echo "hola desde el disco" | sudo tee mnt/README.TXT > /dev/null
	echo "fat works!" | sudo tee mnt/HELLO.TXT > /dev/null
	sudo umount mnt
	rmdir mnt

# Agregar -drive a la línea de QEMU
QEMUFLAGS += -drive file=sd.img,format=raw,if=ide
# (Si ya hay un -hda o similar, reemplazar por esto.)

run-bios: sd.img kernel
	qemu-system-x86_64 $(QEMUFLAGS) ...
```

**Alternativa sin sudo** (mtools, si está instalado):

```sh
mformat -i sd.img -F
mcopy -i sd.img README.TXT ::
mcopy -i sd.img HELLO.TXT ::
```

## Estimación

| Sub-fase | LOC kernel | LOC test | Esfuerzo  |
|----------|-----------:|---------:|-----------|
| 8.1 ATA  | ~150       | ~30      | ~1 sesión |
| 8.2 FAT  | ~400       | ~50      | ~1-2 sesiones |
| 8.3 VFS  | ~100       | ~20      | ~30 min   |
| 8.4 Write| ~250       | ~40      | ~1 sesión |

Total mínimo viable (8.1+8.2+8.3, read-only): ~1.5-2 sesiones.

## Punto de partida concreto al abrir la próxima sesión

```
1. Leer src/drivers/lapic.c como template de driver C (registros,
   inb/outb, init pattern).
2. Leer src/fs/ramfs_vfs.c como template de adapter VFS.
3. Crear src/drivers/block_ata.c con block_ata_init().
4. Verificar: agregar línea de debug en kmain que imprima sector
   count de IDENTIFY. Si imprime 32768 (16MB / 512B), driver OK.
5. Crear src/fs/fat.c con fat_init() leyendo sector 0 + BPB parse.
6. Verificar: imprimir bytes_per_sector, num_fats. Debe dar 512 y 2.
7. Implementar fat_readdir sobre root dir.
8. Agregar fat_vfs_ops + mount en bootstrap_fs.
9. ls /sd should work.
```

---

# Después de FASE 8

## FASE 8.5 — Networking (post-FAT)

La libc ya tiene el surface lista (`arpa/inet.h`, `netinet/in.h`,
`sys/socket.h` con stubs ENOSYS). Cuando lleguemos:

1. Driver de red (e1000 o RTL8139 en QEMU; RTL8139 es ~200 LOC).
2. ARP / IPv4 / ICMP echo minimal.
3. UDP socket.
4. TCP socket (server + client minimal).
5. Reemplazar stubs en `lib/libc/inet.c` con `osnos_syscall*` calls.
6. Demo: `/bin/httpd` minimal sirviendo `/sd/index.html`.

Esperable LOC: ~1500-2000 (incluye stack TCP minimal).

## FASE 10 — Servers a userspace

Mover console, keyboard, shell, fs server de ring 0 a ELFs ring 3.
Necesita:

- IPC kernel-mediated entre user tasks (hoy es kernel global queue).
- Permission model para servicio registration.
- Stable ABI documentada para que cualquier ELF pueda implementar
  un servicio.

## FASE 11 — TUI potente

Mini Norton Commander, viewer, editor. Con FAT funcionando se vuelve
útil (poder editar archivos persistentes).

## FASE 12 — Gráfico

Window server, terminal en ventana, mouse, compositor simple.

---

## Checklist al abrir la próxima sesión

```
[ ] git status — confirmar que estamos en branch trunk
[ ] make 2>&1 | tail -5 — confirmar que la build sigue limpia
[ ] exec /bin/libctest — confirmar 42 PASS (smoke test del estado)
[ ] Abrir este PLAN.md en split + src/drivers/lapic.c como template
[ ] Empezar por block_ata.c — IDENTIFY primero, read después
```
claude --resume 03fa4eb3-b7ff-42c3-aa5b-e8756c186085
