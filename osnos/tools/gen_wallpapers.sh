#!/bin/sh
# tools/gen_wallpapers.sh — produces samurai.ppm and girl.ppm in the
# build directory passed as $1. For each theme:
#   if res/wallpapers/source/<theme>.png exists AND `convert` (ImageMagick)
#     is installed → convert PNG → PPM (1280x800, cover).
#   else → compile + run tools/gen_placeholder.c to emit a procedural PPM.
#
# Fully unattended: never prompts, always produces both files.

set -eu

out=$1
mkdir -p "$out"

bin="$out/genplc"
if [ ! -x "$bin" ]; then
    cc -O2 tools/gen_placeholder.c -o "$bin"
fi

for name in samurai girl; do
    src="res/wallpapers/source/$name.png"
    dst="$out/$name.ppm"
    if [ -f "$src" ] && command -v convert >/dev/null 2>&1; then
        echo "  CONV    $name.png -> $name.ppm" >&2
        convert "$src" \
            -resize 1280x800^ \
            -gravity center \
            -extent 1280x800 \
            -depth 8 \
            -compress none \
            "ppm:$dst"
    else
        echo "  GEN     $name.ppm (placeholder)" >&2
        "$bin" "$name" "$dst"
    fi
done
