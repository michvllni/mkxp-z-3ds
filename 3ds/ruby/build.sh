#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SYNOPSIS
#   3ds/ruby/build.sh [--build-dir DIR] [--jobs N] [--clean] [--target TARGET]
#
# DESCRIPTION
#   Cross-compiles CRuby for the Nintendo 3DS (devkitARM + libctru) as a static library:
#   the same Ruby source and patches the desktop build uses (subprojects/ruby-3.1.wrap, unpacked
#   with `meson subprojects download`), plus the 3DS patches in 3ds/ruby/patches/ and the POSIX
#   shim in 3ds/ruby/shim/. The meson-managed tree under subprojects/ is never modified: it is
#   copied to DIR/src and patched there.
#   Output: DIR/build (libruby-static.a, encinit/extinit and extension archives, config headers
#   in .ext/include/arm-eabi), DIR/src/include (Ruby headers), DIR/shim/librb3dsshim.a, and
#   3ds/ruby/shim/include/rb3ds.h for embedders. Configure output: DIR/configure.out and
#   DIR/build/config.log. Needs DEVKITPRO, meson and a host Ruby of the same version as
#   baseruby on PATH.
#
# OPTIONS
#   --build-dir DIR   build directory (default: build-3ds/ruby in the repository root)
#   --jobs N          parallel make jobs (default: nproc)
#   --clean           remove DIR first
#   --target TARGET   make target (default: all; e.g. libruby-static.a)
#   -h, --help        show this help
#
# EXAMPLE
#   . ~/volume/.toolchain/env.sh && 3ds/ruby/build.sh --jobs 3
set -euo pipefail

usage() {
  sed -n '3,/^set -euo/{/^set -euo/d;s/^# \{0,1\}//;p}' "$0"
}

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="$REPO/build-3ds/ruby"
JOBS="$(nproc)"
CLEAN=0
TARGET=all
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD="$(realpath -m "${2:?--build-dir needs a value}")"; shift 2 ;;
    --jobs) JOBS="${2:?--jobs needs a value}"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --target) TARGET="${2:?--target needs a value}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "build.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

: "${DEVKITPRO:?DEVKITPRO is not set (source the toolchain environment)}"
command -v arm-none-eabi-gcc >/dev/null || { echo "build.sh: arm-none-eabi-gcc not on PATH" >&2; exit 1; }

# wrap_value KEY: prints KEY from the [wrap-file] section of subprojects/ruby-3.1.wrap.
wrap_value() {
  sed -n "s/^$1 *= *//p" "$REPO/subprojects/ruby-3.1.wrap" | head -n1
}

RUBY_DIR="$(wrap_value directory)"           # e.g. ruby-3_1_3
RUBY_TAG="${RUBY_DIR#ruby-}"                 # 3_1_3
RUBY_VERSION_WANTED="${RUBY_TAG//_/.}"       # 3.1.3
BASERUBY="$(command -v ruby || true)"
[ -n "$BASERUBY" ] || { echo "build.sh: host ruby $RUBY_VERSION_WANTED needed as baseruby" >&2; exit 1; }
if [ "$("$BASERUBY" -e 'print RUBY_VERSION')" != "$RUBY_VERSION_WANTED" ]; then
  echo "build.sh: baseruby $("$BASERUBY" -e 'print RUBY_VERSION') != $RUBY_VERSION_WANTED" >&2
  exit 1
fi

[ "$CLEAN" -eq 0 ] || rm -rf "$BUILD"
mkdir -p "$BUILD"

# 1. Source: meson unpacks and patches exactly like the desktop build. meson never updates an
# unpacked tree, so it is unpacked again when the wrap or mkxp-z's packagefiles change.
# The marker lives in the shared tree itself, so any number of build dirs agree on it.
[ -d "$REPO/subprojects/packagefiles/ruby-3.1" ] \
  || { echo "build.sh: subprojects/packagefiles/ruby-3.1 missing" >&2; exit 1; }
UPSTREAM_ID="$( (cat "$REPO/subprojects/ruby-3.1.wrap"; cd "$REPO/subprojects/packagefiles" &&
  find ruby-3.1 -type f -print0 | sort -z | xargs -0 sha256sum) | sha256sum | cut -d' ' -f1)"
UPSTREAM_MARK="$REPO/subprojects/$RUBY_DIR/.rb3ds-upstream"
if [ ! -f "$REPO/subprojects/$RUBY_DIR/configure" ]; then
  (cd "$REPO" && meson subprojects download ruby-3.1)
  echo "$UPSTREAM_ID" >"$UPSTREAM_MARK"
elif [ "$(cat "$UPSTREAM_MARK" 2>/dev/null || true)" != "$UPSTREAM_ID" ]; then
  if [ -f "$UPSTREAM_MARK" ]; then
    # packagefiles or wrap changed since this tree was unpacked: unpack again
    rm -rf "${REPO:?}/subprojects/$RUBY_DIR"
    (cd "$REPO" && meson subprojects download ruby-3.1)
  fi
  # (no marker: a tree unpacked by the desktop build or before markers existed is current)
  echo "$UPSTREAM_ID" >"$UPSTREAM_MARK"
fi

# 2. Private copy + 3DS patches; redone when the patch set or the wrap changes.
# The force-included shim headers count as patches: Ruby's Makefile does not track them.
PATCH_ID="$( (echo "$UPSTREAM_ID"; cat "$HERE"/patches/*.patch "$HERE"/shim/include/*.h \
  "$HERE"/shim/include/sys/*.h) | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$BUILD/src/.rb3ds-patches" 2>/dev/null || true)" != "$PATCH_ID" ]; then
  rm -rf "$BUILD/src" "$BUILD/build"
  cp -a "$REPO/subprojects/$RUBY_DIR" "$BUILD/src"
  for p in "$HERE"/patches/*.patch; do
    patch -d "$BUILD/src" -p1 --no-backup-if-mismatch -s <"$p"
  done
  echo "$PATCH_ID" >"$BUILD/src/.rb3ds-patches"
fi

# 3. Shim library (POSIX functions newlib/libctru lack).
ARCH_FLAGS=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations)
mkdir -p "$BUILD/shim"
arm-none-eabi-gcc "${ARCH_FLAGS[@]}" -std=gnu17 -O2 -Wall -Wextra -Werror -ffunction-sections \
  -I"$HERE/shim/include" -I"$DEVKITPRO/libctru/include" \
  -c "$HERE/shim/rb3ds_shim.c" -o "$BUILD/shim/rb3ds_shim.o"
rm -f "$BUILD/shim/librb3dsshim.a"
arm-none-eabi-ar rcs "$BUILD/shim/librb3dsshim.a" "$BUILD/shim/rb3ds_shim.o"

# 4. Configure (again whenever its arguments change).
# -std=gnu17: Ruby 3.1 predates C23 (GCC >= 15 default), whose `()` == `(void)` breaks its
# function-pointer casts. TRANSIENT_HEAP_TOTAL_SIZE: 4 MiB instead of 32 MiB allocated up front.
# USE_THREAD_CACHE=0: a reused native thread skips native_thread_create, which is where the
# 3DS learns a thread's stack size (patch 0001); also frees thread stacks at once.
# ASFLAGS: Ruby assembles .S files with ASFLAGS only (ARCH_FLAG is empty when cross-compiling);
# -mfpu=vfp is the ARM11 MPCore's VFPv2. ac_cv_*: features newlib declares but the 3DS lacks;
# fcntl=no: libctru's fcntl() only handles sockets (ENOTSOCK on files), e.g. `load` opens files
# O_NONBLOCK and clears it with F_SETFL.
CFLAGS_3DS="-std=gnu17 ${ARCH_FLAGS[*]} -ffunction-sections -fdata-sections -O2 -D__3DS__ -D_GNU_SOURCE \
-DTRANSIENT_HEAP_TOTAL_SIZE=0x400000 -DUSE_THREAD_CACHE=0 \
-I$HERE/shim/include -include rb3ds_compat.h \
-I$DEVKITPRO/libctru/include -I$DEVKITPRO/portlibs/3ds/include"
CONF_ARGS=(
      --host=arm-none-eabi --prefix=/
      --with-baseruby="$BASERUBY"
      --disable-shared --disable-install-doc --disable-rubygems --disable-jit-support
      --with-static-linked-ext --disable-dln --without-gmp
      --with-coroutine=arm32
      --with-ext=zlib "--with-out-ext=openssl,readline,dbm,gdbm"
      CC=arm-none-eabi-gcc AR=arm-none-eabi-ar RANLIB=arm-none-eabi-ranlib
      CFLAGS="$CFLAGS_3DS"
      ASFLAGS="${ARCH_FLAGS[*]} -mfpu=vfp"
      LDFLAGS="-specs=3dsx.specs -L$BUILD/shim -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib"
      LIBS="-lrb3dsshim -lctru -lm"
      ac_cv_func_fork=no ac_cv_func_vfork=no
      ac_cv_func_fcntl=no
      ac_cv_func_mmap=no ac_cv_header_sys_mman_h=no
      ac_cv_func_sigaction=no ac_cv_func_sigaltstack=no ac_cv_func_getcontext=no
      ac_cv_func_dlopen=no ac_cv_lib_dl_dlopen=no
      ac_cv_func___builtin_setjmp=no
      ac_cv_func_crypt=no ac_cv_header_crypt_h=no ac_cv_lib_crypt_crypt=no
      rb_cv_gcc_atomic_builtins=yes rb_cv_gcc_sync_builtins=no
      rb_cv_stack_grow_dir_arm=-1
      rb_cv_binary_elf=no
)
# The shim library takes part in configure's link tests, so its source counts too.
CONF_ID="$( (printf '%s\n' "${CONF_ARGS[@]}"; cat "$HERE/shim/rb3ds_shim.c") | sha256sum | cut -d' ' -f1)"
if [ ! -f "$BUILD/build/Makefile" ] || [ "$(cat "$BUILD/build/.rb3ds-configure" 2>/dev/null || true)" != "$CONF_ID" ]; then
  rm -rf "$BUILD/build"
  mkdir -p "$BUILD/build"
  (cd "$BUILD/build" && "$BUILD/src/configure" "${CONF_ARGS[@]}" >"$BUILD/configure.out" 2>&1) \
    || { tail -n 30 "$BUILD/configure.out" >&2; exit 1; }
  echo "$CONF_ID" >"$BUILD/build/.rb3ds-configure"
fi

# 5. Build.
make -C "$BUILD/build" -j"$JOBS" "$TARGET"

# 6. ABI check: coroutine/arm32/Context.S must save d8-d15 (patch 0004). If it was assembled
# without the hard-float flags, Context.h and Context.S disagree on the frame size and every
# new Fiber jumps to address 0.
if [ -f "$BUILD/build/libruby-static.a" ]; then
  if ! arm-none-eabi-objdump -d "$BUILD/build/coroutine/arm32/Context.o" | grep -q 'vpush'; then
    echo "build.sh: coroutine_transfer does not save d8-d15 (Context.S built without hard-float flags?)" >&2
    exit 1
  fi
fi
