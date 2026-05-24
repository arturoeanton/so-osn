#!/bin/sh
# Wrapper alrededor de clang para BusyBox builds targeting osnos.
# - compile: clang con flags freestanding + musl headers
# - link: ld.lld -m elf_x86_64 directo (no via clang en macOS — usaría ld64)
MUSL_LIB=/Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/build-osnos/lib

has_minus_c=0
for a in "$@"; do
    case "$a" in
        -c) has_minus_c=1 ;;
    esac
done

if [ "$has_minus_c" -eq 1 ]; then
    # Compile mode — clang normal.
    exec clang "$@"
fi

# Link mode. BusyBox pasa: clang ... -o output OBJ1 OBJ2 ... -lm
# Filtramos -lm, sacamos flags clang-only (-static, -no-pie), agregamos
# crt + libc explícitos.
args=""
output=""
out_next=0
for a in "$@"; do
    if [ "$out_next" -eq 1 ]; then output="$a"; out_next=0; continue; fi
    case "$a" in
        -lm|-lpthread|-ldl|-lrt) ;;             # musl libc has these
        -static|-no-pie|-nostdlib|-fno-PIC|-fno-pie) ;;  # ld.lld handles
        -fno-stack-protector|-target|-mno-red-zone) ;;
        x86_64-unknown-none-elf) ;;             # value of -target above
        -fuse-ld=lld) ;;                         # we ARE lld
        -Wl,--start-group|-Wl,--end-group) ;;   # we add our own
        -Wall|-Wshadow|-Wwrite-strings|-Wundef|-Wstrict-prototypes) ;;
        -Wunused*|-Wmissing*|-Wno-*|-Wdeclaration-after-statement) ;;
        -Wold-style-definition|-fno-builtin-*|-fomit-frame-pointer) ;;
        -ffunction-sections|-fdata-sections|-funsigned-char) ;;
        -static-libgcc|-falign-functions=1|-fno-unwind-tables) ;;
        -fno-asynchronous-unwind-tables|-Oz|-O*|-g*) ;;
        -isystem|-nostdinc|-MMD|-MP) ;;
        /*musl/include|/*musl/arch/*|/*musl/build-osnos/*) ;;
        -Wl,*) args="$args ${a#-Wl,}" ;;          # strip -Wl, prefix
        -o)  out_next=1 ;;
        *)   args="$args $a" ;;
    esac
done
[ -z "$output" ] && output="a.out"
exec ld.lld -m elf_x86_64 -static -no-pie -z noexecstack --gc-sections \
    -o "$output" \
    $MUSL_LIB/crt1.o \
    --start-group $args $MUSL_LIB/libc.a --end-group
