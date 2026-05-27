ARCH := x86_64
OUTPUT := kernel

BUILD := build
SRC := src
DEPSDIR := kernel-deps
RUNTIME := $(DEPSDIR)/cc-runtime/src
LIMINE_INC := $(DEPSDIR)/limine-protocol/include
FREESTD_INC := $(DEPSDIR)/freestnd-c-hdrs/include
LINKER := $(DEPSDIR)/linker-scripts/$(ARCH).lds

CC := clang
LD := ld.lld
# Use llvm-* binutils so we work cross-format on macOS. The native
# `/usr/bin/ar` invokes BSD ranlib which rejects ELF members ("not a
# mach-o file"); `objcopy` doesn't ship at all on macOS. llvm-ar and
# llvm-objcopy are format-agnostic and produce valid GNU output.
AR      := $(shell command -v llvm-ar      2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)
OBJCOPY := $(shell command -v llvm-objcopy 2>/dev/null || command -v objcopy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-objcopy)

CFLAGS := -g -O2 -pipe \
	-Wall -Wextra -std=gnu11 \
	-nostdinc -ffreestanding \
	-fno-stack-protector -fno-stack-check \
	-fno-lto -fno-PIC \
	-ffunction-sections -fdata-sections \
	-target x86_64-unknown-none-elf \
	-m64 -march=x86-64 -mabi=sysv \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-mno-red-zone -mcmodel=kernel

CPPFLAGS := \
	-I $(SRC) \
	-I $(LIMINE_INC) \
	-isystem $(FREESTD_INC) \
	-MMD -MP

LDFLAGS := \
	-m elf_x86_64 \
	-nostdlib -static \
	-z max-page-size=0x1000 \
	--gc-sections \
	-T $(LINKER)

SRC_CFILES := $(shell find $(SRC) -type f -name '*.c' | sort)
RUNTIME_CFILES := $(shell find $(RUNTIME) -type f -name '*.c' | sort)

SRC_OBJ := $(patsubst %.c,$(BUILD)/%.c.o,$(SRC_CFILES))
RUNTIME_OBJ := $(patsubst %.c,$(BUILD)/%.c.o,$(RUNTIME_CFILES))

# Freestanding ring-3 compile flags. NO -mcmodel=kernel (default
# "small" model; code lives at 0x400000). PIE off; we link ET_EXEC at
# a fixed address.
USER_CFLAGS := -g -O2 -pipe \
	-Wall -Wextra -Werror -std=gnu11 \
	-nostdinc -ffreestanding \
	-fno-stack-protector -fno-stack-check \
	-fno-PIC -fno-pie \
	-ffunction-sections -fdata-sections \
	-target x86_64-unknown-none-elf \
	-m64 -march=x86-64 -mabi=sysv \
	-mno-red-zone
# Ring-3 ELFs get x87 + SSE + SSE2 (the SysV x86_64 ABI default).
# The kernel itself stays -mno-80387 -mno-sse* (set per-file), so
# kernel code never touches FP registers; user tasks can use
# double/float freely once kmain enables the FPU (CR0/CR4 bits).
# Per-task FXSAVE isn't implemented yet, so mixing FP across
# multiple concurrent user tasks may corrupt state — single-task
# FP (like TCC compiling in the foreground) is fine.

USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -no-pie \
	-z noexecstack --gc-sections

# osnos user-side libc — built once, linked into every libc-using ELF.
LIBC_DIR     := lib/libc
LIBC_INC     := $(LIBC_DIR)/include
LIBC_CFILES  := $(shell find $(LIBC_DIR) -maxdepth 1 -name '*.c' | sort)
LIBC_SFILES  := $(shell find $(LIBC_DIR) -maxdepth 1 -name '*.S' | sort)
LIBC_OBJS    := $(patsubst %.c,$(BUILD)/%.c.o,$(LIBC_CFILES)) \
                $(patsubst %.S,$(BUILD)/%.S.o,$(LIBC_SFILES))
LIBC_A       := $(BUILD)/$(LIBC_DIR)/libosnos_c.a
LIBC_CRT0    := $(BUILD)/$(LIBC_DIR)/crt0.S.o

# User-mode ELF programs embedded into the kernel image.
#
# Layout under elfs/:
#   shell/      mini interpreters and command shells
#   tools/      Unix-style utilities (ls, cat, mv, ...)
#   net/        networking demos (httpd, selectserver, ...)
#   tests/      libc smoke tests and termios demos
#   osn-server/ reserved for FASE 10 (servers moved to ring 3)
#
# Linker scripts:
#   elfs/libc.lds            shared by every libc-linked ELF
#   elfs/tests/user_hello.lds bare ELF demo, hand-rolled _start
#
#   USER_ELF_SRCS      : bare ELFs (own _start, no libc)
#   USER_ELF_LIBC_SRCS : libc-linked ELFs (crt0 + main + libosnos_c.a)
#
# objcopy strips the directory when wrapping the ELF into a .o, so the
# symbol the kernel sees is _binary_<basename>_elf_start regardless of
# where the source lives. Basenames MUST stay unique across categories.
USER_ELF_SRCS      := elfs/tests/user_hello.c

USER_ELF_LIBC_SRCS := \
    elfs/shell/osh.c \
    elfs/tools/hello.c \
    elfs/tools/echo.c \
    elfs/tools/true.c \
    elfs/tools/false.c \
    elfs/tools/init.c \
    elfs/tools/cat.c \
    elfs/tools/touch.c \
    elfs/tools/mkdir.c \
    elfs/tools/rmdir.c \
    elfs/tools/rm.c \
    elfs/tools/mv.c \
    elfs/tools/cp.c \
    elfs/tools/ls.c \
    elfs/tools/calc.c \
    elfs/tools/sleep.c \
    elfs/tools/kill.c \
    elfs/tools/top.c \
    elfs/tools/ovi.c \
    elfs/tools/less.c \
    elfs/tools/poweroff.c \
    elfs/tools/reboot.c \
    elfs/tools/readelf.c \
    elfs/tools/mousetest.c \
    elfs/tools/head.c \
    elfs/tools/env.c \
    elfs/tools/wc.c \
    elfs/tools/pwd.c \
    elfs/tools/uname.c \
    elfs/tools/basename.c \
    elfs/tools/dirname.c \
    elfs/tools/tail.c \
    elfs/tools/seq.c \
    elfs/tools/yes.c \
    elfs/tools/tee.c \
    elfs/tools/date.c \
    elfs/tools/printf.c \
    elfs/tools/grep.c \
    elfs/tools/sort.c \
    elfs/tools/uniq.c \
    elfs/tools/cut.c \
    elfs/tools/tr.c \
    elfs/tools/banner.c \
    elfs/tools/which.c \
    elfs/tools/clear.c \
    elfs/tools/tree.c \
    elfs/net/udptest.c \
    elfs/net/echotcp.c \
    elfs/net/selecttest.c \
    elfs/net/selectserver.c \
    elfs/net/tcpclient.c \
    elfs/net/httpd.c \
    elfs/tests/hello_libc.c \
    elfs/tests/libctest.c \
    elfs/tests/ttytest.c \
    elfs/tests/envtest.c \
    elfs/tests/fptest.c \
    elfs/tests/mmaptest.c \
    elfs/tests/pipetest.c \
    elfs/tests/fbtest.c \
    elfs/tests/inputtest.c \
    elfs/tests/kerntest.c \
    elfs/tests/spawntest.c \
    elfs/tests/exectest.c \
    elfs/tests/forktest.c \
    elfs/tests/sigtest.c \
    elfs/tests/waittest.c \
    elfs/tests/sigchldtest.c \
    elfs/tests/pgrouptest.c \
    elfs/tests/alltest.c \
    elfs/tests/ofdtest.c \
    elfs/tests/ptytest.c \
    elfs/tests/unixtest.c \
    elfs/tests/udp_send.c \
    elfs/tests/udp_connect.c \
    elfs/tests/shmtest.c \
    elfs/tests/fdedgetest.c \
    elfs/tests/jobtest.c \
    elfs/tests/termtest.c \
    elfs/tests/serialtest.c \
    elfs/tests/tcctest.c \
    elfs/tests/luatest.c \
    elfs/tests/jqtest.c \
    elfs/tools/minishell.c \
    elfs/tools/term.c \
    elfs/osn-server/consrv.c \
    elfs/osn-server/kbdsrv.c \
    elfs/osn-server/shellsrv.c \
    elfs/gui/oxsrv.c \
    elfs/gui/oxnotepad.c \
    elfs/gui/oxcalc.c \
    elfs/gui/oxterm.c \
    elfs/gui/oxsettings.c \
    elfs/gui/oxfiles.c \
    elfs/gui/oxtop.c \
    elfs/gui/oxbrowser.c \
    elfs/gui/oxsqliteview.c \
    elfs/gui/oxhexedit.c \
    elfs/gui/oxlog.c \
    elfs/gui/oxmem.c \
    elfs/gui/oxipc.c \
    elfs/gui/oxnet.c \
    elfs/gui/oxjs.c \
    elfs/tools/uxsh.c

# musl-linked ELFs (vendor/musl). Declared early so USER_ELF_LIST sees it.
USER_ELF_MUSL_SRCS := elfs/tests/hello_musl.c elfs/tests/poll_musl.c elfs/tests/ls_musl.c elfs/tests/hello_dyn.c
USER_ELF_MUSL_EXTRA := $(BUILD)/elfs/tools/sqlite3.elf

USER_ELF_LIST := $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_SRCS)) \
                 $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_LIBC_SRCS)) \
                 $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_MUSL_SRCS)) \
                 $(BUILD)/elfs/tools/tcc.elf \
                 $(BUILD)/elfs/tools/lua.elf \
                 $(BUILD)/elfs/tools/jq.elf \
                 $(BUILD)/elfs/tools/sqlite3.elf \
                 $(BUILD)/elfs/tools/make.elf
# lighttpd opt-in: needs hand-crafted vendor/lighttpd/build-osnos/config.h
# which lives outside this repo. Set BUILD_LIGHTTPD=1 to include it.
ifeq ($(BUILD_LIGHTTPD),1)
USER_ELF_LIST += $(BUILD)/elfs/tools/lighttpd.elf
endif

# ---------------------------------------------------------------------------
# Fase 2 disk-resident: split the embedded set into ROM (small, kept inside
# the kernel as a recovery fallback) and disk-only (built + copied to sd.img
# at build time, NOT embedded in the kernel). The kernel binary shrinks
# from ~5 MB to ~500 KB — only the 4 critical servers stay inside.
# ---------------------------------------------------------------------------
USER_ELF_ROM_SRCS := \
    elfs/osn-server/consrv.c \
    elfs/osn-server/kbdsrv.c \
    elfs/osn-server/shellsrv.c \
    elfs/tools/banner.c \
    elfs/gui/oxsrv.c

# objcopy only the ROM set: those are the bytes that travel inside the
# kernel image. Everything else lives exclusively on /sd/bin.
USER_ELF_OBJS := $(patsubst %.c,$(BUILD)/%.elf.o,$(USER_ELF_SRCS)) \
                 $(patsubst %.c,$(BUILD)/%.elf.o,$(USER_ELF_ROM_SRCS))

OBJ := $(SRC_OBJ) $(RUNTIME_OBJ) $(USER_ELF_OBJS)
DEPS := $(OBJ:.o=.d) $(LIBC_OBJS:.o=.d)

$(SRC_OBJ): EXTRA_CFLAGS := -Werror

.PHONY: all clean iso run run-bios
.SECONDARY: $(USER_ELF_LIST) $(LIBC_A) $(LIBC_CRT0) $(LIBC_OBJS)
all: $(BUILD)/$(OUTPUT) $(SD_IMG)

# ---------------------------------------------------------------------------
# ISO + QEMU. Uses system-installed Limine (brew on macOS, distro pkg on
# Linux). LIMINE_DIR is auto-detected; override on the command line if
# Limine is in a non-standard prefix:
#   make LIMINE_DIR=/path/to/limine run-bios
# ---------------------------------------------------------------------------
IMAGE_NAME ?= osnos-$(ARCH)
ISO_ROOT   := $(BUILD)/iso_root
ISO        := $(BUILD)/$(IMAGE_NAME).iso

QEMU    ?= qemu-system-$(ARCH)
XORRISO ?= xorriso
LIMINE  ?= limine

LIMINE_DIR ?= $(shell \
    if [ -d /opt/homebrew/share/limine ]; then echo /opt/homebrew/share/limine; \
    elif [ -d /usr/local/share/limine ]; then echo /usr/local/share/limine; \
    elif [ -d /usr/share/limine ]; then echo /usr/share/limine; \
    fi)

QEMUFLAGS ?= -m 2G

# ---------------------------------------------------------------------------
# sd.img — 16 MiB FAT volume seeded with a couple of files. Built with
# mtools (mformat + mcopy) so no sudo / loopback mount is needed. On a
# 16 MiB volume mformat picks FAT16 by default, which is what our parser
# expects.
# ---------------------------------------------------------------------------
SD_IMG       := sd.img
# 32 MiB now (FASE 12 — wallpapers + GUI ELFs push past the old 16 MiB
# limit). mformat with -c 8 (4 KiB clusters) keeps the cluster count
# under 65525 so the volume stays FAT16, which is what our parser
# understands.
SD_SIZE_MB   ?= 64
SD_TMP       := $(BUILD)/sd_seed
SD_README_TXT = hola desde el disco
SD_HELLO_TXT  = fat works!
SD_LONG_TXT   = hola con nombre largo

# --------------------------------------------------------------------------
# Wallpapers (FASE 12 — ox window system).
# Generated host-side from PNGs in res/wallpapers/source/ when ImageMagick
# is available; otherwise the placeholder generator emits procedural
# anime-style PPMs. Either way the build runs unattended.
# --------------------------------------------------------------------------
WALLPAPER_TMP := $(BUILD)/wallpapers
# Wallpaper names derived from res/wallpapers/source/*.{png,jpg,jpeg}.
# `samurai` is always added so the legacy default never disappears even
# if the source dir is empty.
WALLPAPER_NAMES := $(sort samurai \
    $(patsubst res/wallpapers/source/%.png,%,$(wildcard res/wallpapers/source/*.png)) \
    $(patsubst res/wallpapers/source/%.jpg,%,$(wildcard res/wallpapers/source/*.jpg)) \
    $(patsubst res/wallpapers/source/%.jpeg,%,$(wildcard res/wallpapers/source/*.jpeg)))
WALLPAPERS    := $(addprefix $(WALLPAPER_TMP)/,$(addsuffix .ppm,$(WALLPAPER_NAMES)))
WALLPAPER_THUMBS := $(addprefix $(WALLPAPER_TMP)/thumbs/,$(addsuffix .ppm,$(WALLPAPER_NAMES)))

$(WALLPAPERS) $(WALLPAPER_THUMBS): tools/gen_wallpapers.sh tools/gen_placeholder.c
	@./tools/gen_wallpapers.sh $(WALLPAPER_TMP)

# Re-build sd.img whenever ANY built ELF changes or the build script
# changes (so the disk always reflects the freshly compiled binaries).
# Filenames inside ::/bin drop the .elf extension so they match the
# exec lookup path (proc_execve("/bin/cat") → /sd/bin/cat via aliasfs).
# $$()-quoted prereqs use secondary expansion so LIBTCC1_A / CRTI_O /
# CRTN_O — which are declared further down in this file — resolve at
# build time instead of at rule-parse time (when they'd still be empty).
.SECONDEXPANSION:
$(SD_IMG): $(USER_ELF_LIST) $(LIBC_A) $(LIBC_CRT0) $$(LIBTCC1_A) $$(CRTI_O) $$(CRTN_O) $(WALLPAPERS) $(WALLPAPER_THUMBS) GNUmakefile
	@command -v mformat >/dev/null 2>&1 || { echo "error: mtools (mformat/mcopy) not found. Install: brew install mtools / sudo apt install mtools" >&2; exit 1; }
	@command -v mcopy   >/dev/null 2>&1 || { echo "error: mtools (mformat/mcopy) not found. Install: brew install mtools / sudo apt install mtools" >&2; exit 1; }
	@command -v mmd     >/dev/null 2>&1 || { echo "error: mtools mmd not found." >&2; exit 1; }
	@echo "  GEN     $@ (FAT16 + populate /bin + /home)"
	dd if=/dev/zero of=$@ bs=1048576 count=$(SD_SIZE_MB) status=none
	mformat -i $@ -c 8 ::
	mmd -i $@ ::/bin
	mmd -i $@ ::/home
	mmd -i $@ ::/home/wallpapers
	mmd -i $@ ::/home/.fonts
	mmd -i $@ ::/home/.icons
	mmd -i $@ ::/home/apps
	mmd -i $@ ::/lib
	mmd -i $@ ::/lib/tcc
	mmd -i $@ ::/lib/tcc/include
	mmd -i $@ ::/usr
	mmd -i $@ ::/usr/include
	mmd -i $@ ::/usr/include/sys
	mmd -i $@ ::/usr/include/arpa
	mmd -i $@ ::/usr/include/netinet
	mkdir -p $(SD_TMP)
	printf '%s\n' '$(SD_README_TXT)' > $(SD_TMP)/README.TXT
	printf '%s\n' '$(SD_HELLO_TXT)'  > $(SD_TMP)/HELLO.TXT
	printf '%s\n' '$(SD_LONG_TXT)'   > $(SD_TMP)/long.txt
	printf '<!doctype html>\n<html><head><meta charset="utf-8"><title>osnos</title>\n<style>body{font-family:system-ui,sans-serif;max-width:40em;margin:3em auto;padding:1em;color:#222}h1{color:#0a0}code{background:#eee;padding:.2em .4em}</style></head>\n<body>\n<h1>Hola desde osnos</h1>\n<p>Esto es un servidor HTTP/1.0 corriendo como ELF ring-3 en osnos, sirviendo este archivo desde FAT16 en <code>/sd/index.html</code>.</p>\n<p>El path completo:</p>\n<ul>\n<li>Mac envia un GET via slirp NAT a guest:80</li>\n<li>RTL8139 driver entrega el frame</li>\n<li>eth -&gt; ip -&gt; tcp accept -&gt; recv en ring 3</li>\n<li>open + read del FAT16</li>\n<li>send con HTTP/1.0 200 OK y el HTML</li>\n</ul>\n</body></html>\n' > $(SD_TMP)/index.html
	printf '# osnos shell startup — runs at every shellsrv boot.\n# Edit to add your own commands / env vars.\nbanner\nexport PATH=/bin\nexport HOME=/home\nexport SHELL=/bin/shellsrv\nexport OSNAME=osnos\n' > $(SD_TMP)/.oshrc
	@# Seed a hello.c for TCC smoke testing — first thing you can do
	@# after `tcc -v` is `tcc -o /home/hello /home/hello.c && hello`.
	printf '#include <stdio.h>\n\nint main(void) {\n    printf("hello from tcc on osnos!\\n");\n    return 0;\n}\n' > $(SD_TMP)/hello.c
	@# Seed a test.json so jq users have something to play with:
	@#   jq '.os' /home/test.json
	@#   jq '.tools[]' /home/test.json
	@#   jq '.tools | length' /home/test.json
	printf '{\n  "os": "osnos",\n  "version": "0.11.3",\n  "kernel_mb": 1.3,\n  "self_hosting": ["tcc", "lua", "jq"],\n  "tools": [\n    {"name": "tcc",  "kind": "compiler", "lang": "c"},\n    {"name": "lua",  "kind": "interpreter", "lang": "lua"},\n    {"name": "jq",   "kind": "filter",   "lang": "json"},\n    {"name": "ovi",  "kind": "editor",   "modal": true}\n  ],\n  "tests": {"alltest": 17, "passing": 17}\n}\n' > $(SD_TMP)/test.json
	mcopy -i $@ $(SD_TMP)/README.TXT  ::/README.TXT
	mcopy -i $@ $(SD_TMP)/HELLO.TXT   ::/HELLO.TXT
	mcopy -i $@ $(SD_TMP)/long.txt    "::/My Long Filename.txt"
	mcopy -i $@ $(SD_TMP)/index.html  ::/index.html
	mcopy -i $@ $(SD_TMP)/README.TXT  ::/home/README.TXT
	mcopy -i $@ $(SD_TMP)/HELLO.TXT   ::/home/HELLO.TXT
	mcopy -i $@ $(SD_TMP)/.oshrc      ::/home/.oshrc
	mcopy -i $@ $(SD_TMP)/hello.c     ::/home/hello.c
	mcopy -i $@ $(SD_TMP)/test.json   ::/home/test.json
	@# Seed a Makefile for the make+tcc self-hosting workflow:
	@#   cd /home && make hello && ./hello
	printf '# osnos demo Makefile — try `make hello` then `./hello`\n# Build the C compiler self-hosting story: tcc + pdpmake inside osnos.\nCC = tcc\nCFLAGS = -static\n\nhello: hello.c\n\t$$(CC) $$(CFLAGS) hello.c -o hello\n\nclean:\n\trm -f hello\n\n.PHONY: clean\n' > $(SD_TMP)/Makefile
	mcopy -i $@ $(SD_TMP)/Makefile     ::/home/Makefile
	@# Default .oxrc — oxsrv reads this at boot. /bin/oxsettings rewrites
	@# it from the GUI. If left absent, oxsrv falls back to "wallpaper1".
	printf 'current_wallpaper=wallpaper1\n' > $(SD_TMP)/.oxrc
	mcopy -i $@ $(SD_TMP)/.oxrc       ::/home/.oxrc
	@# Optional TTF font for the WM chrome (Haiku-style proportional
	@# anti-aliased text). Drop any .ttf file at res/fonts/default.ttf
	@# (or run `make fetch-fonts`) and oxsrv picks it up via
	@# ox_text_init("/home/.fonts/default.ttf", 12). Without one we
	@# fall back to the built-in 8x8 bitmap.
	@if [ -f res/fonts/default.ttf ]; then \
	    mcopy -i $@ -o res/fonts/default.ttf ::/home/.fonts/default.ttf; \
	    echo "  COPY    /home/.fonts/default.ttf"; \
	fi
	@for f in res/fonts/*.ttf; do \
	    [ -f "$$f" ] || continue; \
	    base=$$(basename "$$f"); \
	    [ "$$base" = "default.ttf" ] && continue; \
	    mcopy -i $@ -o "$$f" "::/home/.fonts/$$base"; \
	done
	@# 24x24 RGBA icon set for the WM chrome (deskbar tiles, menu
	@# items, future titlebar app glyphs). Populated by
	@# `tools/fetch_icons.sh` from the Yaru theme. Each .rgba is
	@# exactly 2304 bytes; oxsrv loads them lazily on first use.
	@if ls res/icons/*.rgba >/dev/null 2>&1; then \
	    for f in res/icons/*.rgba; do \
	        base=$$(basename "$$f"); \
	        mcopy -i $@ -o "$$f" "::/home/.icons/$$base" || exit 1; \
	    done; \
	    n=$$(ls res/icons/*.rgba | wc -l | tr -d ' '); \
	    echo "  COPY    /home/.icons/  ($$n icons)"; \
	fi
	@# JavaScript apps for /bin/oxjs. Anything ending in `.js` under
	@# res/apps/ gets staged at /home/apps/ on the FAT image.
	@if ls res/apps/*.js >/dev/null 2>&1; then \
	    for f in res/apps/*.js; do \
	        base=$$(basename "$$f"); \
	        mcopy -i $@ -o "$$f" "::/home/apps/$$base" || exit 1; \
	    done; \
	    n=$$(ls res/apps/*.js | wc -l | tr -d ' '); \
	    echo "  COPY    /home/apps/  ($$n JS apps)"; \
	fi
	@# lighttpd: solo index.html ahora; /etc/lighttpd/ se crea más
	@# abajo (después de /etc) para mantener orden mmd correcto.
	mcopy -i $@ -o res/lighttpd/index.html    ::/home/index.html
	@# Wallpaper PPMs + thumbnails (FASE 12). Driven by WALLPAPER_NAMES
	@# so the disk image stays in sync with whatever's in res/wallpapers/source/.
	mmd -i $@ ::/home/wallpapers/thumbs 2>/dev/null || true
	@for name in $(WALLPAPER_NAMES); do \
	    mcopy -i $@ $(WALLPAPER_TMP)/$$name.ppm        ::/home/wallpapers/$$name.ppm        || exit 1; \
	    mcopy -i $@ $(WALLPAPER_TMP)/thumbs/$$name.ppm ::/home/wallpapers/thumbs/$$name.ppm || exit 1; \
	done
	@# SQLite demo DB (FASE 13.3) — pre-poblada con tablas books/users/
	@# checkouts + view + indices, lista para `sqlite3 /home/demo.db
	@# "SELECT ..."`. SQL fuente en res/demo.sql; regenera demo.db
	@# en disco con sqlite3 host antes de copiarla.
	@if [ -f res/demo.sql ] && command -v sqlite3 >/dev/null; then \
	    rm -f res/demo.db; sqlite3 res/demo.db < res/demo.sql; \
	fi
	@if [ -f res/demo.db ]; then \
	    mcopy -i $@ res/demo.db ::/home/demo.db; \
	    mcopy -i $@ res/demo.sql ::/home/demo.sql; \
	fi
	@# Copy every built ELF into /bin/, stripping the .elf extension so
	@# shellsrv's PATH lookup finds /bin/cat (not /bin/cat.elf).
	@for elf in $(USER_ELF_LIST); do \
	    name=$$(basename $$elf .elf); \
	    mcopy -i $@ -o $$elf ::/bin/$$name || exit 1; \
	done
	@# /lib — TCC sysroot: crt scaffolding + libc.a + libtcc1.a so
	@# `tcc foo.c -o foo` finds them in the default search path.
	mcopy -i $@ -o $(LIBC_CRT0)  ::/lib/crt1.o
	mcopy -i $@ -o $(CRTI_O)     ::/lib/crti.o
	mcopy -i $@ -o $(CRTN_O)     ::/lib/crtn.o
	mcopy -i $@ -o $(LIBC_A)     ::/lib/libc.a
	mcopy -i $@ -o $(LIBTCC1_A)  ::/lib/tcc/libtcc1.a
	@# musl dynamic linker / libc.so (FASE 14.4). En musl, libc.so
	@# es a la vez la libc dinámica Y el dynamic linker — el mismo
	@# binario serves both roles. Lo copiamos como /lib/libc.so y
	@# como /lib/ld-musl-x86_64.so.1 (lo que PT_INTERP apunta por
	@# default en cualquier dyn-linked musl ELF).
	@if [ -f $(MUSL_BUILD)/lib/libc.so ]; then \
	    mcopy -i $@ -o $(MUSL_BUILD)/lib/libc.so ::/lib/libc.so; \
	    mcopy -i $@ -o $(MUSL_BUILD)/lib/libc.so ::/lib/ld-musl-x86_64.so.1; \
	    echo "  COPY    /lib/libc.so + /lib/ld-musl-x86_64.so.1"; \
	fi
	@# BusyBox 1.36.1 — segundo shell + ~30 applets (FASE 13.1).
	@if [ -f vendor/busybox/busybox_unstripped ]; then \
	    mcopy -i $@ -o vendor/busybox/busybox_unstripped ::/bin/busybox; \
	    echo "  COPY    /bin/busybox"; \
	fi
	@# /bin/sh — copia de busybox (FAT16 no soporta symlinks). BusyBox
	@# dispatcha por argv[0]: cuando lo invocás como "/bin/sh" toma el
	@# applet sh (ash). Necesario para que `system("...")` de musl (que
	@# hace `execl("/bin/sh", "sh", "-c", cmd)`) funcione — pdpmake,
	@# popen, posix_spawn, todos pasan por ahí.
	@if [ -f vendor/busybox/busybox_unstripped ]; then \
	    mcopy -i $@ -o vendor/busybox/busybox_unstripped ::/bin/sh; \
	    echo "  COPY    /bin/sh   (busybox dispatch)"; \
	fi
	@# /etc — minimal passwd + group para que BusyBox getpwuid etc.
	@# anden. Sin esto `ls -l` y otros applets se cuelgan en lookups.
	mmd -i $@ ::/etc 2>/dev/null || true
	printf 'root:x:0:0:root:/home:/bin/ash\n' > $(SD_TMP)/passwd
	printf 'root:x:0:\n' > $(SD_TMP)/group
	printf '127.0.0.1 localhost\n10.0.2.2 host\n' > $(SD_TMP)/hosts
	@# resolv.conf con el DNS de QEMU slirp NAT (10.0.2.3 default).
	@# Sin esto, gethostbyname/getaddrinfo fallan → ping/wget/nc
	@# dicen "bad address" para hostnames externos.
	printf 'nameserver 10.0.2.3\nnameserver 8.8.8.8\n' > $(SD_TMP)/resolv.conf
	mcopy -i $@ -o $(SD_TMP)/passwd      ::/etc/passwd
	mcopy -i $@ -o $(SD_TMP)/group       ::/etc/group
	mcopy -i $@ -o $(SD_TMP)/hosts       ::/etc/hosts
	mcopy -i $@ -o $(SD_TMP)/resolv.conf ::/etc/resolv.conf
	@# lighttpd config (FASE 14.5). /etc/lighttpd/ creada acá tras /etc.
	mmd -i $@ ::/etc/lighttpd 2>/dev/null || true
	mcopy -i $@ -o res/lighttpd/lighttpd.conf ::/etc/lighttpd/lighttpd.conf
	@# TCC's own builtin headers (stdarg.h, stddef.h, stdbool.h, ...)
	@# — preferred over /usr/include/* because they use TCC-native
	@# va_list, not clang's __builtin_va_list.
	@for h in $(TCC_DIR)/include/*.h;     do mcopy -i $@ -o $$h ::/lib/tcc/include/ || exit 1; done
	@# /usr/include — full osnos libc headers so `#include <stdio.h>`
	@# resolves without -I. Copied flat per dir (mtools doesn't
	@# recurse). Adding a new subdir requires an extra mmd above.
	@for h in $(LIBC_INC)/*.h;            do mcopy -i $@ -o $$h ::/usr/include/         || exit 1; done
	@for h in $(LIBC_INC)/sys/*.h;        do mcopy -i $@ -o $$h ::/usr/include/sys/     || exit 1; done
	@for h in $(LIBC_INC)/arpa/*.h;       do mcopy -i $@ -o $$h ::/usr/include/arpa/    || exit 1; done
	@for h in $(LIBC_INC)/netinet/*.h;    do mcopy -i $@ -o $$h ::/usr/include/netinet/ || exit 1; done
	@# Compiler-provided freestanding headers (stdarg.h, stddef.h,
	@# stdint.h, ...). The host clang puts them in cc-runtime / its
	@# own resource dir; on osnos we ship them through the same VFS
	@# so user programs compiled by TCC can `#include <stdarg.h>`.
	@for h in $(FREESTD_INC)/*.h;         do mcopy -i $@ -o $$h ::/usr/include/         || exit 1; done
	@echo "  GEN     $@ done — $$(ls $(USER_ELF_LIST) | wc -l | tr -d ' ') ELFs in /bin"
	rm -rf $(SD_TMP)

iso: $(ISO)

$(ISO): $(BUILD)/$(OUTPUT) limine.conf
	@test -n "$(LIMINE_DIR)" || { echo "error: Limine not found. Install it (brew/dnf/apt) or pass LIMINE_DIR=..." >&2; exit 1; }
	@test -f "$(LIMINE_DIR)/limine-bios-cd.bin" || { echo "error: $(LIMINE_DIR) does not contain Limine boot files." >&2; exit 1; }
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(BUILD)/$(OUTPUT) $(ISO_ROOT)/boot/kernel
	cp limine.conf $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	$(XORRISO) -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(ISO_ROOT) -o $(ISO)
	$(LIMINE) bios-install $(ISO)
	rm -rf $(ISO_ROOT)

run: run-bios

run-bios: $(ISO) $(SD_IMG)
	$(QEMU) -M pc -cdrom $(ISO) -boot d \
	    -drive file=$(SD_IMG),format=raw,if=ide,index=0,media=disk \
	    -netdev user,id=net0,hostfwd=tcp::8080-:80,hostfwd=tcp::9034-:9034,hostfwd=udp::1234-:1234 \
	    -device rtl8139,netdev=net0 \
	    $(QEMUFLAGS)

-include $(DEPS)

$(BUILD)/$(OUTPUT): $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/%.c.o: %.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) -c $< -o $@

# libc C objects.
# -I src/include lets libc + user ELFs reach the shared ABI headers
# (osnos_ipc_abi.h, osnos_keys.h, osnos_status.h) — single source of
# truth shared with the kernel.
#
# Use a STATIC pattern rule (with explicit target list) instead of a
# bare pattern rule. The generic kernel pattern
# `$(BUILD)/%.c.o: %.c` above also matches the libc target paths, and
# GNU make tiebreaks pattern rules by order-of-definition — without
# this static binding the kernel rule wins and compiles libc files
# with kernel CFLAGS (no libc include path, -mcmodel=kernel, etc.),
# which only worked accidentally when the .o files were already on
# disk from a previous build.
LIBC_C_OBJS := $(filter %.c.o,$(LIBC_OBJS))
$(filter-out $(BUILD)/$(LIBC_DIR)/ox_text.c.o,$(LIBC_C_OBJS)): $(BUILD)/$(LIBC_DIR)/%.c.o: $(LIBC_DIR)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -MMD -MP -c $< -o $@

# stb_truetype amalgamated build — public-domain single-header library
# bundled inside ox_text.c. The implementation uses old-C constructs
# that trip -Wsign-compare / -Wunused-but-set / -Wmissing-prototypes,
# so we drop strictness for this one object (matches how TCC, Lua, jq
# are compiled). The rest of libc still builds with -Werror.
$(BUILD)/$(LIBC_DIR)/ox_text.c.o: $(LIBC_DIR)/ox_text.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -I vendor/stb -MMD -MP -c $< -o $@

# libc assembly objects (crt0.S).
$(BUILD)/$(LIBC_DIR)/%.S.o: $(LIBC_DIR)/%.S
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -c $< -o $@

# Bundle every .c.o into libosnos_c.a (crt0.o stays separate so user
# linker scripts can place it explicitly at the start of .text).
$(LIBC_A): $(filter-out $(LIBC_CRT0),$(LIBC_OBJS))
	mkdir -p "$(dir $@)"
	rm -f $@
	$(AR) rcs $@ $^

# Bare ring-3 ELF: source has its own _start, no libc, no crt0.
$(BUILD)/elfs/tests/user_hello.elf: elfs/tests/user_hello.c elfs/tests/user_hello.lds
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -c $< -o $@.tmp.o
	$(LD) $(USER_LDFLAGS) -T elfs/tests/user_hello.lds $@.tmp.o -o $@
	rm $@.tmp.o

# libc-linked ring-3 ELF — one pattern handles every category under
# elfs/ (% matches "tools/ls", "net/httpd", etc.). All share libc.lds.
$(BUILD)/elfs/%.elf: elfs/%.c elfs/libc.lds $(LIBC_A) $(LIBC_CRT0)
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -c $< -o $@.tmp.o
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $@.tmp.o $(LIBC_A) -o $@
	rm $@.tmp.o

# --------------------------------------------------------------------------
# BearSSL port — FASE 12.5. Constant-time TLS 1.0/1.2 library by Thomas
# Pornin (bearssl.org), MIT. Designed for embedded — fully freestanding,
# no malloc, caller-provided buffers. Vendor at vendor/bearssl/.
# ~67K LOC across 277 .c files. Output is libbearssl.a linked into
# oxbrowser. Replaces tlse (which compiled but its ClientHello wasn't
# accepted by major modern servers — Cloudflare, AWS-ELB, Google all
# closed connections immediately).
# --------------------------------------------------------------------------
BEARSSL_DIR  := vendor/bearssl
# Exclude SIMD/intrinsic-accelerated variants — they hard-#include
# <x86intrin.h> / <altivec.h> at the top of the .c file regardless of
# the BR_AES_X86NI runtime gate, so mini-libc freestanding chokes.
# BearSSL ships pure-C fallbacks for every primitive (aes_ct, aes_big,
# ghash_ctmul, etc.) which we use instead.
BEARSSL_SRCS := $(shell find $(BEARSSL_DIR)/src -name '*.c' \
                 ! -name '*x86ni*' ! -name '*pclmul*' \
                 ! -name '*pwr8*'  ! -name '*sse2*' \
                 ! -name 'sysrng.c') \
                $(BEARSSL_DIR)/osnos_glue.c
BEARSSL_OBJS := $(patsubst $(BEARSSL_DIR)/%.c,$(BUILD)/$(BEARSSL_DIR)/%.c.o,$(BEARSSL_SRCS))
BEARSSL_A    := $(BUILD)/$(BEARSSL_DIR)/libbearssl.a
BEARSSL_INC  := $(BEARSSL_DIR)/inc

# Disable all Intel intrinsics-based code paths (AES-NI, PCLMUL,
# SSE2, RDRAND) — mini-libc has no <x86intrin.h> / <immintrin.h>.
# BearSSL has pure-C fallbacks for every primitive. Slower in QEMU
# TCG but works freestanding.
BEARSSL_DEFS := -DBR_AES_X86NI=0 -DBR_GHASH_PCLMUL=0 \
                -DBR_SSE2=0 -DBR_RDRAND=0 \
                -DBR_AMD64=1 -DBR_64=1

$(BUILD)/$(BEARSSL_DIR)/%.c.o: $(BEARSSL_DIR)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) \
	    -I src/include -I $(BEARSSL_INC) -I $(BEARSSL_DIR)/src \
	    $(BEARSSL_DEFS) -c $< -o $@

$(BEARSSL_A): $(BEARSSL_OBJS)
	ar rcs $@ $^

# --------------------------------------------------------------------------
# Duktape 2.7.0 — small embeddable ECMAScript engine for JS-as-app
# scripting. ~100K LOC in a single `duktape.c` (amalgamated source).
# Compiled with `-w` because the amalgam uses idioms that trip our
# strict warning set, like every other vendored amalgamation.
# --------------------------------------------------------------------------
DUKTAPE_DIR  := vendor/duktape
DUKTAPE_SRC  := $(DUKTAPE_DIR)/src/duktape.c
DUKTAPE_OBJ  := $(BUILD)/$(DUKTAPE_DIR)/src/duktape.c.o
DUKTAPE_A    := $(BUILD)/$(DUKTAPE_DIR)/libduktape.a
DUKTAPE_INC  := $(DUKTAPE_DIR)/src

# Duktape defaults assume a hosted POSIX env. Override math macros for
# functions our mini-libc doesn't ship (cbrt); Duktape has internal
# pow()-based fallbacks when the macros are undefined.
DUKTAPE_DEFS := -UDUK_CBRT

$(DUKTAPE_OBJ): $(DUKTAPE_SRC) $(DUKTAPE_DIR)/src/duktape.h $(DUKTAPE_DIR)/src/duk_config.h
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) \
	    -I src/include -I $(DUKTAPE_INC) $(DUKTAPE_DEFS) -c $< -o $@

$(DUKTAPE_A): $(DUKTAPE_OBJ)
	$(AR) rcs $@ $^

# --------------------------------------------------------------------------
# tlse port — kept for reference but oxbrowser no longer links it. tlse's
# ClientHello had interop issues with modern servers. See FASE 12.5 notes
# in STATUS.md.
#
# NOTA orden: estas variables se definen ACÁ (antes del override de
# oxbrowser que las referencia en su prerequisite line) porque gmake
# expande prerequisites en parse time. Si TLSE_A vive más abajo en
# el archivo, $(TLSE_A) en el rule expande a empty y el link falla
# con "cannot open build/vendor/tlse/libtlse.a".
# --------------------------------------------------------------------------
TLSE_DIR     := vendor/tlse
# MP_32BIT → libtomcrypt usa mp_word=uint64_t (no __int128). Sin esto,
# el linker pide __udivti3 que no tenemos en mini-libc.
#   NO_TLS_LEGACY_SUPPORT → no listar TLS 1.0/1.1 en supported_versions.
#   Cloudflare/AWS-ELB rechazan ClientHellos que ofrezcan versiones
#   pre-1.2 (servidor cierra TCP sin responder). Habilitar SOLO 1.2/1.3.
TLSE_CPPFLAGS := -DTLS_AMALGAMATION -DNO_SSL_COMPATIBLE_INTERFACE \
                 -DMP_32BIT -DNO_TLS_LEGACY_SUPPORT
TLSE_OBJ     := $(BUILD)/$(TLSE_DIR)/tlse.c.o
TLSE_A       := $(BUILD)/$(TLSE_DIR)/libtlse.a

$(TLSE_OBJ): $(TLSE_DIR)/tlse.c $(TLSE_DIR)/libtomcrypt.c $(TLSE_DIR)/tlse.h
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) \
	    -I src/include -I $(TLSE_DIR) $(TLSE_CPPFLAGS) -c $< -o $@

$(TLSE_A): $(TLSE_OBJ)
	ar rcs $@ $^

# Override for oxbrowser: link in libbearssl.a (BearSSL — TLS 1.2
# client). Replaces libtlse.a (whose ClientHello was rejected by all
# major modern servers in our testing). Otherwise identical to the
# generic ELF pattern above.
$(BUILD)/elfs/gui/oxbrowser.elf: elfs/gui/oxbrowser.c elfs/libc.lds $(LIBC_A) $(LIBC_CRT0) $(BEARSSL_A)
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -c $< -o $@.tmp.o
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $@.tmp.o $(BEARSSL_A) $(LIBC_A) -o $@
	rm $@.tmp.o

# Override for oxjs: link in libduktape.a. The runner needs the
# Duktape headers for the script API and the static library for the
# engine itself.
$(BUILD)/elfs/gui/oxjs.elf: elfs/gui/oxjs.c elfs/libc.lds $(LIBC_A) $(LIBC_CRT0) $(DUKTAPE_A)
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -I $(DUKTAPE_INC) -c $< -o $@.tmp.o
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $@.tmp.o $(DUKTAPE_A) $(LIBC_A) -o $@
	rm $@.tmp.o

# --------------------------------------------------------------------------
# musl-linked ring-3 ELFs (FASE 12.1 — vendored musl 1.2.5).
#
# Distinct from the libc-linked path above: uses vendor/musl/build-osnos/
# (built once by the musl Makefile via ./configure + make). Apps that
# want the full musl surface (printf %f / wide chars / fnmatch / setjmp
# from a real implementation / locale) opt-in via USER_ELF_MUSL_SRCS.
#
# Use musl's own crt1.o (which calls __libc_start_main) + crti.o + crtn.o.
# Compile with -isystem on musl headers so we don't pull our minimal
# headers in. Link is purely static.
# --------------------------------------------------------------------------
MUSL_DIR     := vendor/musl
MUSL_BUILD   := $(MUSL_DIR)/build-osnos
MUSL_INC     := $(MUSL_DIR)/include $(MUSL_DIR)/arch/x86_64 $(MUSL_DIR)/arch/generic $(MUSL_BUILD)/obj/include
MUSL_LIB     := $(MUSL_BUILD)/lib/libc.a
MUSL_CRT1    := $(MUSL_BUILD)/lib/crt1.o
MUSL_CRTI    := $(MUSL_BUILD)/lib/crti.o
MUSL_CRTN    := $(MUSL_BUILD)/lib/crtn.o

MUSL_CFLAGS  := -O2 -g -nostdinc -ffreestanding -fno-stack-protector \
                -fno-PIC -fno-pie -ffunction-sections -fdata-sections \
                -target x86_64-unknown-none-elf -m64 -march=x86-64 \
                -mno-red-zone \
                $(addprefix -isystem ,$(MUSL_INC))

MUSL_LDFLAGS := -m elf_x86_64 -nostdlib -static -no-pie \
                -z noexecstack --gc-sections

$(BUILD)/elfs/tests/hello_musl.elf: elfs/tests/hello_musl.c elfs/musl.lds \
        $(MUSL_LIB) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
	mkdir -p "$(dir $@)"
	$(CC) $(MUSL_CFLAGS) -c $< -o $@.tmp.o
	$(LD) $(MUSL_LDFLAGS) -T elfs/musl.lds \
	    $(MUSL_CRT1) $(MUSL_CRTI) $@.tmp.o $(MUSL_LIB) $(MUSL_CRTN) -o $@
	rm $@.tmp.o

$(BUILD)/elfs/tests/poll_musl.elf: elfs/tests/poll_musl.c elfs/musl.lds \
        $(MUSL_LIB) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
	mkdir -p "$(dir $@)"
	$(CC) $(MUSL_CFLAGS) -c $< -o $@.tmp.o
	$(LD) $(MUSL_LDFLAGS) -T elfs/musl.lds \
	    $(MUSL_CRT1) $(MUSL_CRTI) $@.tmp.o $(MUSL_LIB) $(MUSL_CRTN) -o $@
	rm $@.tmp.o

$(BUILD)/elfs/tests/ls_musl.elf: elfs/tests/ls_musl.c elfs/musl.lds \
        $(MUSL_LIB) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
	mkdir -p "$(dir $@)"
	$(CC) $(MUSL_CFLAGS) -c $< -o $@.tmp.o
	$(LD) $(MUSL_LDFLAGS) -T elfs/musl.lds \
	    $(MUSL_CRT1) $(MUSL_CRTI) $@.tmp.o $(MUSL_LIB) $(MUSL_CRTN) -o $@
	rm $@.tmp.o

# Dynamic-linked hello (FASE 14.4). Linkea contra libc.so en vez de
# libc.a, PT_INTERP=/lib/ld-musl-x86_64.so.1. ET_EXEC con dyn segments.
# `-Bdynamic` + `--dynamic-linker` + ld.so as input, no -static.
MUSL_LIBSO   := $(MUSL_BUILD)/lib/libc.so
$(BUILD)/elfs/tests/hello_dyn.elf: elfs/tests/hello_dyn.c \
        $(MUSL_LIBSO) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
	mkdir -p "$(dir $@)"
	$(CC) $(MUSL_CFLAGS) -fPIC -c $< -o $@.tmp.o
	$(LD) -m elf_x86_64 -nostdlib -no-pie -z noexecstack \
	    --dynamic-linker=/lib/ld-musl-x86_64.so.1 \
	    --hash-style=both --allow-shlib-undefined \
	    $(MUSL_CRT1) $(MUSL_CRTI) $@.tmp.o $(MUSL_LIBSO) $(MUSL_CRTN) -o $@
	rm $@.tmp.o

# --------------------------------------------------------------------------
# SQLite 3.45.2 port — FASE 13.3. Vendor amalgamation (sqlite3.c + shell.c
# + sqlite3.h, ~290k LOC en 1 file). Linkea contra musl (necesita stdio
# completo). Defines:
#   - THREADSAFE=0   no pthread (osnos es single-task per shell)
#   - DEFAULT_MEMSTATUS=0   menos overhead
#   - OMIT_LOAD_EXTENSION   no dlopen
#   - OMIT_DEPRECATED   pruning
#   - OMIT_WAL   sin Write-Ahead-Logging (necesita file locking real)
#   - DEFAULT_TEMP_STORE=2   temp tables en memoria
#   - OMIT_AUTHORIZATION + OMIT_PROGRESS_CALLBACK   pruning
#   - HAVE_USLEEP=1   acepta sleep en us
#   - HAVE_FCHOWN=0   no chown
#   - HAVE_READLINK=0   no symlinks
#   - HAVE_LSTAT=0   alias de stat
#   - DISABLE_LFS=0   FAT16 maneja >2GB en teoría
#   - SHELL_NO_SYSCALLS=0   shell.c usa system() opcional — lo desactiva
# Build: ~6 MB ELF estático.
SQLITE_DIR    := vendor/sqlite
SQLITE_CFLAGS := -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
                 -DSQLITE_OMIT_WAL -DSQLITE_DEFAULT_TEMP_STORE=2 \
                 -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_AUTOINIT \
                 -DHAVE_USLEEP=1 -DSQLITE_USE_URI=0 \
                 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
                 -DSQLITE_ENABLE_COLUMN_METADATA \
                 -DSQLITE_DEFAULT_LOCKING_MODE=1 \
                 -DSQLITE_NO_SYNC=1 \
                 -DSQLITE_DEFAULT_MMAP_SIZE=0 -DSQLITE_MAX_MMAP_SIZE=0 \
                 -w

$(BUILD)/$(SQLITE_DIR)/sqlite3.c.o: $(SQLITE_DIR)/sqlite3.c
	mkdir -p "$(dir $@)"
	$(CC) $(MUSL_CFLAGS) $(SQLITE_CFLAGS) -I$(SQLITE_DIR) -c $< -o $@

$(BUILD)/$(SQLITE_DIR)/shell.c.o: $(SQLITE_DIR)/shell.c
	mkdir -p "$(dir $@)"
	$(CC) $(MUSL_CFLAGS) $(SQLITE_CFLAGS) -I$(SQLITE_DIR) -c $< -o $@

$(BUILD)/elfs/tools/sqlite3.elf: $(BUILD)/$(SQLITE_DIR)/sqlite3.c.o $(BUILD)/$(SQLITE_DIR)/shell.c.o \
        elfs/musl.lds $(MUSL_LIB) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
	mkdir -p "$(dir $@)"
	$(LD) $(MUSL_LDFLAGS) -T elfs/musl.lds \
	    $(MUSL_CRT1) $(MUSL_CRTI) $(BUILD)/$(SQLITE_DIR)/sqlite3.c.o $(BUILD)/$(SQLITE_DIR)/shell.c.o \
	    $(MUSL_LIB) $(MUSL_CRTN) -o $@

# --------------------------------------------------------------------------
# lighttpd 1.4.76 port — FASE 14.5. Vendor tarball, hand-crafted config.h
# en build-osnos/, lemon-generated configparser.c, subset de built-in
# mods (no auth/cgi/SSL/proxy/deflate). Linkea contra musl static.
# Output: /bin/lighttpd (~1.85 MB).
# --------------------------------------------------------------------------
LIGHT_DIR    := vendor/lighttpd
LIGHT_BUILD  := $(LIGHT_DIR)/build-osnos
LIGHT_OBJ    := $(BUILD)/vendor/lighttpd/obj
LIGHT_CFLAGS := -O2 -g -nostdinc -ffreestanding -fno-stack-protector \
                -fno-PIC -fno-pie -ffunction-sections -fdata-sections \
                -target x86_64-unknown-none-elf -m64 -march=x86-64 \
                -mabi=sysv -mno-red-zone \
                $(addprefix -isystem ,$(MUSL_INC)) \
                -I $(LIGHT_DIR)/src -I $(LIGHT_BUILD) \
                -DHAVE_CONFIG_H -w

LIGHT_SRCS := server.c response.c connections.c h1.c sock_addr_cache.c \
              network.c network_write.c fdevent_impl.c http_range.c \
              data_config.c configfile.c configparser.c \
              base64.c buffer.c burl.c log.c http_header.c http_kv.c \
              keyvalue.c chunk.c http_chunk.c fdevent.c fdevent_fdnode.c \
              gw_backend.c stat_cache.c http_etag.c array.c algo_md5.c \
              algo_sha1.c algo_splaytree.c configfile-glue.c \
              http-header-glue.c http_cgi.c http_date.c plugin.c reqpool.c \
              request.c sock_addr.c rand.c fdlog_maint.c fdlog.c \
              sys-setjmp.c ck.c \
              mod_indexfile.c mod_staticfile.c mod_access.c mod_alias.c \
              mod_setenv.c mod_expire.c mod_redirect.c mod_simple_vhost.c \
              mod_evhost.c mod_rewrite.c

LIGHT_OBJS := $(addprefix $(LIGHT_OBJ)/, $(LIGHT_SRCS:.c=.o))

# configparser.c se genera de configparser.y vía lemon (parser
# generator). Compilamos lemon en host una vez y lo cacheamos.
$(BUILD)/vendor/lighttpd/lemon:
	mkdir -p "$(dir $@)"
	clang -O2 $(LIGHT_DIR)/src/lemon.c -o $@

$(LIGHT_DIR)/src/configparser.c: $(LIGHT_DIR)/src/configparser.y $(BUILD)/vendor/lighttpd/lemon
	cp $(BUILD)/vendor/lighttpd/lemon $(LIGHT_DIR)/src/lemon-bin
	cd $(LIGHT_DIR)/src && ./lemon-bin -q configparser.y
	rm -f $(LIGHT_DIR)/src/lemon-bin

# rand.c necesita -include sys/random.h (no incluye explícito y
# musl no expone getrandom() implícitamente).
$(LIGHT_OBJ)/rand.o: $(LIGHT_DIR)/src/rand.c $(LIGHT_BUILD)/config.h
	mkdir -p "$(dir $@)"
	$(CC) $(LIGHT_CFLAGS) -include sys/random.h -c $< -o $@

$(LIGHT_OBJ)/%.o: $(LIGHT_DIR)/src/%.c $(LIGHT_BUILD)/config.h $(LIGHT_BUILD)/plugin-static.h
	mkdir -p "$(dir $@)"
	$(CC) $(LIGHT_CFLAGS) -c $< -o $@

$(BUILD)/elfs/tools/lighttpd.elf: $(LIGHT_OBJS) elfs/musl.lds \
        $(MUSL_LIB) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
	mkdir -p "$(dir $@)"
	$(LD) $(MUSL_LDFLAGS) -T elfs/musl.lds \
	    $(MUSL_CRT1) $(MUSL_CRTI) $(LIGHT_OBJS) \
	    $(MUSL_LIB) $(MUSL_CRTN) -o $@

# --------------------------------------------------------------------------
# pdpmake (Public Domain POSIX make) — FASE 14.1. Vendor 1.4.1 (~3.4K LOC).
# Linkeado contra **mini-libc** (NO musl) — la razón: musl `system()` usa
# `posix_spawn` que llama `clone(CLONE_VM|CLONE_VFORK)`, y nuestra
# implementación de SYS_CLONE (alias de fork) no maneja correctamente la
# semántica vfork-style que musl espera. Con mini-libc, system() es
# fork+execve directo (en lib/libc/stdlib.c), y todo funciona limpio.
# Stubs en mini-libc: <glob.h> retorna GLOB_NOMATCH (suficiente para
# Makefile use), <ar.h> tiene la struct pero no se usa en práctica.
# --------------------------------------------------------------------------
PDPMAKE_DIR  := vendor/pdpmake
PDPMAKE_SRCS := check.c input.c macro.c main.c make.c modtime.c rules.c target.c utils.c
PDPMAKE_OBJS := $(addprefix $(BUILD)/$(PDPMAKE_DIR)/, $(PDPMAKE_SRCS:.c=.o))

$(BUILD)/$(PDPMAKE_DIR)/%.o: $(PDPMAKE_DIR)/%.c $(PDPMAKE_DIR)/make.h
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -I$(PDPMAKE_DIR) -w -c $< -o $@

$(BUILD)/elfs/tools/make.elf: $(PDPMAKE_OBJS) elfs/libc.lds $(LIBC_A) $(LIBC_CRT0)
	mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $(PDPMAKE_OBJS) $(LIBC_A) -o $@

# --------------------------------------------------------------------------
# TinyCC port — FASE 11.0. Vendor tree at vendor/tinycc/ (release 0.9.27).
# Two compilation units: libtcc.c is the unity build (#includes tccpp.c,
# tccgen.c, tccelf.c, x86_64-gen.c, x86_64-link.c, i386-asm.c, tccasm.c,
# tccrun.c) and tcc.c is the CLI driver. -DONE_SOURCE in libtcc.c, =0 in
# tcc.c so the unity-build symbols don't double-define when linking.
#
# Warning flags: -w because TCC's old (2017) source has many sign-
# compare / format / unused warnings that -Werror would trip. Cleaner
# to silence them all than to patch the upstream tree.
# --------------------------------------------------------------------------
TCC_DIR       := vendor/tinycc
TCC_CPPFLAGS  := -I $(TCC_DIR) -DTCC_TARGET_X86_64 \
                 -DCONFIG_TCC_STATIC=1 -DCONFIG_TCCBOOT=1

$(BUILD)/$(TCC_DIR)/libtcc.c.o: $(TCC_DIR)/libtcc.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include $(TCC_CPPFLAGS) -DONE_SOURCE -c $< -o $@

$(BUILD)/$(TCC_DIR)/tcc.c.o: $(TCC_DIR)/tcc.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include $(TCC_CPPFLAGS) -DONE_SOURCE=0 -c $< -o $@

$(BUILD)/elfs/tools/tcc.elf: $(BUILD)/$(TCC_DIR)/tcc.c.o $(BUILD)/$(TCC_DIR)/libtcc.c.o elfs/libc.lds $(LIBC_A) $(LIBC_CRT0)
	mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $(BUILD)/$(TCC_DIR)/tcc.c.o $(BUILD)/$(TCC_DIR)/libtcc.c.o $(LIBC_A) -o $@

# --------------------------------------------------------------------------
# Lua 5.4 port — FASE 11.2. Vendor tree at vendor/lua/ (release 5.4.7).
# All .c files in vendor/lua/src/ compile cleanly with USER_CFLAGS and
# link against osnos libc. We compile with NEITHER LUA_USE_POSIX nor
# LUA_USE_WINDOWS — falls through to ISO C path: no signal handlers,
# no readline, no popen, no os.execute. `os.tmpname`, `os.execute`,
# `io.popen` return errors at runtime but everything else works.
#
# CORE_O + LIB_O + lua.c (CLI driver) → /bin/lua. No luac yet — bytecode
# compiler is a separate target; add when needed.
# --------------------------------------------------------------------------
LUA_DIR       := vendor/lua/src
LUA_CPPFLAGS  := -I $(LUA_DIR)
LUA_CORE_SRCS := \
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c \
    llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c \
    ltable.c ltm.c lundump.c lvm.c lzio.c
LUA_LIB_SRCS  := \
    lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c \
    loadlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c linit.c
LUA_SRCS      := $(LUA_CORE_SRCS) $(LUA_LIB_SRCS) lua.c
LUA_OBJS      := $(patsubst %.c,$(BUILD)/$(LUA_DIR)/%.c.o,$(LUA_SRCS))

$(BUILD)/$(LUA_DIR)/%.c.o: $(LUA_DIR)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include $(LUA_CPPFLAGS) -c $< -o $@

$(BUILD)/elfs/tools/lua.elf: $(LUA_OBJS) elfs/libc.lds $(LIBC_A) $(LIBC_CRT0)
	mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $(LUA_OBJS) $(LIBC_A) -o $@

# --------------------------------------------------------------------------
# jq 1.7.1 port — FASE 11.3. Vendor tree at vendor/jq/src/.
#
# We build WITHOUT_ONIG (no regex builtins — test/match/sub/gsub fail
# at runtime, language otherwise intact) which strips the ~30 KLOC
# oniguruma dependency.
#
# Pre-built parser + lexer ship in the release tarball. We pre-generate
# builtin.inc, config_opts.inc, version.h on the host once; they live
# in the vendored tree.
# --------------------------------------------------------------------------
JQ_DIR        := vendor/jq/src
JQ_CPPFLAGS   := -I vendor/jq -I $(JQ_DIR) -I $(JQ_DIR)/decNumber \
                 -DWITHOUT_ONIG=1 -DHAVE_ISATTY=1 -DHAVE_SETLOCALE=1 \
                 -DHAVE_MEMMEM=1 -DHAVE_ALLOCA_H=1 \
                 -DHAVE_DECL_INFINITY=1 -DHAVE_DECL_ISNAN=1 \
                 -DHAVE_DECL_NAN=1 -DHAVE_DECL_ISINF=1 \
                 -DHAVE_STRDUP=1 \
                 -DIEEE_8087=1
JQ_SRCS       := \
    builtin.c bytecode.c compile.c execute.c jv.c jv_alloc.c jv_aux.c \
    jv_dtoa.c jv_dtoa_tsd.c jv_file.c jv_parse.c jv_print.c \
    jv_unicode.c linker.c locfile.c util.c lexer.c parser.c main.c \
    jq_test.c \
    decNumber/decContext.c decNumber/decNumber.c
JQ_OBJS       := $(patsubst %.c,$(BUILD)/$(JQ_DIR)/%.c.o,$(JQ_SRCS))

$(BUILD)/$(JQ_DIR)/%.c.o: $(JQ_DIR)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include $(JQ_CPPFLAGS) -c $< -o $@

$(BUILD)/elfs/tools/jq.elf: $(JQ_OBJS) elfs/libc.lds $(LIBC_A) $(LIBC_CRT0)
	mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -T elfs/libc.lds \
	    $(LIBC_CRT0) $(JQ_OBJS) $(LIBC_A) -o $@

# libtcc1.a — TCC's runtime support library. Holds 64-bit divmod
# helpers, soft-float conversion, alloca, etc. Compiled with the
# same USER_CFLAGS as user ELFs so it's link-compatible. Lives in
# /lib/tcc/libtcc1.a inside osnos at runtime.
LIBTCC1_OBJS := $(BUILD)/$(TCC_DIR)/lib/libtcc1.c.o
LIBTCC1_A    := $(BUILD)/$(TCC_DIR)/lib/libtcc1.a

$(BUILD)/$(TCC_DIR)/lib/libtcc1.c.o: $(TCC_DIR)/lib/libtcc1.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -w -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -c $< -o $@

$(LIBTCC1_A): $(LIBTCC1_OBJS)
	ar rcs $@ $^

# crti.o / crtn.o stubs — empty .init / .fini sections so TCC's link
# step finds the expected glibc-style scaffolding files. crt0.S
# provides _start (acts as crt1.o).
CRTI_O := $(BUILD)/lib/sysroot/crti.o
CRTN_O := $(BUILD)/lib/sysroot/crtn.o

$(CRTI_O): lib/sysroot/crti.S
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -c $< -o $@

$(CRTN_O): lib/sysroot/crtn.S
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -c $< -o $@

# Wrap each ELF blob in a relocatable .o. objcopy `cd`s into the build
# directory so the resulting symbol is _binary_<basename>_elf_start,
# stripping the elfs/<category>/ prefix.
$(BUILD)/elfs/%.elf.o: $(BUILD)/elfs/%.elf
	cd $(dir $<) && $(OBJCOPY) \
	    -B i386:x86-64 \
	    -I binary \
	    -O elf64-x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(notdir $<) $(notdir $@)

clean:
	rm -rf $(BUILD) $(SD_IMG)

# Fetch a permissively-licensed TTF font (DejaVu Sans, Bitstream Vera
# license — free for any use). After running this once, sd.img will
# include the font and oxsrv switches to proportional anti-aliased
# chrome text. Drop other .ttf files in res/fonts/ to ship them too.
DEJAVU_VERSION ?= 2.37
DEJAVU_URL     ?= https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_$(subst .,_,$(DEJAVU_VERSION))/dejavu-fonts-ttf-$(DEJAVU_VERSION).tar.bz2
.PHONY: fetch-fonts
fetch-fonts:
	@mkdir -p res/fonts
	@if [ -f res/fonts/default.ttf ]; then \
	    echo "fetch-fonts: res/fonts/default.ttf already exists — skipping"; \
	    exit 0; \
	fi
	@command -v curl >/dev/null 2>&1 || { echo "error: curl required"; exit 1; }
	@echo "  FETCH   DejaVu Sans $(DEJAVU_VERSION)"
	@tmp=$$(mktemp -d); cd $$tmp && \
	    curl -fsSL -o dejavu.tar.bz2 "$(DEJAVU_URL)" && \
	    tar xjf dejavu.tar.bz2 && \
	    find . -name 'DejaVuSans.ttf' -exec cp {} "$$OLDPWD/res/fonts/default.ttf" \; && \
	    rm -rf $$tmp
	@if [ -f res/fonts/default.ttf ]; then \
	    echo "  OK      res/fonts/default.ttf ($$(wc -c < res/fonts/default.ttf) bytes)"; \
	    echo "          run 'make' to refresh sd.img with the font"; \
	else \
	    echo "error: download failed"; exit 1; \
	fi

# Fetch 24x24 color app icons from the Yaru theme (Ubuntu/GPL-3) and
# transcode them to raw RGBA byte streams for runtime loading. Look
# at tools/fetch_icons.sh to see / extend the per-icon mapping.
# Re-run with FETCH_ICON_ARGS=-f to overwrite cached .rgba files.
.PHONY: fetch-icons
fetch-icons:
	@tools/fetch_icons.sh $(FETCH_ICON_ARGS)
