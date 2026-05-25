#!/bin/sh
# Wrapper alrededor de clang para BusyBox cross-compile a osnos
# desde macOS. Tres modos:
#   1. Preproc mode (-E / -xc / -MM / -dM): plain clang con musl headers.
#   2. Compile mode (-c): clang con -U__APPLE__ -D__linux__ + musl headers.
#   3. Link mode: ld.lld directo (no via clang en macOS — usaría ld64).
# El truco crítico: -U__APPLE__ -D__linux__ hace que include/platform.h
# tome la rama Linux y use <byteswap.h>/<endian.h>/<sys/syscall.h> de
# musl en vez de <machine/endian.h> que es macOS-specific.

MUSL_DIR=/Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl
MUSL_LIB=$MUSL_DIR/build-osnos/lib
MUSL_INCS="-isystem $MUSL_DIR/build-osnos/obj/include \
           -isystem $MUSL_DIR/arch/x86_64 \
           -isystem $MUSL_DIR/arch/generic \
           -isystem $MUSL_DIR/include \
           -isystem /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/kernel-deps/freestnd-c-hdrs/include"

OSNOS_DEFS="-U__APPLE__ -D__linux__ -D__linux"

# Classify args
has_minus_c=0
is_preproc=0
for a in "$@"; do
    case "$a" in
        -c)             has_minus_c=1 ;;
        -E|-xc|-MM|-dM) is_preproc=1 ;;
    esac
done

# Preprocess-only invocation (busybox feature probes like `cc -E -dM -`)
if [ "$is_preproc" -eq 1 ]; then
    exec clang -target x86_64-unknown-none-elf $OSNOS_DEFS -nostdinc $MUSL_INCS "$@"
fi

# Compile-only invocation (-c)
if [ "$has_minus_c" -eq 1 ]; then
    exec clang -target x86_64-unknown-none-elf $OSNOS_DEFS -nostdinc $MUSL_INCS "$@"
fi

# Link mode. Filtramos flags clang-only y agregamos crt + libc explícitos.
args=""
output=""
out_next=0
for a in "$@"; do
    if [ "$out_next" -eq 1 ]; then output="$a"; out_next=0; continue; fi
    case "$a" in
        -lm|-lpthread|-ldl|-lrt|-lc) ;;          # musl libc has these
        -static|-no-pie|-nostdlib|-fno-PIC|-fno-pie) ;;
        -fno-stack-protector|-target|-mno-red-zone) ;;
        x86_64-unknown-none-elf) ;;
        -fuse-ld=lld) ;;
        -Wl,--start-group|-Wl,--end-group) ;;
        -Wall|-Wshadow|-Wwrite-strings|-Wundef|-Wstrict-prototypes) ;;
        -Wunused*|-Wmissing*|-Wno-*|-Wdeclaration-after-statement) ;;
        -Wold-style-definition|-fno-builtin-*|-fomit-frame-pointer) ;;
        -ffunction-sections|-fdata-sections|-funsigned-char) ;;
        -static-libgcc|-falign-functions=1|-fno-unwind-tables) ;;
        -falign-jumps=*|-falign-labels=*|-falign-loops=*) ;;
        -finline-limit=*|-fno-asynchronous-unwind-tables) ;;
        -std=*|-include) ;;
        -Iinclude|-Ilibbb|-I*) ;;
        -D_GNU_SOURCE|-DNDEBUG|-DBB_VER=*|-DKBUILD_*) ;;
        -Wp,*) ;;
        -Oz|-O*|-g*) ;;
        -isystem|-nostdinc|-MMD|-MP) ;;
        -U__APPLE__|-D__linux__|-D__linux) ;;
        /*musl/include|/*musl/arch/*|/*musl/build-osnos/*|/*freestnd-c-hdrs/*) ;;
        -Wl,-Map,*|-Wl,--warn-common|-Wl,--verbose|-Wl,--sort-common|-Wl,--sort-section=*|-Wl,--gc-sections|-Wl,-z,*) ;;
        -Wl,*) args="$args ${a#-Wl,}" ;;
        -o)  out_next=1 ;;
        *)   args="$args $a" ;;
    esac
done
[ -z "$output" ] && output="a.out"
exec ld.lld -m elf_x86_64 -static -no-pie -z noexecstack --gc-sections \
    -o "$output" \
    $MUSL_LIB/crt1.o \
    --start-group $args $MUSL_LIB/libc.a --end-group
