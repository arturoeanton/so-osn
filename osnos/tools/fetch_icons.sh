#!/usr/bin/env bash
# fetch_icons.sh — populate res/icons/ from the Yaru icon theme.
#
# Downloads a curated subset of Yaru PNGs (Ubuntu's GPL-3 theme, has
# a freedesktop-style icon set with good color flat designs that
# match the BeOS R5 / Haiku aesthetic at small sizes). Each PNG is
# resized to 24x24 (if not already) and emitted as a raw RGBA byte
# stream at res/icons/<key>.rgba.
#
# The osnos build copies res/icons/*.rgba to /home/.icons/ on the
# FAT16 image; oxsrv loads them at startup via ox_icon_get().
#
# Re-run this script to refresh the cache; it skips files already
# present unless -f / --force is given.
set -euo pipefail

BASE="https://raw.githubusercontent.com/ubuntu/yaru/master/icons/Yaru/24x24/apps"
OUT_DIR="res/icons"
FORCE=0
[[ "${1:-}" == "-f" || "${1:-}" == "--force" ]] && FORCE=1

# Mapping: <local-key> <yaru-png-name>
MAPPING=(
    "notepad   accessories-text-editor.png"
    "browser   internet-web-browser.png"
    "netsurf   org.gnome.Epiphany.png"
    "sqlite    applications-office.png"
    "hex       accessories-character-map.png"
    "files     system-file-manager.png"
    "calc      accessories-calculator.png"
    "term      utilities-terminal.png"
    "settings  preferences-system.png"
    "top       utilities-system-monitor.png"
    "clock     gnome-clocks.png"
    "paint     libreoffice-draw.png"
    "info      help-browser.png"
    "log       gnome-logs.png"
    "app       applications-system.png"
)

mkdir -p "$OUT_DIR"
command -v curl  >/dev/null || { echo "error: curl required"; exit 1; }
command -v magick >/dev/null || command -v convert >/dev/null || {
    echo "error: ImageMagick required (brew install imagemagick)"; exit 1; }

IM=$(command -v magick || command -v convert)

ok=0; skipped=0; failed=0
for entry in "${MAPPING[@]}"; do
    set -- $entry
    key="$1"; remote="$2"
    out="$OUT_DIR/$key.rgba"
    if [[ -f "$out" && $FORCE -eq 0 ]]; then
        echo "  SKIP    $key (exists, use -f to refresh)"
        skipped=$((skipped+1))
        continue
    fi
    tmp_png=$(mktemp -t osnos-icon.XXXXXX).png
    current="$remote"
    fetched=0
    # Yaru stores many icons as git-level symlinks. raw.githubusercontent
    # serves the symlink target string instead of the linked file, so a
    # "PNG" download may come back as a 20-byte ASCII filename. Follow
    # up to 5 levels of indirection.
    for hop in 1 2 3 4 5; do
        if ! curl -fsSL -o "$tmp_png" "$BASE/$current"; then
            break
        fi
        sz=$(wc -c < "$tmp_png")
        if [[ "$sz" -lt 256 ]] && grep -qE '^[a-zA-Z0-9_.-]+\.png[[:space:]]*$' "$tmp_png"; then
            current=$(tr -d '[:space:]' < "$tmp_png")
            continue
        fi
        fetched=1
        break
    done
    if [[ $fetched -eq 0 ]]; then
        echo "  FAIL    $key   (could not resolve $remote)"
        failed=$((failed+1))
        rm -f "$tmp_png"
        continue
    fi
    "$IM" "$tmp_png" -background none -resize 24x24 -depth 8 "RGBA:$out"
    echo "  GOT     $key   ($(wc -c < "$out") bytes from $current)"
    rm -f "$tmp_png"
    ok=$((ok+1))
done

echo ""
echo "icons: $ok fetched, $skipped skipped, $failed failed (in $OUT_DIR/)"
echo "run 'make' to refresh sd.img with the new icons"
