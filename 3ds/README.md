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
    frame has not finished; a busy linear texture gets new memory instead of waiting. Deleted
    objects are freed once the GPU is done with them (or right away after a wait when memory is
    short). A full vertex ring or command buffer submits the frame early, without showing the
    partly drawn top screen.

  **Works:**
  - textures, render targets, clear/`fill_rect`, `draw_text`, `get_pixel`,
    `Graphics.snap_to_bitmap`;
  - `blt` with opacity: exact on transparent and opaque destination pixels, approximated on
    semi-transparent ones (the TEV cannot divide);
  - sprites, planes and tilemap layers: opacity, color, tone incl. gray, sprite bush (geometry
    split on the CPU at the bush line) and invert, plane flash. Each TEV stage clamps, unlike
    GLSL, so the color/flash mixes come before the tone (scaled by what the mixes leave), which
    gives GLSL's result. Exception: gray together with a color (or plane color and flash) and a
    tone beyond ±127 per channel exceeds the six TEV stages; the tone is then limited to ±127; autotile animation, window
    contents, tilemap flashes, plain textures, flat and vertex colors;
  - viewport and screen effects (tone incl. gray, color, flash, brightness);
  - `Graphics.transition`, plain and with a transition image (the threshold ramp is scaled in
    steps of 4 on the TEV because it cannot divide; 8-bit precision);
  - presenting to the top screen.

  **Missing:**
  - Textures over 1024 px: the GL table reports 1024, so larger bitmaps stay CPU "mega
    surfaces" (mkxp-z). Drawing one directly is not possible; mkxp-z's RGSS Tilemap rejects
    tilesets taller than about 1300 px (the rmxp3ds converter splits them).
  - Sprite patterns (mkxp-z extension).
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

- **Native Essentials map renderer** (`display/tilerenderer.{h,cpp}`, Ruby class
  `RMXP3DSTileRenderer`, rmxp3ds D17): draws Pokémon Essentials v21 maps instead of its Ruby
  `TilemapRenderer`. The rmxp3ds patch `patches/essentials-v21/tilemap_renderer.rb` (preload
  script) keeps Essentials' interface and passes the maps on screen per frame. The atlas is made
  of 1024x1024 pages with 32x32 slots: tileset tiles are copied, and RMXP block autotiles are
  composed per frame from their 16x16 pieces (`autotileRects`), so no texture exceeds 1024 px.
  Z order follows Essentials' tile sprites: scene elements at z -100 (reflective water), z 0
  (ground) and 32*k+1 (priorities), created before the character sprites. Within an element,
  layers 0, 1 and 2 draw in that order, batched per atlas page.
- **Hooks in upstream files**, all marked `mkxp-z-3ds` and inert on other platforms:
  - `src/system/system.h`: `MKXPZ_PLATFORM_3DS`.
  - `binding/binding-mri.cpp`: `System.platform` returns "Nintendo 3DS".
  - `src/display/graphics.cpp`: movie playback (Theora + OpenAL) is left out;
    `Graphics.play_movie` logs and returns, as if the movie had ended.
  - `src/input/input.cpp`: `Input::Controller.name` returns "Nintendo 3DS".
  - `src/display/graphics.cpp`: frames end in `gl3dsPresent()` instead of `SDL_GL_SwapWindow`.
  - `src/display/bitmap.cpp`: no high-res bitmap twins in Hires mode (`MKXPZ_HIRES_BITMAPS`).
  - `src/display/bitmap.cpp` `releaseResources`: a bitmap's texture is released also when
    `get_pixel`/`set_pixel` made a CPU copy (upstream released only the copy: one texture leaked
    per bitmap read with `get_pixel`).
  - `src/display/graphics.cpp` `snapToBitmap`: a plain bitmap of the game's size, scaled up from
    the screen buffer (upstream's Hires snapshot relies on bitmap twins).
  - `src/display/gl/glstate.h`: the scissor box is applied again even when unchanged. Upstream
    scales it when the Hires ping-pong framebuffer is bound, so the cached value can be wrong for
    a bitmap with the same box (`fill_rect` then fills only a quarter).
  - `src/display/gl/texpool.h`: the pool of disposed bitmaps keeps 4 MB instead of 20 MB.
  - `src/display/tilemap.cpp`: tile texture insets stay half a texel below 1:1 scaling.
  - Hires with a scaling factor below 1, which upstream does not handle:
    - `src/display/graphics.cpp` `ScreenScene`: the scene geometry stays in game coordinates
      (sprite and viewport culling, plane size); the ping-pong buffer size drives the GL
      viewport and blits. Upstream uses the buffer size for both, which culls everything beyond
      320×240 at factor 0.5.
    - `src/display/gl/shader.cpp`: `GrayShader` takes the scaled projection (its screen quad is
      in game coordinates now).
    - `src/display/gl/gl-meta.cpp`: shader blits (no native blit on the 3DS) project to the
      target's pixel size; upstream applies the Hires projection when the target is a ping-pong
      buffer, which shrinks the copy.
  - `src/display/plane.cpp`: the tiled path (no `npot_repeat` on the 3DS) places tiles at the
    viewport's position and scrolls with its origin, as the `npot_repeat` path does.
  - `binding/binding-mri.cpp` `showExc`: the uncaught-error text also goes to `rgssErrorMsg`.
  - `binding/binding-mri.cpp`: registers `RMXP3DSTileRenderer` (RGSS1 only) and `RMXP3DSProfile`.
  - `src/display/graphics.cpp`: frame profile scopes around `composite`, the frame limiter, the
    blit and `gl3dsPresent` (`display/frameprofile.{h,cpp}`; `RMXP3DSProfile.read` returns the
    per-frame averages in ms, plus gl3ds draw, vertex transform and upload time and the draw count).
  - `src/util/encoding.h`: well-formed UTF-8 text without NUL bytes (configs, `Game.ini` title,
    Ruby `String#to_utf8`) is taken as is
    instead of guessing its charset, and an unknown guessed charset throws instead of crashing.
    uchardet guesses mostly ASCII text with a few UTF-8 characters wrongly (Essentials'
    `mkxp.json`: `MAC-CENTRALEUROPE`, a name libiconv doesn't know; `iconv()` then wrote
    through the failed handle).
- **MIDI:** fluidsynth is loaded at run time as on desktop. That fails on the 3DS, so MIDI is
  disabled.

## Running
- **Game directory:** `argv[1]` when given (3dslink, launcher arguments), otherwise the game
  picker (`platform/gamepicker.cpp`) chooses it: every `sdmc:/rmxp3ds/games/<name>/` with a
  `Game.ini` or `mkxp.json` (packages from rmxp3ds' converter) plus `sdmc:/rmxp3ds/game` if it
  has one of them (kits and tests). One game starts directly; several are listed on the bottom screen
  (D-pad selects, A starts, START exits; the last choice is kept in
  `sdmc:/rmxp3ds/last-game.txt`). `mkxp.json` and `Game.ini` are read from the game directory,
  as upstream does. For
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
- **HOME menu and sleep mode** are handled by APT on the main thread. citro3d's own APT hook
  waits for and clears the GPU queue there, while the RGSS thread may be drawing. gl3ds therefore
  pauses rendering at a frame boundary: the RGSS thread holds a lock while a citro3d frame is
  open, our APT hook takes it on suspend and sleep (before citro3d's hook runs), and the main loop
  releases it after `aptMainLoop()` returns. Ruby keeps running until its next draw. The wait
  for the lock lasts at most until the current frame ends.

## Known limitations (this stage)
- **Renderer:** see the "Missing" list above.
- No sound, no movies (skipped), no HTTP.
- RGSS2/3 classes are compiled but out of scope (rmxp3ds D4).
