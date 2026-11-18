<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# SDL2 utility libraries for the Nintendo 3DS

mkxp-z uses SDL2 for much more than windowing. Bitmaps are `SDL_Surface`s, images are loaded with
SDL2_image, text is rendered with SDL2_ttf, and files go through `SDL_RWops`. On the 3DS the
runtime keeps these uses and builds the libraries without video, render, audio and joystick.
Drawing goes through citro3d, input through libctru (rmxp3ds PLAN §3.1). Part of the rmxp3ds
project (https://github.com/michvllni/rmxp3ds).

## Build
Needs devkitARM/libctru (`DEVKITPRO`), cmake and ninja. The rmxp3ds dev container provides them.

```sh
3ds/deps/build.sh --jobs 4          # output in build-3ds/deps/prefix/
3ds/deps/build.sh --help
```

Each library is downloaded once, checked against its SHA-256, configured with devkitPro's
`cmake/3DS.cmake` and installed into `build-3ds/deps/prefix` (`include/SDL2`, `lib/*.a`, CMake and
pkg-config files). A library is rebuilt when its version, `build.sh`, the compiler or newlib, the
CMake toolchain file, libctru or the portlibs archives it links (libpng, libjpeg, FreeType, zlib,
bzip2) change, or when it is missing from the prefix. A
rebuilt SDL2 clears the prefix and rebuilds the other two.

| Library | Version | Configuration | License |
|---|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | 2.32.10 | static; video, render, audio, joystick, haptic, sensor, hidapi off; threads, timers, RWops, surfaces and blits on | zlib |
| [SDL2_image](https://github.com/libsdl-org/SDL_image) | 2.8.12 | PNG via libpng 1.6.53 and JPEG via libjpeg-turbo from devkitPro's 3DS portlibs (not stb_image, which needs about compressed + 2× raw size at peak, double libpng's), plus BMP, GIF, TGA and other formats without dependencies; no AVIF, JXL, TIFF, WebP | zlib; contains nanosvg (zlib) and qoi (MIT); PNG/JPEG saving goes through libpng/libjpeg |
| [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf) | 2.24.0 | FreeType 2.14.1 from devkitPro's 3DS portlibs, no HarfBuzz | zlib |

Portlibs linked from devkitPro: FreeType (FTL or GPLv2; under the FTL, binary releases must
credit the FreeType Project), libpng (libpng license), libjpeg-turbo (IJG/BSD), bzip2 (BSD-like),
zlib (zlib). All are compatible with GPL-2.0-or-later and GPL-3.0.

The archives come from the GitHub releases of the three projects and are not stored in this
repository. The checksums are GitHub's asset digests (SDL2, SDL2_image). For SDL2_ttf, which has
no digest, the release signature by the SDL release key `1528635D8053A57F77D1E08630A59377A7763BE6`
(Sam Lantinga) was checked when pinning.

## Linking
Link `libSDL2_ttf.a libSDL2_image.a libSDL2.a`, then libpng and libjpeg (`-lpng16 -ljpeg`), then
FreeType with its static dependencies from devkitPro's 3DS pkg-config
(`pkg-config --static --libs freetype2`: `-lfreetype -lbz2 -lz -lpng16 -lm ... -lctru`). Define `SDL_MAIN_HANDLED` and call `SDL_SetMainReady()`: the
runtime has its own `main()`.

## Notes
- **Threads:** SDL's 3DS threads use libctru `threadCreate` with the creator's priority. The
  default stack is 80 KiB; `SDL_CreateThreadWithStackSize` sets any size. The Ruby ("rgss") thread
  needs about 1 MiB. SDL 2.28.1 (mkxp-z's meson wrap) capped 3DS thread stacks at 16 KiB with a 4 KiB
  default (`src/thread/n3ds/SDL_systhread.c:30-31` at `release-2.28.1`); 2.30 and later have no
  cap. Plain `SDL_CreateThread` takes its size from `SDL_HINT_THREAD_STACK_SIZE`.
- **Priorities:** an SDL thread gets its creator's priority on the default core. The 3DS kernel
  does not time-slice threads of equal priority, so the runtime sets priorities and cores itself.
- **Desktop differences:** mkxp-z's desktop build uses SDL2 2.28.1, SDL2_image 2.6.3 and SDL2_ttf
  2.0.18 (meson wraps). Text metrics and JPEG decoding can differ from desktop golden images.
- The rmxp3ds `tools/sdlcheck` app tests the build in headless Azahar and on the console.
  - PNG load: pixel-exact.
  - Alpha blit.
  - Text: measured and rendered with the bundled Liberation Sans.
  - A 1 MiB thread.
  - RWops on `sdmc:/`.
