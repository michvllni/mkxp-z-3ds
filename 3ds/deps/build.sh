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
#     SDL2_image 2.8.12   stb_image backend (PNG, JPG) plus the built-in formats; no AVIF, JXL,
#                         TIFF, WebP
#     SDL2_ttf 2.24.0     FreeType from devkitPro's 3DS portlibs, no HarfBuzz
#   Release archives are downloaded from GitHub and checked against the SHA-256 sums below
#   (SDL2, SDL2_image: GitHub's asset digests; SDL2_ttf: signature by the SDL release key
#   1528635D8053A57F77D1E08630A59377A7763BE6, checked when pinning).
#   Output: DIR/prefix/{include,lib} with CMake and pkg-config files. Each library is rebuilt
#   only when its version or this script changes. Needs DEVKITPRO, cmake, ninja.
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
for tool in cmake ninja arm-none-eabi-gcc; do
  command -v "$tool" >/dev/null || { echo "build.sh: $tool not on PATH" >&2; exit 1; }
done
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"
[ -f "$TOOLCHAIN" ] || { echo "build.sh: $TOOLCHAIN missing" >&2; exit 1; }

[ "$CLEAN" -eq 0 ] || rm -rf "$BUILD"
DL="${DL:-$BUILD/download}"
PREFIX="$BUILD/prefix"
mkdir -p "$BUILD" "$DL" "$PREFIX"
SCRIPT_ID="$(sha256sum "$0" | cut -d' ' -f1)"

# fetch NAME URL SHA256: downloads URL to DL/NAME unless present with the right checksum.
fetch() {
  local name="$1" url="$2" sum="$3"
  if [ -f "$DL/$name" ] && echo "$sum  $DL/$name" | sha256sum -c --quiet - 2>/dev/null; then return; fi
  curl -fsSL --retry 3 -o "$DL/$name.part" "$url"
  echo "$sum  $DL/$name.part" | sha256sum -c --quiet - \
    || { echo "build.sh: checksum mismatch for $url" >&2; rm -f "$DL/$name.part"; exit 1; }
  mv "$DL/$name.part" "$DL/$name"
}

# build_lib NAME VERSION URL SHA256 CMAKE_ARGS...: unpacks, configures, builds and installs one
# library into PREFIX; skipped when the stamp matches version and script.
build_lib() {
  local name="$1" version="$2" url="$3" sum="$4"
  shift 4
  local archive="$name-$version.tar.gz" stamp="$BUILD/.$name.stamp" id="$version $SCRIPT_ID"
  if [ "$(cat "$stamp" 2>/dev/null || true)" = "$id" ]; then
    echo "$name $version: up to date"
    return
  fi
  fetch "$archive" "$url" "$sum"
  rm -rf "$BUILD/src/$name-$version" "$BUILD/build/$name"
  mkdir -p "$BUILD/src" "$BUILD/build"
  tar -xzf "$DL/$archive" -C "$BUILD/src"
  cmake -S "$BUILD/src/$name-$version" -B "$BUILD/build/$name" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DBUILD_SHARED_LIBS=OFF \
    "$@" >"$BUILD/build/$name.configure.log" 2>&1 \
    || { tail -n 30 "$BUILD/build/$name.configure.log" >&2; exit 1; }
  cmake --build "$BUILD/build/$name" -j "$JOBS"
  cmake --install "$BUILD/build/$name" >/dev/null
  echo "$id" >"$stamp"
  echo "$name $version: installed"
}

# SDL2 first: the other two find it through CMAKE_PREFIX_PATH. A rebuilt SDL2 invalidates them.
SDL_STAMP_BEFORE="$(cat "$BUILD/.SDL2.stamp" 2>/dev/null || true)"
build_lib SDL2 2.32.10 \
  https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
  5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
  -DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_AUDIO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF \
  -DSDL_SENSOR=OFF -DSDL_HIDAPI=OFF
if [ "$SDL_STAMP_BEFORE" != "$(cat "$BUILD/.SDL2.stamp")" ]; then
  rm -f "$BUILD/.SDL2_image.stamp" "$BUILD/.SDL2_ttf.stamp"
fi
build_lib SDL2_image 2.8.12 \
  https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.12/SDL2_image-2.8.12.tar.gz \
  393f5efb50536ec13ca4f4affb69cc9966d3c3f969e6c5e701faddf9f9785381 \
  -DSDL2IMAGE_DEPS_SHARED=OFF -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_SAMPLES=OFF \
  -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF
build_lib SDL2_ttf 2.24.0 \
  https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz \
  0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd \
  -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF -DSDL2TTF_SAMPLES=OFF
