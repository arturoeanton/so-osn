#!/bin/sh
# tools/gen_wallpapers.sh — produces full-size + thumbnail PPMs for every
# wallpaper in res/wallpapers/source/. For each <name>.{png,jpg,jpeg}:
#   - <out>/<name>.ppm        — 1280x800 full screen (P6 binary)
#   - <out>/thumbs/<name>.ppm — 200x120  preview for oxsettings (P6 binary)
# Always produces a `samurai.ppm` procedural placeholder so the legacy
# default still works if everything else is removed.
#
# Fully unattended: never prompts, always emits whatever it can.

set -eu

out=$1
mkdir -p "$out" "$out/thumbs"

bin="$out/genplc"
if [ ! -x "$bin" ]; then
    cc -O2 tools/gen_placeholder.c -o "$bin"
fi

src_dir="res/wallpapers/source"

# Discover all wallpapers from source dir. Names are derived from
# basename minus extension. If the source dir is empty we fall back to
# the legacy two procedural placeholders.
names=
if [ -d "$src_dir" ]; then
    for f in "$src_dir"/*.png "$src_dir"/*.jpg "$src_dir"/*.jpeg; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        n=${b%.*}
        # de-dup
        case " $names " in
            *" $n "*) ;;
            *) names="$names $n" ;;
        esac
    done
fi
names=$(echo "$names" | tr ' ' '\n' | grep -v '^$' || true)

# Always include `samurai` placeholder so the boot fallback is never
# missing. (Procedural-only — we keep it for old .oxrc compatibility.)
if ! echo "$names" | grep -q '^samurai$'; then
    "$bin" "samurai" "$out/samurai.ppm"
    "$bin" "samurai" "$out/thumbs/samurai.ppm"
fi

if [ -z "$names" ]; then
    # Source dir is empty — emit the legacy pair of procedural placeholders.
    echo "  GEN     samurai.ppm + girl.ppm (placeholders)" >&2
    "$bin" "girl" "$out/girl.ppm"
    "$bin" "girl" "$out/thumbs/girl.ppm"
    exit 0
fi

have_convert=0
if command -v convert >/dev/null 2>&1; then
    have_convert=1
fi

# Loud warning if convert is missing AND there are real source images
# to convert. Without ImageMagick every wallpaper becomes one of two
# procedural placeholders, which is confusing because the settings UI
# still lists all 9 names — the user just sees them repeat.
if [ "$have_convert" -eq 0 ] && [ -n "$names" ]; then
    echo "" >&2
    echo "  WARNING: ImageMagick (\`convert\`) not found." >&2
    echo "           All wallpapers will be procedural placeholders" >&2
    echo "           (every name in res/wallpapers/source/ ends up" >&2
    echo "           looking like samurai or girl, not the real image)." >&2
    echo "           Install it for the real wallpapers:" >&2
    echo "             Fedora: sudo dnf install ImageMagick" >&2
    echo "             Debian: sudo apt install imagemagick" >&2
    echo "             Arch:   sudo pacman -S imagemagick" >&2
    echo "             macOS:  brew install imagemagick" >&2
    echo "" >&2
fi

for name in $names; do
    src=
    for ext in png jpg jpeg; do
        if [ -f "$src_dir/$name.$ext" ]; then
            src="$src_dir/$name.$ext"
            break
        fi
    done
    dst="$out/$name.ppm"
    thumb="$out/thumbs/$name.ppm"

    if [ -n "$src" ] && [ "$have_convert" -eq 1 ]; then
        echo "  CONV    $(basename "$src") -> $name.ppm + thumb" >&2
        # Full size: 1280x800 cover-fit, P6 binary (omit -compress none).
        convert "$src" -resize 1280x800^ -gravity center \
            -extent 1280x800 -depth 8 "ppm:$dst"
        # Thumbnail: 200x120 cover-fit.
        convert "$src" -resize 200x120^ -gravity center \
            -extent 200x120 -depth 8 "ppm:$thumb"
    else
        echo "  GEN     $name.ppm + thumb (placeholder)" >&2
        "$bin" "$name" "$dst"
        "$bin" "$name" "$thumb"
    fi
done
