cmd_coreutils/env.o := /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/busybox/osnos-cc-wrapper.sh -Wp,-MD,coreutils/.env.o.d  -std=gnu99 -Iinclude -Ilibbb  -include include/autoconf.h -D_GNU_SOURCE -DNDEBUG  -DBB_VER='"1.36.1"' -Wall -Wshadow -Wwrite-strings -Wundef -Wstrict-prototypes -Wunused -Wunused-parameter -Wunused-function -Wunused-value -Wmissing-prototypes -Wmissing-declarations -Wno-format-security -Wdeclaration-after-statement -Wold-style-definition -finline-limit=0 -fno-builtin-strlen -fomit-frame-pointer -ffunction-sections -fdata-sections -funsigned-char -static-libgcc -falign-functions=1 -falign-jumps=1 -falign-labels=1 -falign-loops=1 -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-printf -Oz    -DKBUILD_BASENAME='"env"'  -DKBUILD_MODNAME='"env"' -c -o coreutils/env.o coreutils/env.c

deps_coreutils/env.o := \
  coreutils/env.c \
    $(wildcard include/config/env.h) \
  include/libbb.h \
    $(wildcard include/config/feature/shadowpasswds.h) \
    $(wildcard include/config/use/bb/shadow.h) \
    $(wildcard include/config/selinux.h) \
    $(wildcard include/config/feature/utmp.h) \
    $(wildcard include/config/locale/support.h) \
    $(wildcard include/config/use/bb/pwd/grp.h) \
    $(wildcard include/config/lfs.h) \
    $(wildcard include/config/feature/buffers/go/on/stack.h) \
    $(wildcard include/config/feature/buffers/go/in/bss.h) \
    $(wildcard include/config/extra/cflags.h) \
    $(wildcard include/config/variable/arch/pagesize.h) \
    $(wildcard include/config/feature/verbose.h) \
    $(wildcard include/config/feature/etc/services.h) \
    $(wildcard include/config/feature/ipv6.h) \
    $(wildcard include/config/feature/seamless/xz.h) \
    $(wildcard include/config/feature/seamless/lzma.h) \
    $(wildcard include/config/feature/seamless/bz2.h) \
    $(wildcard include/config/feature/seamless/gz.h) \
    $(wildcard include/config/feature/seamless/z.h) \
    $(wildcard include/config/float/duration.h) \
    $(wildcard include/config/feature/check/names.h) \
    $(wildcard include/config/feature/prefer/applets.h) \
    $(wildcard include/config/long/opts.h) \
    $(wildcard include/config/feature/pidfile.h) \
    $(wildcard include/config/feature/syslog.h) \
    $(wildcard include/config/feature/syslog/info.h) \
    $(wildcard include/config/warn/simple/msg.h) \
    $(wildcard include/config/feature/individual.h) \
    $(wildcard include/config/shell/ash.h) \
    $(wildcard include/config/shell/hush.h) \
    $(wildcard include/config/echo.h) \
    $(wildcard include/config/sleep.h) \
    $(wildcard include/config/printf.h) \
    $(wildcard include/config/test.h) \
    $(wildcard include/config/test1.h) \
    $(wildcard include/config/test2.h) \
    $(wildcard include/config/kill.h) \
    $(wildcard include/config/killall.h) \
    $(wildcard include/config/killall5.h) \
    $(wildcard include/config/chown.h) \
    $(wildcard include/config/ls.h) \
    $(wildcard include/config/xxx.h) \
    $(wildcard include/config/route.h) \
    $(wildcard include/config/feature/hwib.h) \
    $(wildcard include/config/desktop.h) \
    $(wildcard include/config/feature/crond/d.h) \
    $(wildcard include/config/feature/setpriv/capabilities.h) \
    $(wildcard include/config/run/init.h) \
    $(wildcard include/config/feature/securetty.h) \
    $(wildcard include/config/pam.h) \
    $(wildcard include/config/use/bb/crypt.h) \
    $(wildcard include/config/feature/adduser/to/group.h) \
    $(wildcard include/config/feature/del/user/from/group.h) \
    $(wildcard include/config/ioctl/hex2str/error.h) \
    $(wildcard include/config/feature/editing.h) \
    $(wildcard include/config/feature/editing/history.h) \
    $(wildcard include/config/feature/tab/completion.h) \
    $(wildcard include/config/feature/username/completion.h) \
    $(wildcard include/config/feature/editing/fancy/prompt.h) \
    $(wildcard include/config/feature/editing/savehistory.h) \
    $(wildcard include/config/feature/editing/vi.h) \
    $(wildcard include/config/feature/editing/save/on/exit.h) \
    $(wildcard include/config/pmap.h) \
    $(wildcard include/config/feature/show/threads.h) \
    $(wildcard include/config/feature/ps/additional/columns.h) \
    $(wildcard include/config/feature/topmem.h) \
    $(wildcard include/config/feature/top/smp/process.h) \
    $(wildcard include/config/pgrep.h) \
    $(wildcard include/config/pkill.h) \
    $(wildcard include/config/pidof.h) \
    $(wildcard include/config/sestatus.h) \
    $(wildcard include/config/unicode/support.h) \
    $(wildcard include/config/feature/mtab/support.h) \
    $(wildcard include/config/feature/clean/up.h) \
    $(wildcard include/config/feature/devfs.h) \
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
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/ctype.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/dirent.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/dirent.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/errno.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/errno.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/fcntl.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/fcntl.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/inttypes.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/netdb.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/netinet/in.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/socket.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/socket.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/setjmp.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/setjmp.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/signal.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/signal.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/paths.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/stdio.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/stdlib.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/alloca.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/stdarg.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/stddef.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/string.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/strings.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/libgen.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/poll.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/poll.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/ioctl.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/ioctl.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/ioctl_fix.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/mman.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/mman.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/resource.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/time.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/select.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/resource.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/stat.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/x86_64/bits/stat.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/types.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/sysmacros.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/wait.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/termios.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/termios.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/time.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/param.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/pwd.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/grp.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/mntent.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/statfs.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/sys/statvfs.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/arch/generic/bits/statfs.h \
  /Users/arturoeanton/github.com/arturoeanton/so-osn/osnos/vendor/musl/include/arpa/inet.h \
  include/xatonum.h \

coreutils/env.o: $(deps_coreutils/env.o)

$(deps_coreutils/env.o):
