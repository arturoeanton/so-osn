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
        # The upstream `-binary` branches ship pre-built BIOS / UEFI
        # blobs (limine-bios-cd.bin, BOOTX64.EFI, …) at the repo root;
        # `make` only builds the small host `limine` tool used for
        # `limine bios-install`. We never want the source/master branch
        # because it ships unbuilt sources.
        #
        # Branch naming is `v<MAJOR>.x-binary`. The major number tracks
        # the bootloader's protocol version. The kernel-deps/limine-protocol
        # header pinned by kernel-deps/get-deps must match the major;
        # if you bump one, bump the other.
        #
        # To avoid hardcoding a major that might not exist upstream, we
        # query the remote and pick the highest `v<N>.x-binary` branch.
        # Override with LIMINE_BRANCH=v12.x-binary (or similar) if the
        # discovered branch turns out to be too new for the pinned
        # protocol header.
        if [[ -n "${LIMINE_BRANCH:-}" ]]; then
            limine_branch="$LIMINE_BRANCH"
            echo "    using LIMINE_BRANCH=$limine_branch (override)"
        else
            echo "    discovering latest -binary branch on upstream..."
            limine_branch="$(git ls-remote --heads \
                https://github.com/limine-bootloader/limine.git 2>/dev/null \
                | awk '{print $2}' \
                | sed -n 's@^refs/heads/\(v[0-9]\{1,\}\.x-binary\)$@\1@p' \
                | sort -V -r \
                | head -1)"
            if [[ -z "$limine_branch" ]]; then
                echo "error: could not discover a v<N>.x-binary branch on limine upstream." >&2
                echo "       Set LIMINE_BRANCH=<branch> to override." >&2
                exit 1
            fi
            echo "    picked $limine_branch"
        fi
        if ! git clone --quiet --depth 1 --branch "$limine_branch" \
            https://github.com/limine-bootloader/limine.git "$LIMINE_VENDOR"; then
            echo "error: git clone of limine ($limine_branch) failed." >&2
            echo "       Try LIMINE_BRANCH=<other-branch> ./build_and_run.sh ..." >&2
            exit 1
        fi
        if ! make -C "$LIMINE_VENDOR"; then
            echo "error: building limine host tool failed." >&2
            echo "       Make sure gcc/cc is installed: sudo dnf install gcc" >&2
            exit 1
        fi
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
    maybe_need ar                 binutils
    maybe_need bison              bison
    maybe_need flex               flex
    maybe_need limine             limine
    maybe_need qemu-system-x86_64 qemu
    # Bootstraps below clone repos + run host make. On macOS brew always
    # has these; on Linux distros the minimal install might not.
    maybe_need git                git
    maybe_need make               make

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
  Fedora:   sudo dnf install clang lld xorriso mtools binutils qemu-system-x86 git make gcc
  Debian:   sudo apt install clang lld xorriso mtools binutils qemu-system-x86 git make gcc
  Arch:     sudo pacman -S clang lld xorriso mtools binutils qemu git make gcc

Notes:
  - limine is auto-vendored into osnos/vendor/limine on Linux when not
    packaged by the distro (Fedora as of 2026). No manual install needed.
  - On Linux gcc/cc is needed by limine's host Makefile + musl configure.
  - kernel-deps + tinycc are cloned automatically on first run (need git).
  - Optional: ImageMagick (\`convert\`) lets the build use the real JPG
    wallpapers from res/wallpapers/source/. Without it, every wallpaper
    falls back to a procedural samurai/girl placeholder.
      Fedora: sudo dnf install ImageMagick
      Debian: sudo apt install imagemagick
      Arch:   sudo pacman -S imagemagick
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
# osnos/kernel-deps/{cc-runtime,freestnd-c-hdrs,limine-protocol} are
# cloned by the helper script; on a fresh repo clone only the linker
# scripts + the helper itself are present in the working tree. The
# `.deps-obtained` marker is created at the end of get-deps.
if [[ "$ACTION" != "clean" ]] && [[ ! -f osnos/kernel-deps/.deps-obtained ]]; then
    echo "==> fetching kernel-deps (one-time bootstrap)"
    if ! (cd osnos/kernel-deps && ./get-deps); then
        echo "error: kernel-deps bootstrap failed (network / DNS?)." >&2
        echo "       Re-run after fixing connectivity; .deps-obtained" >&2
        echo "       was not written, so the next run will retry." >&2
        exit 1
    fi
fi

# --- bootstrap tinycc 0.9.27 source -----------------------------------------
# osnos/vendor/tinycc is recorded in git as an orphan gitlink (160000 mode
# pointing at upstream commit 1cd776bd..., but no .gitmodules entry), so a
# fresh `git clone` of the parent repo creates an empty directory there
# rather than populating any content. We detect that case (no `tcc.c` on
# disk) and shallow-clone the release_0_9_27 tag into it directly.
if [[ "$ACTION" != "clean" ]] && [[ ! -f osnos/vendor/tinycc/tcc.c ]]; then
    echo "==> fetching tinycc 0.9.27 (one-time)"
    tmpdir="$(mktemp -d)"
    if ! git clone --quiet --depth 1 -b release_0_9_27 \
            https://repo.or.cz/tinycc.git "$tmpdir/tinycc"; then
        echo "error: clone of tinycc from repo.or.cz failed." >&2
        echo "       Check network access to https://repo.or.cz/" >&2
        rm -rf "$tmpdir"
        exit 1
    fi
    rm -rf "$tmpdir/tinycc/.git"
    mkdir -p osnos/vendor/tinycc
    # /. copies contents (incl. dotfiles) into the (possibly empty) dest
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
# osnos only has a PS/2 AUX mouse driver (no USB, no virtio-input), so the
# usual QEMU `-device usb-tablet` absolute-pointer trick does nothing for
# us. PS/2 mouse is relative-deltas only, and the way QEMU translates host
# motion to those deltas depends on the display backend:
#   - cocoa (macOS): captures the cursor implicitly; motion feels smooth.
#   - gtk   (Linux): without grab-on-hover the host cursor drifts away
#     from the guest cursor on every move — feels broken. Forcing
#     grab-on-hover=on gives the same behaviour as cocoa.
# Override the backend with QEMU_DISPLAY=sdl (or whatever) if gtk still
# misbehaves on a particular host.
DISPLAY_FLAG=""
case "$(uname -s)" in
    Darwin) DISPLAY_FLAG="-display ${QEMU_DISPLAY:-cocoa,zoom-to-fit=on}" ;;
    Linux)  DISPLAY_FLAG="-display ${QEMU_DISPLAY:-gtk,grab-on-hover=on}" ;;
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
