<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# mkxp-z for the Nintendo 3DS

The 3DS build of mkxp-z (`mkxp-z.3dsx`), the runtime of
[rmxp3ds](https://github.com/michvllni/rmxp3ds). It targets the New 3DS and RGSS1 (RPG Maker XP).

## Build
From the repository root, with devkitARM/libctru (`DEVKITPRO`), cmake, meson, ninja and a host Ruby
3.1.3 (the rmxp3ds dev container has all of them):

```sh
3ds/ruby/build.sh --jobs 4      # CRuby 3.1.3            -> build-3ds/ruby/
3ds/deps/build.sh --jobs 4      # SDL2 libs, uchardet, pixman -> build-3ds/deps/prefix/
cmake -S 3ds -B build-3ds/mkxp -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake"
cmake --build build-3ds/mkxp    # -> build-3ds/mkxp/mkxp-z.3dsx
```

`3ds/CMakeLists.txt` compiles mkxp-z's own sources from `src/` and `binding/` and the 3DS files
listed below. It links CRuby, the libraries from `3ds/deps` and devkitPro's 3DS portlibs (PhysFS,
FreeType, libpng, libjpeg, bzip2, zlib, iconv). The meson build for desktop platforms is not
involved.

## How the port is organised
- **Upstream code as is:** the RGSS classes and their Ruby bindings, Config, the filesystem layer
  (PhysFS), Input, Font, the scene graph, and mkxp-z's GPU wrapper layer (`src/display/gl`:
  TEX/FBO, buffers, Quad, ShaderSet, GLState).
- **The GL function table** (`gl`, `src/display/gl/gl-fun.h`) is the only way the display code
  reaches the GPU. On the 3DS it is `display/gl3ds.cpp`, an implementation of that GL ES 2 subset
  for the PICA200 (rmxp3ds `docs/DECISIONS.md` D14). **Current state:** bookkeeping only. Object
  names, compile and link status and uniform locations work, but nothing is drawn, and
  `ReadPixels` returns transparent black. The citro3d implementation comes next. It must:
  - store textures larger than the PICA200's 1024 px as several pieces (the table reports 4096,
    so mkxp-z's tileset atlas fits real tilesets);
  - honour `GL_UNPACK_ROW_LENGTH`/`SKIP_*` in `TexSubImage2D` (`gl.unpack_subimage` is set).
- **3DS replacements** for desktop-only files:

| Upstream | 3DS | |
|---|---|---|
| `src/main.cpp` | `platform/main3ds.cpp` | RGSS thread with a 1 MiB stack, game directory, stderr |
| `src/eventthread.cpp` | `platform/eventthread3ds.cpp` | buttons and circle pad as a game controller, APT loop |
| `src/filesystem/filesystemImpl.cpp` | `platform/filesystemImpl3ds.cpp` | POSIX and `sdmc:/` paths instead of ghc::filesystem |
| `src/system/systemImpl.cpp` | `platform/systemImpl3ds.cpp` | system language from the console settings |
| `src/audio/*.cpp` (OpenAL) | `platform/audio3ds.cpp` | silent for now; ndsp later |
| `src/net/net.cpp` (cpp-httplib) | `platform/net3ds.cpp` | HTTPLite raises "not available" |
| `src/display/gl/gl-fun.cpp` | `display/gl3ds.cpp` | the GL function table (see above) |

- **Hooks in upstream files**, all marked `mkxp-z-3ds` and inert on other platforms:
  - `src/system/system.h`: `MKXPZ_PLATFORM_3DS`.
  - `binding/binding-mri.cpp`: `System.platform` returns "Nintendo 3DS".
  - `src/display/graphics.cpp`: movie playback (Theora + OpenAL) is left out;
    `Graphics.play_movie` logs and returns, as if the movie had ended.
  - `src/input/input.cpp`: `Input.controller_name` returns "Nintendo 3DS".
- **MIDI:** fluidsynth is loaded at run time as on desktop. That fails on the 3DS, so MIDI is
  disabled.

## Running
- **Game directory:** `argv[1]` when given (3dslink, launcher arguments), otherwise
  `sdmc:/rmxp3ds/game`. `mkxp.json` and `Game.ini` are read from there, as upstream does. For
  tests, `"customScript": "file.rb"` in `mkxp.json` runs a single script instead of
  `Data/Scripts.rxdata`.
- **Controls:**

  | 3DS | RGSS |
  |---|---|
  | A | C (confirm) |
  | B | B (cancel) |
  | X | A |
  | Y | X |
  | L/R | L/R |
  | ZL/ZR | Y/Z |
  | D-pad or circle pad | directions |

  These are mkxp-z's default controller bindings.
- **Logs and messages:**
  - Without 3dslink, stderr (Ruby errors, `Debug()` output, `print`/`msgbox` text) goes to
    `sdmc:/rmxp3ds/mkxp-stderr.txt`.
  - Message boxes are printed on the bottom-screen console and do not block. A blocking box needs
    the renderer.
  - When the game ends with an error, or after a message box (e.g. an uncaught Ruby exception),
    the message stays on the bottom screen until START is pressed. With
    `sdmc:/rmxp3ds/test/autoexit` it exits after 3 s.
- **HOME menu and sleep mode** pause the game at its next `Graphics.update` (APT hook,
  `SyncPoint`).

## Known limitations (this stage)
- **No picture yet:** the GL function table does not draw. Bitmap pixel reads (`get_pixel`) return
  0.
- No sound, no movies (skipped), no HTTP.
- RGSS2/3 classes are compiled but out of scope (rmxp3ds D4).
