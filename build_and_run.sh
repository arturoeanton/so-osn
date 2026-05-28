#!/usr/bin/env bash
# Build OSnOS and boot it in QEMU.
# Works on macOS (Homebrew) and Linux (Fedora/Debian/Arch). Uses
# whichever Limine is installed on the host — nothing is bundled.
#
#   ./build_and_run.sh            # clean + build + run-bios (graphical)
#   ./build_and_run.sh build      # just build the ISO
#   ./build_and_run.sh run        # boot the existing ISO
#   ./build_and_run.sh headless   # boot WITHOUT a window — host stdio
#                                  # becomes the osnos serial console.
#                                  # Useful for CI / `tee log.txt`.
#   ./build_and_run.sh clean      # wipe build artifacts
#
# In graphical mode the serial console is teed to ./serial.log so you
# can `tail -f serial.log` from another terminal without interrupting
# the QEMU window.
set -euo pipefail

cd "$(dirname "$0")"

ACTION="${1:-all}"

# --- toolchain check (with brew bootstrap on macOS) -------------------------
# Each required binary maps to a Homebrew formula. Limine is in here too
# because brew installs both the `limine` CLI and the share/ boot files.
declare -a MISSING_BINS=()
declare -a MISSING_BREW=()
maybe_need() {
    local bin="$1" brew_pkg="$2"
    if ! command -v "$bin" >/dev/null 2>&1; then
        MISSING_BINS+=("$bin")
        MISSING_BREW+=("$brew_pkg")
    fi
}

if [[ "$ACTION" != "clean" ]]; then
    # On macOS, several brew formulae are keg-only (their bin dirs aren't
    # on PATH by default). Pre-pend each so previously-installed ones are
    # picked up here without forcing the user to `brew link --force`.
    # binutils → objcopy/ar/ranlib (build needs GNU, not Apple's BSD)
    # bison    → yacc (Apple's /usr/bin/yacc needs full Xcode)
    # flex     → lex (parser generation for jq)
    if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
        for pkg in binutils bison flex; do
            kegdir="$(brew --prefix "$pkg" 2>/dev/null)/bin"
            [[ -d "$kegdir" ]] && export PATH="$kegdir:$PATH"
        done
    fi

    # On Linux distros that don't package limine (e.g. Fedora as of
    # 2026), vendor it from the upstream binary branch BEFORE the
    # toolchain check so `maybe_need limine` below finds it. macOS goes
    # the brew route — its `limine` formula stays in place.
    LIMINE_VENDOR="$(pwd)/osnos/vendor/limine"
    if [[ "$(uname -s)" == "Linux" ]] && \
       ! command -v limine >/dev/null 2>&1 && \
       [[ ! -x "$LIMINE_VENDOR/limine" ]]; then
        if ! command -v git >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
            echo "error: limine bootstrap needs git + make on the host." >&2
            echo "       sudo dnf install git make gcc" >&2
            exit 1
        fi
        echo "==> fetching limine (one-time bootstrap — distro lacks it)"
        rm -rf "$LIMINE_VENDOR"
        # v9.x-binary is the upstream binary branch — ships the
        # pre-built BIOS / UEFI blobs (limine-bios-cd.bin, BOOTX64.EFI,
        # …) at the repo root; `make` only builds the small host `limine`
        # binary used for `limine bios-install`. Pinned to the binary
        # branch (not master) so we don't accidentally pick up the
        # unbuilt source tree.
        git clone --quiet --depth 1 --branch v9.x-binary \
            https://github.com/limine-bootloader/limine.git "$LIMINE_VENDOR"
        make -C "$LIMINE_VENDOR"
    fi
    # Prepend the vendored bin so the rest of the script + the
    # LIMINE_DIR auto-detection below pick it up automatically.
    if [[ -x "$LIMINE_VENDOR/limine" ]]; then
        export PATH="$LIMINE_VENDOR:$PATH"
    fi

    maybe_need clang              llvm
    maybe_need ld.lld             lld
    maybe_need xorriso            xorriso
    maybe_need mformat            mtools
    maybe_need mcopy              mtools
    maybe_need objcopy            binutils
    maybe_need bison              bison
    maybe_need flex               flex
    maybe_need limine             limine
    maybe_need qemu-system-x86_64 qemu

    # macOS ships GNU Make 3.81 (from 2006). It has a bug where
    # pattern rules with multi-component stems don't win over more
    # generic rules — which makes lib/libc/*.c get compiled with
    # kernel CFLAGS instead of USER_CFLAGS. `brew install make`
    # installs GNU Make 4.x as `gmake`. On Linux the bundled `make`
    # is already 4.x, so this only kicks in on Darwin.
    if [[ "$(uname -s)" == "Darwin" ]]; then
        if ! command -v gmake >/dev/null 2>&1; then
            MISSING_BINS+=("gmake")
            MISSING_BREW+=("make")
        fi
    fi

    if (( ${#MISSING_BINS[@]} > 0 )); then
        echo "error: missing host tools: ${MISSING_BINS[*]}" >&2
        # de-dup the brew package list while preserving order
        declare -a UNIQ_BREW=()
        for p in "${MISSING_BREW[@]}"; do
            # ${UNIQ_BREW[*]:-} guards the first iteration under `set -u`,
            # where the array is still empty.
            [[ " ${UNIQ_BREW[*]:-} " == *" $p "* ]] || UNIQ_BREW+=("$p")
        done

        if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
            # On macOS Apple clang ships with /usr/bin/clang; only suggest
            # `brew install llvm` if clang is genuinely absent.
            echo
            echo "Detected Homebrew. To install everything missing, run:"
            echo "  brew install ${UNIQ_BREW[*]}"
            read -r -p "Install now? [y/N] " reply
            if [[ "$reply" =~ ^[Yy]$ ]]; then
                brew install "${UNIQ_BREW[@]}"
                # Re-prepend keg-only formula bins after install so the
                # build sees them without `brew link --force`.
                for pkg in binutils bison flex; do
                    kegdir="$(brew --prefix "$pkg" 2>/dev/null)/bin"
                    [[ -d "$kegdir" ]] && export PATH="$kegdir:$PATH"
                done
            else
                exit 1
            fi
        else
            cat >&2 <<EOF

Install the missing tools:
  macOS:    brew install ${UNIQ_BREW[*]}
  Fedora:   sudo dnf install clang lld xorriso mtools qemu-system-x86 git make
  Debian:   sudo apt install clang lld xorriso mtools qemu-system-x86 git make
  Arch:     sudo pacman -S clang lld xorriso mtools qemu git make

Note: limine is auto-vendored into osnos/vendor/limine on Linux when not
packaged by the distro (Fedora as of 2026). No manual install needed.
EOF
            exit 1
        fi
    fi
fi

# --- locate Limine boot files (installed by `brew install limine`, the
# Linux distro package, or vendored at osnos/vendor/limine) ------------------
if [[ -z "${LIMINE_DIR:-}" ]]; then
    for candidate in \
        "$(pwd)/osnos/vendor/limine" \
        /opt/homebrew/share/limine \
        /usr/local/share/limine \
        /usr/share/limine
    do
        if [[ -f "$candidate/limine-bios-cd.bin" ]]; then
            LIMINE_DIR="$candidate"
            break
        fi
    done
fi

if [[ -z "${LIMINE_DIR:-}" ]] && [[ "$ACTION" != "clean" ]]; then
    cat >&2 <<EOF
error: Limine boot files not found.

Looked at osnos/vendor/limine (auto-bootstrap), /opt/homebrew/share/limine,
/usr/local/share/limine, /usr/share/limine. Set LIMINE_DIR=/path/to/limine
manually if the binaries live elsewhere.
EOF
    exit 1
fi

# --- pick GNU Make 4.x (gmake on macOS, make on Linux) ----------------------
if command -v gmake >/dev/null 2>&1; then
    MAKE_BIN=gmake
else
    MAKE_BIN=make
fi

# --- bootstrap kernel-deps (cloned, not vendored) ---------------------------
if [[ "$ACTION" != "clean" ]] && [[ ! -f osnos/kernel-deps/.deps-obtained ]]; then
    echo "==> fetching kernel-deps (one-time bootstrap)"
    (cd osnos/kernel-deps && ./get-deps)
fi

# --- bootstrap tinycc 0.9.27 source (vendor/tinycc/ is empty in fresh clones)
if [[ "$ACTION" != "clean" ]] && [[ -z "$(ls -A osnos/vendor/tinycc 2>/dev/null)" ]]; then
    echo "==> fetching tinycc 0.9.27 (one-time)"
    tmpdir="$(mktemp -d)"
    git clone --quiet --depth 1 -b release_0_9_27 \
        https://repo.or.cz/tinycc.git "$tmpdir/tinycc"
    rm -rf "$tmpdir/tinycc/.git"
    # /. copies contents (incl. dotfiles) into the existing empty dir
    cp -R "$tmpdir/tinycc/." osnos/vendor/tinycc/
    rm -rf "$tmpdir"
fi

# tinycc's tcc.h includes "config.h" which is generated by its own configure.
# Generate a host-default config.h once; the osnos build overrides what it
# needs with -D flags at compile time.
if [[ "$ACTION" != "clean" ]] && [[ ! -f osnos/vendor/tinycc/config.h ]]; then
    echo "==> generating tinycc config.h (one-time)"
    (cd osnos/vendor/tinycc && ./configure >/dev/null)
fi

# --- bootstrap vendored musl 1.2.5 (one-time configure + build) -------------
# The kernel build links a handful of ELFs (hello_musl, ls_musl, sqlite3,
# hello_dyn) against this libc. The osnos GNUmakefile expects libc.a +
# libc.so + crt{1,i,n}.o under vendor/musl/build-osnos/lib/ but has no
# rule to build them. macOS-specific patches we apply post-configure:
#   - target=x86_64-linux-gnu so clang's link driver picks ELF/lld
#     defaults instead of Mach-O (otherwise libc.so link fails)
#   - --ld-path to the explicit ld.lld so clang doesn't pick ld64.lld
#   - AR/RANLIB to plain ar/ranlib (no x86_64- prefix exists on host)
#   - drop -Wa,--noexecstack: not understood by clang's link step
if [[ "$ACTION" != "clean" ]] && [[ ! -f osnos/vendor/musl/build-osnos/lib/libc.so ]]; then
    echo "==> building vendored musl 1.2.5 (one-time, ~2-3 min)"
    (
        cd osnos/vendor/musl/build-osnos
        CC="clang -target x86_64-linux-gnu -fno-stack-protector -fno-stack-check -ffreestanding -nostdinc -mno-red-zone" \
            ../configure --target=x86_64 --syslibdir=/lib --prefix=/usr
        # Portable in-place: BSD sed wants `-i ''` for no-backup; GNU sed
        # wants just `-i`. Pipe through a tmp file to avoid the dialect
        # split AND to avoid clobbering the upstream config.mak.bak.
        sed \
            -e 's/-Wa,--noexecstack//g' \
            -e 's|^LDFLAGS_AUTO = $|LDFLAGS_AUTO = --ld-path='"$(command -v ld.lld)"'|' \
            -e 's/^AR = .*/AR = ar/' \
            -e 's/^RANLIB = .*/RANLIB = ranlib/' \
            config.mak > config.mak.new && mv config.mak.new config.mak
        "$MAKE_BIN" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    )
fi

# --- pick a QEMU display backend that exists on this host -------------------
DISPLAY_FLAG=""
case "$(uname -s)" in
    Darwin) DISPLAY_FLAG="-display cocoa,zoom-to-fit=on" ;;
    Linux)  DISPLAY_FLAG="-display gtk"   ;;
esac

# Serial routing:
#   graphical mode → tee to ./serial.log (file sink, non-interactive)
#   headless mode  → -nographic with stdio = serial (interactive console)
# The host's `chardev:file` sink is infinite so writes never block.
case "$ACTION" in
    headless)
        QEMUFLAGS="${QEMUFLAGS:--m 2G -nographic -serial mon:stdio}"
        ;;
    *)
        QEMUFLAGS="${QEMUFLAGS:--m 2G $DISPLAY_FLAG -serial file:serial.log}"
        ;;
esac

export LIMINE_DIR
export QEMUFLAGS

case "$ACTION" in
    clean)
        "$MAKE_BIN" -C osnos clean
        ;;
    build|iso)
        "$MAKE_BIN" -C osnos iso
        ;;
    run)
        "$MAKE_BIN" -C osnos run-bios
        ;;
    headless)
        "$MAKE_BIN" -C osnos run-bios
        ;;
    all)
        "$MAKE_BIN" -C osnos clean
        "$MAKE_BIN" -C osnos run-bios
        ;;
    *)
        echo "usage: $0 [all|build|run|headless|clean]" >&2
        exit 2
        ;;
esac
