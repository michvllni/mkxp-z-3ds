<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# AGENTS.md — mkxp-z-3ds

Fork of [mkxp-z](https://github.com/mkxp-z/mkxp-z) that becomes the Nintendo 3DS runtime of
[rmxp3ds](https://github.com/michvllni/rmxp3ds). Project plan, decisions, rules and status live
there (`docs/PLAN.md`, `docs/DECISIONS.md`, `AGENTS.md`, `NOTES.md`); this file covers only
what is specific to this repository.

## Layout
- Upstream mkxp-z: `src/`, `binding/`, `shader/`, `subprojects/` (meson wraps; Ruby:
  `subprojects/ruby-3.1.wrap` + `subprojects/packagefiles/ruby-3.1/`), platform dirs `linux/`,
  `macos/`, `windows/`.
- 3DS port: `3ds/`
  - `3ds/ruby/`: CRuby cross-build (`build.sh`), Ruby patches (`patches/`), POSIX shim
    (`shim/`). See `3ds/ruby/README.md`.
  - `3ds/deps/`: SDL2, SDL2_image, SDL2_ttf, uchardet, pixman as static libraries for the 3DS
    (`build.sh`, pinned release archives). See `3ds/deps/README.md`.
  - `3ds/CMakeLists.txt`, `3ds/platform/`, `3ds/display/`: the 3DS build of mkxp-z itself
    (`mkxp-z.3dsx`), with replacements for desktop-only files and the GL function table on the
    PICA200. See `3ds/README.md`.
  - Later milestones add the platform layer and the citro3d renderer here (rmxp3ds PLAN §3.1).

## Build / test
| What | Command |
|---|---|
| CRuby for 3DS | `3ds/ruby/build.sh --jobs N` → `build-3ds/ruby/` (needs `DEVKITPRO`, meson, host Ruby 3.1.3) |
| 3DS libraries | `3ds/deps/build.sh --jobs N` → `build-3ds/deps/prefix/` (needs `DEVKITPRO`, cmake, meson, ninja; downloads the pinned archives) |
| mkxp-z for 3DS | `cmake -S 3ds -B build-3ds/mkxp -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" && cmake --build build-3ds/mkxp` → `mkxp-z.3dsx` (after the two scripts above) |
| Desktop build (upstream reference, golden images) | see upstream `README.md` / `.github/workflows/autobuild.yml` |
| 3DS tests | run from rmxp3ds: `tools/bench`, `tools/sdlcheck`, the runtime engine check + `tests/smoke/azahar-smoke.sh` (headless Azahar) |

The toolchain comes from the rmxp3ds dev container (or its `tools/setup-workspace.sh`).

## Workflow
- Default branch `dev`. Branch from up-to-date `dev`, PR into `dev`, squash merge. After the
  merge, a separate rmxp3ds PR bumps its `runtime/` submodule pointer.
- Keep upstream's desktop CI (`autobuild.yml`) green; never disable or remove upstream workflows
  without the maintainer's approval. No PRs or issues on upstream mkxp-z without approval.
- License GPL-2.0-or-later; SPDX header on every new file (patch files: first line). Record any
  copied third-party code in `THIRD_PARTY.md` first.
- Ruby patches: one topic per file, a description header, generated against the meson-unpacked
  tree (`subprojects/ruby-3_1_3`), never edited in place there (the desktop build shares it).
  Changes that would require regenerating Ruby's `configure` break upstream's
  `verify-ruby-configure-scripts` job: prefer `ac_cv_*` overrides and C patches.
- No game content (game data, RTP, Essentials scripts) in this repository.

## Upstream code in the 3DS build
- The GPU is reached only through the GL function table (`src/display/gl/gl-fun.h`), which is
  `3ds/display/gl3ds.cpp` on the 3DS (rmxp3ds D14). Keep display classes and the GL wrapper layer
  upstream's; extend gl3ds instead of patching them.
- Hooks in upstream files are `#if`/`#ifdef __3DS__` blocks marked `mkxp-z-3ds`. Keep them small;
  prefer a replacement file in `3ds/platform` for files that are desktop-only anyway.
- New upstream source files must be added to `3ds/CMakeLists.txt` by hand.

## Embedding
`3ds/ruby/shim/include/rb3ds.h`: set `rb3ds_main_stack_size` before `RUBY_INIT_STACK` when Ruby
runs on a thread other than main() (the runtime's "rgss" thread).

## Pitfalls
- Link `ext/extinit.a` before `libruby-static.a`: both define `Init_ext` (libruby's `dmyext.o` is
  empty), and with the wrong order `require 'zlib'` fails.
- devkitPro's SDL2 headers rename `main` to `SDL_main`; the 3DS `main()` defines `SDL_MAIN_HANDLED`.
- PhysFS derives its base directory from argv[0]. Launchers may start the app without arguments,
  so `main3ds.cpp` substitutes a path inside the game directory.
- meson cross builds: `-Dc_args` replaces the architecture flags of devkitPro's cross file. Extra
  flags go into the cross file instead (`MESON_C_ARGS` in `3ds/deps/build.sh`).
- devkitARM GCC defaults to C23 (`-std=gnu17` needed for Ruby 3.1) and short enums.
- Ruby assembles `.S` files with `ASFLAGS` only; without `-mfpu=vfp` and the hard-float flags
  the arm32 coroutine silently loses its VFP save (`build.sh` checks this).
- libctru's `fcntl()` only handles sockets; anything relying on `F_GETFD`/`F_SETFD` on files
  fails with `ENOTSOCK`.
- The force-included `rb3ds_compat.h` must stay inert for configure tests (guarded by
  `RUBY_EXPORT`): autoconf's dummy prototypes clash with real declarations.
