/*
 * /bin/tcc — STUB. Real TinyCC port is not done yet.
 *
 * This binary exists so the path forward is concrete: when you
 * `make tcc-port` (TODO), the build will swap this stub for a
 * real TinyCC ELF bundled the same way as any other /bin entry.
 *
 * What's already in place for the eventual port (see STATUS.md):
 *   - User stack bumped to 64 KiB (TCC needs >4 KiB).
 *   - Hardware FPU enabled in ring 3 (math.h works).
 *   - sys_write respects f->offset (TCC writes ELFs in pieces).
 *   - proc_execve accepts arbitrary VFS paths (run the output).
 *   - libc headers: ctype, limits, float, math, signal.
 *
 * What still blocks a clean port (~1-2 days):
 *   - mmap (or build TCC with --no-mmap and use read+malloc).
 *   - A handful of Linux-only headers TCC source #includes.
 *   - Cross-compiling TCC against osnos libc + flags.
 *   - Per-task FXSAVE for safe FP under preemption.
 *
 * Smoke-test path once it's real:
 *   tcc -o /home/hi hello.c
 *   /home/hi
 *   # prints "hello, world"
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("osnos tcc (stub)\n");
    printf("  version: 0.0-pre, TinyCC not yet bundled\n");
    printf("  invoked with %d arg%s\n", argc, argc == 1 ? "" : "s");
    for (int i = 1; i < argc; i++) {
        printf("    argv[%d] = %s\n", i, argv[i]);
    }
    printf("\n");
    printf("Real port is tracked in STATUS.md. The osnos side now\n");
    printf("has user stack 64 KiB + FPU + write-offset + exec-from-\n");
    printf("VFS + libc headers, so a vanilla TinyCC source tree\n");
    printf("compiled against this libc should mostly work.\n");
    return 0;
}
