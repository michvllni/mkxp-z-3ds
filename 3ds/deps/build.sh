#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SYNOPSIS
#   3ds/deps/build.sh [--build-dir DIR] [--jobs N] [--clean] [--download-dir DIR]
#
# DESCRIPTION
#   Cross-compiles the SDL2 libraries the 3DS runtime uses as utility libraries (surfaces, blits,
#   RWops, threads, image loading, font rendering; never for video or audio) as static libraries
#   for the Nintendo 3DS (devkitARM + libctru, devkitPro's CMake toolchain file):
#     SDL2 2.32.10        video, render, audio, joystick, haptic, sensor, hidapi off
#     SDL2_image 2.8.12   PNG and JPEG via libpng and libjpeg(-turbo) from devkitPro's 3DS
#                         portlibs (not stb_image: half the peak memory for large images), plus
#                         the built-in formats; no AVIF, JXL, TIFF, WebP
#     SDL2_ttf 2.24.0     FreeType from devkitPro's 3DS portlibs, no HarfBuzz
#     uchardet 0.0.8      text encoding detection (mkxp-z: Game.ini titles, config, fonts)
#     pixman 0.46.4       region tracking in mkxp-z's Bitmap (meson build, ARMv6 SIMD)
#   Release archives are downloaded and checked against the SHA-256 sums below (SDL2,
#   SDL2_image: GitHub's asset digests; SDL2_ttf: signature by the SDL release key
#   1528635D8053A57F77D1E08630A59377A7763BE6, checked when pinning; uchardet, pixman: the
#   checksums of mkxp-z's subprojects/*.wrap).
#   Output: DIR/prefix/{include,lib} with CMake and pkg-config files. Each library is rebuilt
#   only when its version, this script, the compiler or newlib, the CMake toolchain
#   file, libctru or the linked portlibs archives change, or when it is missing.
#   Needs DEVKITPRO, cmake, meson, ninja.
#
# OPTIONS
#   --build-dir DIR     build directory (default: build-3ds/deps in the repository root)
#   --jobs N            parallel build jobs (default: nproc)
#   --clean             remove DIR first
#   --download-dir DIR  where release archives are kept (default: DIR/download)
#   -h, --help          show this help
#
# EXAMPLE
#   . ~/volume/.toolchain/env.sh && 3ds/deps/build.sh --jobs 3
set -euo pipefail

usage() {
  sed -n '3,/^set -euo/{/^set -euo/d;s/^# \{0,1\}//;p}' "$0"
}

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="$REPO/build-3ds/deps"
JOBS="$(nproc)"
CLEAN=0
DL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD="$(realpath -m "${2:?--build-dir needs a value}")"; shift 2 ;;
    --jobs) JOBS="${2:?--jobs needs a value}"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --download-dir) DL="$(realpath -m "${2:?--download-dir needs a value}")"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "build.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
: "${DEVKITPRO:?DEVKITPRO is not set (source the toolchain environment)}"
for tool in cmake meson ninja arm-none-eabi-gcc; do
  command -v "$tool" >/dev/null || { echo "build.sh: $tool not on PATH" >&2; exit 1; }
done
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"
[ -f "$TOOLCHAIN" ] || { echo "build.sh: $TOOLCHAIN missing" >&2; exit 1; }

[ "$CLEAN" -eq 0 ] || rm -rf "$BUILD"
DL="${DL:-$BUILD/download}"
PREFIX="$BUILD/prefix"
mkdir -p "$BUILD" "$DL" "$PREFIX"
# Inputs besides the versions: this script, the CMake toolchain file, the compiler and newlib,
# libctru, and the portlibs archives SDL2_image and SDL2_ttf link (libpng, libjpeg, FreeType and
# FreeType's zlib and bzip2), devkitPro's meson cross-file generator and the meson version.
# Archives, not .pc files: a same-version package rebuild counts too.
PORTLIBS="$DEVKITPRO/portlibs/3ds"
INPUTS=("$0" "$TOOLCHAIN" "$(command -v arm-none-eabi-gcc)"
  "$(arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -print-file-name=libc.a)"
  "$DEVKITPRO/libctru/lib/libctru.a"
  "$PORTLIBS/lib/libpng16.a" "$PORTLIBS/lib/libjpeg.a" "$PORTLIBS/lib/libfreetype.a"
  "$PORTLIBS/lib/libz.a" "$PORTLIBS/lib/libbz2.a" "$DEVKITPRO/meson-toolchain.sh")
for f in "${INPUTS[@]}"; do
  [ -f "$f" ] || { echo "build.sh: $f missing" >&2; exit 1; }
done
INPUT_ID="$( (sha256sum "${INPUTS[@]}" | cut -d' ' -f1; meson --version) | sha256sum | cut -d' ' -f1)"

# fetch NAME URL SHA256: downloads URL to DL/NAME unless present with the right checksum.
fetch() {
  local name="$1" url="$2" sum="$3"
  if [ -f "$DL/$name" ] && echo "$sum  $DL/$name" | sha256sum -c --quiet - 2>/dev/null; then return; fi
  curl -fsSL --retry 3 -o "$DL/$name.part" "$url"
  echo "$sum  $DL/$name.part" | sha256sum -c --quiet - \
    || { echo "build.sh: checksum mismatch for $url" >&2; rm -f "$DL/$name.part"; exit 1; }
  mv "$DL/$name.part" "$DL/$name"
}

# build_lib NAME VERSION URL SHA256 CMAKE_ARGS...: unpacks, configures (CMake), builds and installs one
# library into PREFIX; skipped when the stamp matches version and inputs and the library exists.
# Environment: BUILDER=meson (default cmake), LIBNAME (library file name without lib/.a, default
# NAME), MESON_C_ARGS (extra quoted C flags for meson builds, e.g. "'-Wfoo','-Wbar'").
build_lib() {
  local name="$1" version="$2" url="$3" sum="$4"
  shift 4
  local archive="${url##*/}" stamp="$BUILD/.$name.stamp" id="$version $INPUT_ID"
  if [ "$(cat "$stamp" 2>/dev/null || true)" = "$id" ] && [ -f "$PREFIX/lib/lib${LIBNAME:-$name}.a" ]; then
    echo "$name $version: up to date"
    return
  fi
  fetch "$archive" "$url" "$sum"
  # files an older version of this library installed (SDL2 itself: the prefix is cleared below)
  local lib="${LIBNAME:-$name}"
  rm -rf "$PREFIX/include/SDL2/SDL_${name#SDL2_}.h" "$PREFIX/include/$name" "$PREFIX/include/$lib" \
    "$PREFIX/lib/cmake/$name" "$PREFIX/lib/pkgconfig/$name.pc" "$PREFIX/lib/pkgconfig/$lib.pc" \
    "$PREFIX/lib/lib$lib.a"
  rm -rf "$BUILD/src/$name-$version" "$BUILD/build/$name"
  mkdir -p "$BUILD/src" "$BUILD/build"
  tar -xf "$DL/$archive" -C "$BUILD/src"
  if [ "${BUILDER:-cmake}" = meson ]; then
    # devkitPro's meson cross file for the 3DS (same compiler and flags as 3DS.cmake)
    "$DEVKITPRO/meson-toolchain.sh" 3ds >"$BUILD/build/$name.cross"
    # Extra C flags go into the cross file: -Dc_args would replace its architecture flags.
    if [ -n "${MESON_C_ARGS:-}" ]; then
      sed -i "s|^c_args = \[|c_args = [${MESON_C_ARGS},|" "$BUILD/build/$name.cross"
    fi
    meson setup "$BUILD/build/$name" "$BUILD/src/$name-$version" --cross-file "$BUILD/build/$name.cross" \
      --buildtype=release --default-library=static --prefix="$PREFIX" --libdir=lib \
      "$@" >"$BUILD/build/$name.configure.log" 2>&1 \
      || { tail -n 30 "$BUILD/build/$name.configure.log" >&2; exit 1; }
    ninja -C "$BUILD/build/$name" -j "$JOBS" >"$BUILD/build/$name.build.log" 2>&1 \
      || { tail -n 30 "$BUILD/build/$name.build.log" >&2; exit 1; }
    ninja -C "$BUILD/build/$name" install >/dev/null
  else
    cmake -S "$BUILD/src/$name-$version" -B "$BUILD/build/$name" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DBUILD_SHARED_LIBS=OFF \
      "$@" >"$BUILD/build/$name.configure.log" 2>&1 \
      || { tail -n 30 "$BUILD/build/$name.configure.log" >&2; exit 1; }
    cmake --build "$BUILD/build/$name" -j "$JOBS"
    cmake --install "$BUILD/build/$name" >/dev/null
  fi
  echo "$id" >"$stamp"
  echo "$name $version: installed"
}

# SDL2 first: the other two find it through CMAKE_PREFIX_PATH. When SDL2 must be rebuilt, the
# whole prefix is cleared (no stale headers, CMake or pkg-config files of an older version) and all
# the others are rebuilt as well.
SDL2_VERSION=2.32.10
if [ "$(cat "$BUILD/.SDL2.stamp" 2>/dev/null || true)" != "$SDL2_VERSION $INPUT_ID" ] || [ ! -f "$PREFIX/lib/libSDL2.a" ]; then
  rm -rf "$PREFIX" "$BUILD"/.SDL2*.stamp
  mkdir -p "$PREFIX"
fi
build_lib SDL2 "$SDL2_VERSION" \
  "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" \
  5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
  -DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_AUDIO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF \
  -DSDL_SENSOR=OFF -DSDL_HIDAPI=OFF
build_lib SDL2_image 2.8.12 \
  https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.12/SDL2_image-2.8.12.tar.gz \
  393f5efb50536ec13ca4f4affb69cc9966d3c3f969e6c5e701faddf9f9785381 \
  -DSDL2IMAGE_DEPS_SHARED=OFF -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_BACKEND_STB=OFF \
  -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF
build_lib SDL2_ttf 2.24.0 \
  https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz \
  0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd \
  -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF -DSDL2TTF_SAMPLES=OFF
# uchardet: same version and checksum as mkxp-z's subprojects/uchardet.wrap (no mkxp-z patches).
build_lib uchardet 0.0.8 \
  https://www.freedesktop.org/software/uchardet/releases/uchardet-0.0.8.tar.xz \
  e97a60cfc00a1c147a674b097bb1422abd9fa78a2d9ce3f3fdcc2e78a34ac5f0 \
  -DBUILD_BINARY=OFF -DBUILD_STATIC=ON -DCHECK_SSE2=OFF
# pixman (meson): same version and checksum as mkxp-z's subprojects/pixman.wrap; mkxp-z's Bitmap
# tracks changed regions with it. ARMv6 SIMD paths on (the ARM11 MPCore has them). GCC >= 14
# rejects pixman's int32_t* vs int* mix (newlib's int32_t is long); same size on ARM, so a warning.
# No PIC (meson's default for static libraries): its thread-local cache would need __tls_get_addr.
BUILDER=meson LIBNAME=pixman-1 MESON_C_ARGS="'-Wno-error=incompatible-pointer-types'" \
  build_lib pixman 0.46.4 \
  https://cairographics.org/releases/pixman-0.46.4.tar.gz \
  d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c \
  -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled \
  -Dtimers=false -Dgnuplot=false -Dneon=disabled -Da64-neon=disabled -Darm-simd=enabled -Db_staticpic=false
