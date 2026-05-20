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
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone

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
#   USER_ELF_SRCS      : bare ELFs (own _start, no libc)
#   USER_ELF_LIBC_SRCS : libc-linked ELFs (crt0 + main + libosnos_c.a)
#
# Each <name>.c under tests/ paired with tests/<name>.lds becomes:
#   $(BUILD)/tests/<name>.elf    ring-3 ELF64 ET_EXEC
#   $(BUILD)/tests/<name>.elf.o  same bytes wrapped as data; symbols
#                                _binary_<name>_elf_start/end
USER_ELF_SRCS      := tests/user_hello.c
USER_ELF_LIBC_SRCS := tests/hello_libc.c \
                      tests/hello.c \
                      tests/echo.c \
                      tests/true.c \
                      tests/false.c \
                      tests/init.c \
                      tests/cat.c \
                      tests/touch.c \
                      tests/mkdir.c \
                      tests/rmdir.c \
                      tests/rm.c \
                      tests/mv.c \
                      tests/cp.c \
                      tests/ls.c \
                      tests/calc.c \
                      tests/osh.c \
                      tests/sleep.c \
                      tests/kill.c \
                      tests/top.c \
                      tests/libctest.c

USER_ELF_LIST := $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_SRCS)) \
                 $(patsubst %.c,$(BUILD)/%.elf,$(USER_ELF_LIBC_SRCS))
USER_ELF_OBJS := $(patsubst %.c,$(BUILD)/%.elf.o,$(USER_ELF_SRCS)) \
                 $(patsubst %.c,$(BUILD)/%.elf.o,$(USER_ELF_LIBC_SRCS))

OBJ := $(SRC_OBJ) $(RUNTIME_OBJ) $(USER_ELF_OBJS)
DEPS := $(OBJ:.o=.d) $(LIBC_OBJS:.o=.d)

$(SRC_OBJ): EXTRA_CFLAGS := -Werror

.PHONY: all clean iso run run-bios
.SECONDARY: $(USER_ELF_LIST) $(LIBC_A) $(LIBC_CRT0) $(LIBC_OBJS)
all: $(BUILD)/$(OUTPUT)

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
SD_SIZE_MB   ?= 16
SD_TMP       := $(BUILD)/sd_seed
SD_README_TXT = hola desde el disco
SD_HELLO_TXT  = fat works!

$(SD_IMG):
	@command -v mformat >/dev/null 2>&1 || { echo "error: mtools (mformat/mcopy) not found. Install: brew install mtools / sudo apt install mtools" >&2; exit 1; }
	@command -v mcopy   >/dev/null 2>&1 || { echo "error: mtools (mformat/mcopy) not found. Install: brew install mtools / sudo apt install mtools" >&2; exit 1; }
	dd if=/dev/zero of=$@ bs=1048576 count=$(SD_SIZE_MB) status=none
	mformat -i $@ ::
	mkdir -p $(SD_TMP)
	printf '%s\n' '$(SD_README_TXT)' > $(SD_TMP)/README.TXT
	printf '%s\n' '$(SD_HELLO_TXT)'  > $(SD_TMP)/HELLO.TXT
	mcopy -i $@ $(SD_TMP)/README.TXT ::README.TXT
	mcopy -i $@ $(SD_TMP)/HELLO.TXT  ::HELLO.TXT
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
	    $(QEMUFLAGS)

-include $(DEPS)

$(BUILD)/$(OUTPUT): $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/%.c.o: %.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) -c $< -o $@

# libc C objects.
$(BUILD)/$(LIBC_DIR)/%.c.o: $(LIBC_DIR)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -MMD -MP -c $< -o $@

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
$(BUILD)/tests/user_hello.elf: tests/user_hello.c tests/user_hello.lds
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -c $< -o $@.tmp.o
	$(LD) $(USER_LDFLAGS) -T tests/user_hello.lds $@.tmp.o -o $@
	rm $@.tmp.o

# libc-linked ring-3 ELF: provides main(), crt0 + libc give it _start.
# All share tests/libc.lds.
$(BUILD)/tests/%.elf: tests/%.c tests/libc.lds $(LIBC_A) $(LIBC_CRT0)
	mkdir -p "$(dir $@)"
	$(CC) $(USER_CFLAGS) -isystem $(FREESTD_INC) -I $(LIBC_INC) -c $< -o $@.tmp.o
	$(LD) $(USER_LDFLAGS) -T tests/libc.lds \
	    $(LIBC_CRT0) $@.tmp.o $(LIBC_A) -o $@
	rm $@.tmp.o

# Wrap each ELF blob in a relocatable .o so the linker can see it as
# data with symbols _binary_<sanitized_name>_start / _end.
$(BUILD)/tests/%.elf.o: $(BUILD)/tests/%.elf
	cd $(dir $<) && objcopy \
	    -B i386:x86-64 \
	    -I binary \
	    -O elf64-x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(notdir $<) $(notdir $@)

clean:
	rm -rf $(BUILD) $(SD_IMG)
