# SPDX-License-Identifier: GPL-2.0-or-later
# mkxp-z-3ds: the runtime for the Nintendo 3DS (mkxp-z.3dsx).
#
# Builds mkxp-z's engine (src/, binding/) with the 3DS platform files in 3ds/platform and
# 3ds/display instead of the desktop ones (main, event loop, filesystem/system helpers, audio,
# networking, the GL function table), against:
#   - CRuby 3.1.3 from 3ds/ruby/build.sh   (build-3ds/ruby)
#   - SDL2, SDL2_image, SDL2_ttf, uchardet, pixman from 3ds/deps/build.sh (build-3ds/deps/prefix)
#   - devkitPro's 3DS portlibs (PhysFS, FreeType, libpng, libjpeg, bzip2, zlib, iconv)
# The meson build for desktop platforms is not involved.
#
# Build (from the repository root, after 3ds/ruby/build.sh and 3ds/deps/build.sh):
#   cmake -S 3ds -B build-3ds/mkxp -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake"
#   cmake --build build-3ds/mkxp            # -> build-3ds/mkxp/mkxp-z.3dsx
cmake_minimum_required(VERSION 3.21) # file(COPY_FILE ... ONLY_IF_DIFFERENT) in cmake/git-hash.cmake
project(mkxp_z_3ds C CXX)

get_filename_component(MKXP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(RUBY3DS_DIR "${MKXP_ROOT}/build-3ds/ruby" CACHE PATH "Output of 3ds/ruby/build.sh")
set(DEPS3DS_DIR "${MKXP_ROOT}/build-3ds/deps/prefix" CACHE PATH "Prefix of 3ds/deps/build.sh")
foreach(f "${RUBY3DS_DIR}/build/libruby-static.a" "${DEPS3DS_DIR}/lib/libSDL2.a" "${DEPS3DS_DIR}/lib/libpixman-1.a")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "${f} missing: run 3ds/ruby/build.sh and 3ds/deps/build.sh first")
  endif()
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(PHYSFS REQUIRED physfs)

# Version: meson.build's project version. Git hash: regenerated at every build (cmake/git-hash.cmake),
# as meson's vcs_tag does in src/meson.build.
file(READ "${MKXP_ROOT}/meson.build" MESON_BUILD)
string(REGEX MATCH "version: *'([^']+)'" _ "${MESON_BUILD}")
set(MKXPZ_VERSION "${CMAKE_MATCH_1}-3ds")
find_package(Git QUIET)
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")
add_custom_target(mkxp_git_hash
  COMMAND "${CMAKE_COMMAND}" "-DROOT=${MKXP_ROOT}" "-DOUT=${GEN_DIR}/git-hash.h" "-DGIT=${GIT_EXECUTABLE}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/git-hash.cmake"
  BYPRODUCTS "${GEN_DIR}/git-hash.h" VERBATIM)

# Embedded files (shaders, bundled font), generated like assets/ and shader/meson.build do.
set(EMBEDDED)
file(GLOB SHADER_FILES "${MKXP_ROOT}/shader/*.frag" "${MKXP_ROOT}/shader/*.vert" "${MKXP_ROOT}/shader/*.h")
foreach(f ${SHADER_FILES} "${MKXP_ROOT}/assets/liberation.ttf")
  get_filename_component(name "${f}" NAME)
  string(REPLACE "." "_" sym "${name}")
  if(f MATCHES "/shader/")
    set(sym "mkxp_shader_${sym}")
  else()
    set(sym "mkxp_assets_${sym}")
  endif()
  add_custom_command(OUTPUT "${GEN_DIR}/${name}.xxd"
    COMMAND "${Python3_EXECUTABLE}" "${MKXP_ROOT}/subprojects/common/embedtool.py" "${f}" "${GEN_DIR}/${name}.xxd" "${sym}"
    DEPENDS "${f}" "${MKXP_ROOT}/subprojects/common/embedtool.py" VERBATIM)
  list(APPEND EMBEDDED "${GEN_DIR}/${name}.xxd")
endforeach()
# The pass-through PICA vertex shader of gl3ds (picasso), embedded the same way.
add_custom_command(OUTPUT "${GEN_DIR}/passthrough.shbin"
  COMMAND "${CTR_PICASSO_EXE}" -o "${GEN_DIR}/passthrough.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/display/passthrough.v.pica"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/display/passthrough.v.pica" VERBATIM)
add_custom_command(OUTPUT "${GEN_DIR}/passthrough.shbin.xxd"
  COMMAND "${Python3_EXECUTABLE}" "${MKXP_ROOT}/subprojects/common/embedtool.py" "${GEN_DIR}/passthrough.shbin" "${GEN_DIR}/passthrough.shbin.xxd" mkxp_shader_passthrough_shbin
  DEPENDS "${GEN_DIR}/passthrough.shbin" VERBATIM)
list(APPEND EMBEDDED "${GEN_DIR}/passthrough.shbin.xxd")
add_custom_target(mkxp_embedded DEPENDS ${EMBEDDED})

# Upstream sources used unchanged (or with small __3DS__ hooks).
set(UPSTREAM_SOURCES
  src/config.cpp src/sharedstate.cpp
  src/audio/fluid-fun.cpp
  src/crypto/rgssad.cpp
  src/display/autotiles.cpp src/display/autotilesvx.cpp src/display/bitmap.cpp src/display/font.cpp
  src/display/graphics.cpp src/display/plane.cpp src/display/sprite.cpp src/display/tilemap.cpp
  src/display/tilemapvx.cpp src/display/viewport.cpp src/display/window.cpp src/display/windowvx.cpp
  src/display/gl/gl-debug.cpp src/display/gl/gl-meta.cpp src/display/gl/glstate.cpp
  src/display/gl/scene.cpp src/display/gl/shader.cpp src/display/gl/texpool.cpp
  src/display/gl/tileatlas.cpp src/display/gl/tileatlasvx.cpp src/display/gl/tilequad.cpp
  src/display/gl/vertex.cpp
  src/display/libnsgif/libnsgif.c src/display/libnsgif/lzw.c
  src/etc/etc.cpp src/etc/table.cpp
  src/filesystem/filesystem.cpp
  src/input/input.cpp src/input/keybindings.cpp
  src/util/iniconfig.cpp
  binding/binding-mri.cpp binding/binding-util.cpp binding/table-binding.cpp binding/etc-binding.cpp
  binding/bitmap-binding.cpp binding/font-binding.cpp binding/graphics-binding.cpp
  binding/input-binding.cpp binding/sprite-binding.cpp binding/viewport-binding.cpp
  binding/plane-binding.cpp binding/window-binding.cpp binding/tilemap-binding.cpp
  binding/audio-binding.cpp binding/module_rpg.cpp binding/filesystem-binding.cpp
  binding/windowvx-binding.cpp binding/tilemapvx-binding.cpp binding/http-binding.cpp)
list(TRANSFORM UPSTREAM_SOURCES PREPEND "${MKXP_ROOT}/")
# 3DS replacements: main.cpp, eventthread.cpp, filesystemImpl.cpp, systemImpl.cpp, audio/*,
# net/net.cpp, display/gl/gl-fun.cpp.
set(PLATFORM_SOURCES
  platform/main3ds.cpp platform/eventthread3ds.cpp platform/filesystemImpl3ds.cpp
  platform/systemImpl3ds.cpp platform/audio3ds.cpp platform/net3ds.cpp
  display/gl3ds.cpp)

add_executable(mkxp-z ${UPSTREAM_SOURCES} ${PLATFORM_SOURCES})
add_dependencies(mkxp-z mkxp_embedded mkxp_git_hash)
set_target_properties(mkxp-z PROPERTIES CXX_STANDARD 14 CXX_EXTENSIONS ON C_STANDARD 11 C_EXTENSIONS ON)
target_compile_definitions(mkxp-z PRIVATE
  MKXPZ_VERSION="${MKXPZ_VERSION}" HAVE_NANOSLEEP MKXPZ_INIT_GL_LATER
  MKXPZ_TTF_FONT=TTF_Font MKXPZ_FLUIDSYNTH_SONAME="libfluidsynth.so")
target_include_directories(mkxp-z PRIVATE
  "${GEN_DIR}" "${MKXP_ROOT}" "${MKXP_ROOT}/src" "${MKXP_ROOT}/src/audio" "${MKXP_ROOT}/src/crypto"
  "${MKXP_ROOT}/src/display" "${MKXP_ROOT}/src/display/gl" "${MKXP_ROOT}/src/display/libnsgif"
  "${MKXP_ROOT}/src/display/libnsgif/utils" "${MKXP_ROOT}/src/etc" "${MKXP_ROOT}/src/filesystem"
  "${MKXP_ROOT}/src/input" "${MKXP_ROOT}/src/net" "${MKXP_ROOT}/src/system" "${MKXP_ROOT}/src/util"
  "${MKXP_ROOT}/src/util/sigslot" "${MKXP_ROOT}/src/util/sigslot/adapter" "${MKXP_ROOT}/binding"
  "${CMAKE_CURRENT_SOURCE_DIR}/ruby/shim/include" "${CMAKE_CURRENT_SOURCE_DIR}/display")
target_include_directories(mkxp-z SYSTEM PRIVATE
  "${DEPS3DS_DIR}/include/SDL2" "${DEPS3DS_DIR}/include/uchardet" "${DEPS3DS_DIR}/include/pixman-1"
  ${FREETYPE_INCLUDE_DIRS} ${PHYSFS_INCLUDE_DIRS}
  "${RUBY3DS_DIR}/build/.ext/include/arm-eabi" "${RUBY3DS_DIR}/src/include")
target_compile_options(mkxp-z PRIVATE -Wno-deprecated-declarations)
# Static libraries reference each other (Ruby, SDL, FreeType); one link group resolves any order.
target_link_libraries(mkxp-z PRIVATE
  -Wl,--start-group
  # extinit.a before libruby-static.a: both define Init_ext (libruby's dmyext.o is empty) and the
  # first archive in the group wins; with the empty one, `require 'zlib'` fails.
  "${RUBY3DS_DIR}/build/ext/extinit.a" "${RUBY3DS_DIR}/build/libruby-static.a"
  "${RUBY3DS_DIR}/build/ext/zlib/zlib.a" "${RUBY3DS_DIR}/build/enc/encinit.a"
  "${RUBY3DS_DIR}/build/enc/libenc.a" "${RUBY3DS_DIR}/build/enc/libtrans.a"
  "${RUBY3DS_DIR}/shim/librb3dsshim.a"
  "${DEPS3DS_DIR}/lib/libSDL2_ttf.a" "${DEPS3DS_DIR}/lib/libSDL2_image.a" "${DEPS3DS_DIR}/lib/libSDL2.a"
  "${DEPS3DS_DIR}/lib/libuchardet.a" "${DEPS3DS_DIR}/lib/libpixman-1.a"
  ${PHYSFS_STATIC_LDFLAGS} png16 jpeg ${FREETYPE_STATIC_LDFLAGS} iconv z m citro3d
  -Wl,--end-group)

ctr_generate_smdh(mkxp-z.smdh
  NAME "mkxp-z-3ds"
  DESCRIPTION "RPG Maker XP runtime (mkxp-z)"
  AUTHOR "mkxp-z-3ds / rmxp3ds")
ctr_create_3dsx(mkxp-z SMDH mkxp-z.smdh)
