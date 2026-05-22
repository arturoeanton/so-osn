#!/usr/bin/env bash
# Build OSnOS and boot it in QEMU.
# Works on macOS (Homebrew) and Linux (Fedora/Debian/Arch). Uses
# whichever Limine is installed on the host — nothing is bundled.
#
#   ./build_and_run.sh            # clean + build + run-bios
#   ./build_and_run.sh build      # just build the ISO
#   ./build_and_run.sh run        # boot the existing ISO
#   ./build_and_run.sh clean      # wipe build artifacts
set -euo pipefail

cd "$(dirname "$0")"

ACTION="${1:-all}"

# --- locate Limine -----------------------------------------------------------
if [[ -z "${LIMINE_DIR:-}" ]]; then
    for candidate in \
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

Install Limine first:
  macOS:    brew install limine
  Fedora:   sudo dnf install limine
  Debian:   sudo apt install limine
  Arch:     sudo pacman -S limine

Or set LIMINE_DIR=/path/to/limine/share manually.
EOF
    exit 1
fi

# --- check the rest of the toolchain ----------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' not in PATH" >&2; exit 1; }; }
if [[ "$ACTION" != "clean" ]]; then
    need clang
    need ld.lld
    need xorriso
    need limine
    need qemu-system-x86_64
fi

# --- pick a QEMU display backend that exists on this host -------------------
DISPLAY_FLAG=""
case "$(uname -s)" in
    Darwin) DISPLAY_FLAG="-display cocoa,zoom-to-fit=on" ;;
    Linux)  DISPLAY_FLAG="-display gtk"   ;;
esac

QEMUFLAGS="${QEMUFLAGS:--m 2G $DISPLAY_FLAG}"

export LIMINE_DIR
export QEMUFLAGS

case "$ACTION" in
    clean)
        make -C osnos clean
        ;;
    build|iso)
        make -C osnos iso
        ;;
    run)
        make -C osnos run-bios
        ;;
    all)
        make -C osnos clean
        make -C osnos run-bios
        ;;
    *)
        echo "usage: $0 [all|build|run|clean]" >&2
        exit 2
        ;;
esac
