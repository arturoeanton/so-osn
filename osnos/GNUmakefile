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
    elfs/gui/oxsettings.c

USER_ELF_LIST := $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_SRCS)) \
                 $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_LIBC_SRCS)) \
                 $(BUILD)/elfs/tools/tcc.elf \
                 $(BUILD)/elfs/tools/lua.elf \
                 $(BUILD)/elfs/tools/jq.elf

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
SD_SIZE_MB   ?= 32
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
WALLPAPERS    := $(WALLPAPER_TMP)/samurai.ppm $(WALLPAPER_TMP)/girl.ppm

$(WALLPAPERS): tools/gen_wallpapers.sh tools/gen_placeholder.c
	@./tools/gen_wallpapers.sh $(WALLPAPER_TMP)

# Re-build sd.img whenever ANY built ELF changes or the build script
# changes (so the disk always reflects the freshly compiled binaries).
# Filenames inside ::/bin drop the .elf extension so they match the
# exec lookup path (proc_execve("/bin/cat") → /sd/bin/cat via aliasfs).
$(SD_IMG): $(USER_ELF_LIST) $(LIBC_A) $(LIBC_CRT0) $(LIBTCC1_A) $(CRTI_O) $(CRTN_O) $(WALLPAPERS) GNUmakefile
	@command -v mformat >/dev/null 2>&1 || { echo "error: mtools (mformat/mcopy) not found. Install: brew install mtools / sudo apt install mtools" >&2; exit 1; }
	@command -v mcopy   >/dev/null 2>&1 || { echo "error: mtools (mformat/mcopy) not found. Install: brew install mtools / sudo apt install mtools" >&2; exit 1; }
	@command -v mmd     >/dev/null 2>&1 || { echo "error: mtools mmd not found." >&2; exit 1; }
	@echo "  GEN     $@ (FAT16 + populate /bin + /home)"
	dd if=/dev/zero of=$@ bs=1048576 count=$(SD_SIZE_MB) status=none
	mformat -i $@ -c 8 ::
	mmd -i $@ ::/bin
	mmd -i $@ ::/home
	mmd -i $@ ::/home/wallpapers
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
	@# Default .oxrc — oxsrv reads this at boot. /bin/oxsettings rewrites
	@# it from the GUI. If left absent, oxsrv falls back to "samurai".
	printf 'current_wallpaper=samurai\n' > $(SD_TMP)/.oxrc
	mcopy -i $@ $(SD_TMP)/.oxrc       ::/home/.oxrc
	@# Wallpaper PPMs (FASE 12).
	mcopy -i $@ $(WALLPAPER_TMP)/samurai.ppm ::/home/wallpapers/samurai.ppm
	mcopy -i $@ $(WALLPAPER_TMP)/girl.ppm    ::/home/wallpapers/girl.ppm
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
$(BUILD)/$(LIBC_DIR)/%.c.o: $(LIBC_DIR)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -I src/include -MMD -MP -c $< -o $@

# libc assembly objects (crt0.S).
$(BUILD)/$(LIBC_DIR)/%.S.o: $(LIBC_DIR)/%.S
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -c $< -o $@

# Bundle every .c.o into libosnos_c.a (crt0.o stays separate so user
# linker scripts can place it explicitly at the start of .text).
$(LIBC_A): $(filter-out $(LIBC_CRT0),$(LIBC_OBJS))
	mkdir -p "$(dir $@)"
	ar rcs $@ $^

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
	cd $(dir $<) && objcopy \
	    -B i386:x86-64 \
	    -I binary \
	    -O elf64-x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(notdir $<) $(notdir $@)

clean:
	rm -rf $(BUILD) $(SD_IMG)
