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
  for the PICA200 on citro3d (rmxp3ds `docs/DECISIONS.md` D14). Design (see the top of
  `gl3ds.cpp`):
  - **Textures:** RGBA8, padded to powers of two, swizzled on the CPU. They move to VRAM the first
    time they are drawn into, because PICA render targets must be in VRAM; clears of textures
    still in linear memory run on the CPU. When VRAM (6 MB) is full, the least recently used
    textures move back to linear memory.
  - **Framebuffer 0** is the top screen. mkxp-z sees a 400×240 window; the physical framebuffer is
    rotated.
  - **Vertex programs** run on the CPU, followed by one pass-through PICA shader
    (`display/passthrough.v.pica`).
  - **Fragment programs:** recognised by their embedded source and mapped to TEV presets.
  - **`glClear`** draws a quad over the whole target, so the scissor applies.
  - **GPU synchronisation:** draws are queued per citro3d frame. Each texture remembers the last
    frame that used it; uploads, CPU clears and `ReadPixels` wait for the GPU only when that
    frame has not finished. Deleted objects are freed once the GPU is done with them. A full
    vertex ring or command buffer submits the frame early.

  **Works:**
  - textures, render targets, clear/`fill_rect`, `draw_text`, `get_pixel`,
    `Graphics.snap_to_bitmap`;
  - `blt` with opacity: exact on transparent and opaque destination pixels, approximated on
    semi-transparent ones (the TEV cannot divide);
  - sprites and tilemap layers (opacity, color, tone without gray), autotile animation, window
    contents, tilemap flashes, plain textures, flat and vertex colors;
  - presenting to the top screen.

  **Missing:**
  - Textures over 1024 px (stored as pieces later; the table reports 4096 so mkxp-z's tileset
    atlas fits real tilesets).
  - The gray tone of sprites and tilemaps; bush, pattern and invert of sprites.
  - Plane and transitions.
  - Hue, blur and the remaining programs (drawn as plain texture, logged once).
- **Screen resolution (rmxp3ds D15):** the game screen is rendered at 3DS height, pillarboxed.
  mkxp-z's Hires mode scales its screen buffers by `framebufferScalingFactor`. `main3ds.cpp`
  always enables it, with the default factor 240/480 = 0.5 for RGSS1's 640×480; `mkxp.json` can
  set another factor (e.g. 0.625 for 512×384). Bitmaps keep their size (hook in `bitmap.cpp`).
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
  - `src/input/input.cpp`: `Input::Controller.name` returns "Nintendo 3DS".
  - `src/display/graphics.cpp`: frames end in `gl3dsPresent()` instead of `SDL_GL_SwapWindow`.
  - `src/display/bitmap.cpp`: no high-res bitmap twins in Hires mode (`MKXPZ_HIRES_BITMAPS`).
  - `src/display/graphics.cpp` `snapToBitmap`: a plain bitmap of the game's size, scaled up from
    the screen buffer (upstream's Hires snapshot relies on bitmap twins).
  - `src/display/gl/glstate.h`: the scissor box is applied again even when unchanged. Upstream
    scales it when the Hires ping-pong framebuffer is bound, so the cached value can be wrong for
    a bitmap with the same box (`fill_rect` then fills only a quarter).
  - `src/display/gl/texpool.h`: the pool of disposed bitmaps keeps 4 MB instead of 20 MB.
  - `src/display/tilemap.cpp`: tile texture insets stay half a texel below 1:1 scaling.
  - `binding/binding-mri.cpp` `showExc`: the uncaught-error text also goes to `rgssErrorMsg`.
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
  - Message boxes (`print`, `msgbox`) are printed on the bottom-screen console and do not block. A
    blocking box needs the renderer.
  - When the game ends with an error (initialisation error, uncaught Ruby exception: hook in
    `showExc`), the message stays on the bottom screen until START is pressed. With
    `sdmc:/rmxp3ds/test/autoexit` it exits after 3 s. If the RGSS thread does not stop within
    10 s, the app shows that and ends the process without tearing down state the thread still
    uses.
- **HOME menu and sleep mode** are handled by APT on the main thread. The game is not paused: with
  no rendering there is nothing to protect. Halting the RGSS thread from an APT hook would block
  HOME/sleep until its next `Graphics.update`; the renderer will stop drawing while suspended
  instead.

## Known limitations (this stage)
- **Renderer:** see the "Missing" list above.
- No sound, no movies (skipped), no HTTP.
- RGSS2/3 classes are compiled but out of scope (rmxp3ds D4).
