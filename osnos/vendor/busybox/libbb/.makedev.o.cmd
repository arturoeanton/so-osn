cmd_libbb/makedev.o := /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/busybox/osnos-cc-wrapper.sh -Wp,-MD,libbb/.makedev.o.d  -std=gnu99 -Iinclude -Ilibbb  -include include/autoconf.h -D_GNU_SOURCE -DNDEBUG  -DBB_VER='"1.36.1"' -Wall -Wshadow -Wwrite-strings -Wundef -Wstrict-prototypes -Wunused -Wunused-parameter -Wunused-function -Wunused-value -Wmissing-prototypes -Wmissing-declarations -Wno-format-security -Wdeclaration-after-statement -Wold-style-definition -finline-limit=0 -fno-builtin-strlen -fomit-frame-pointer -ffunction-sections -fdata-sections -funsigned-char -static-libgcc -falign-functions=1 -falign-jumps=1 -falign-labels=1 -falign-loops=1 -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-printf -Oz    -DKBUILD_BASENAME='"makedev"'  -DKBUILD_MODNAME='"makedev"' -c -o libbb/makedev.o libbb/makedev.c

deps_libbb/makedev.o := \
  libbb/makedev.c \
  include/platform.h \
    $(wildcard include/config/werror.h) \
    $(wildcard include/config/big/endian.h) \
    $(wildcard include/config/little/endian.h) \
    $(wildcard include/config/nommu.h) \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/limits.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/features.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/build-osnos/obj/include/bits/alltypes.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/limits.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/byteswap.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/stdint.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/stdint.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/endian.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/stdbool.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/unistd.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/posix.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/sysmacros.h \

libbb/makedev.o: $(deps_libbb/makedev.o)

$(deps_libbb/makedev.o):
