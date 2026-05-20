# ROADMAP — Apéndice

Cosas que **no vamos a hacer hasta que el resto del roadmap esté terminado**.

Conceptualmente son mejoras de hardening, performance y producción. Hoy
osnos es un kernel didáctico — estos items aplican cuando el sistema
tenga base estable (todas las fases 1-12 del ROADMAP principal cerradas).

## Hardening de memoria

- **SMAP / SMEP** — bits del CR4 que bloquean ejecución de páginas user
  desde ring 0 (SMEP) y accesos a memoria user sin habilitarlos
  explícitamente con `stac/clac` (SMAP). Requieren coordinación con
  copy_from/to_user.

- **KPTI / Meltdown mitigation** — separar el PML4 user y kernel para
  que páginas kernel no aparezcan en TLB user. Costo significativo en
  performance.

- **NX everywhere** — habilitar EFER.NXE, marcar páginas de datos como
  NX (no-execute) consistentemente. Ya tenemos el bit `PTE_NX` definido
  en `vmm.h` pero no lo usamos en todas las páginas.

- **W^X** — invariante "ninguna página simultáneamente writable +
  executable". Requiere división estricta de regiones .text vs .data.

- **ASLR (kernel y user)** — randomizar bases de regiones (kernel,
  heap, stack, libs). Requiere RNG kernel + cooperación del ELF loader.

- **Stack canaries** — `-fstack-protector` con un canary aleatorio.
  Hoy compilamos con `-fno-stack-protector`.

## Memoria avanzada

- **Copy-on-write fork** — duplicar PML4 marcando páginas como RO
  shared; copiar en el primer write fault.

- **mmap real** — anonymous + file-backed, con flags MAP_SHARED /
  MAP_PRIVATE, MAP_FIXED, etc.

- **Demand paging / lazy alloc** — alocar páginas físicas solo cuando
  se accede.

- **Swap a disco** — paginar a un swap area cuando hay presión.

- **Huge pages (2 MiB / 1 GiB)** — `PS=1` en PD/PDPT.

- **NUMA awareness** — irrelevante hasta que haya SMP.

## Concurrencia

- **SMP (multi-core)** — APIC, IPIs, per-CPU storage, spinlocks /
  ticket locks.

- **RCU** — read-copy-update para estructuras kernel.

- **lock-free / wait-free data structures**.

## Otros sistemas

- **Audit subsystem** — log de syscalls / accesos.
- **eBPF-like** — VM verificable para hooks user-defined.
- **cgroups / namespaces** — containers.
- **Capabilities / LSM (SELinux/AppArmor-like)** — fine-grained
  permission model más allá de Unix uid/gid.
- **Profiling kernel** (kperf / kpatch) — instrumentación dinámica.

## Performance

- **vDSO** — funciones rápidas como `gettimeofday` que no requieren
  syscall.

- **Vectorization** — habilitar SSE/AVX para userland (FPU state
  per-task, lazy FPU switching).

- **DMA** — drivers usan DMA engines en vez de PIO.

- **Zero-copy I/O** — splice, sendfile, sin copias intermedias.

---

**Cuándo abrir este apéndice**: cuando hayamos terminado las FASES
1-12 del ROADMAP principal y queramos pulir el kernel para
producción. Hoy estamos en FASE 5, así que esto está muy lejos.
