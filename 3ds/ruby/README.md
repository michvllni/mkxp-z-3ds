<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# CRuby for the Nintendo 3DS

Cross-compiles the Ruby that mkxp-z embeds (`subprojects/ruby-3.1.wrap`: Ruby 3.1.3 plus
mkxp-z's patches) as a static library for the New Nintendo 3DS (devkitARM + libctru, ARM11
MPCore, hard float). Part of the rmxp3ds project (https://github.com/michvllni/rmxp3ds), which
builds its test and benchmark apps against this output.

## Build
Needs devkitARM/libctru (`DEVKITPRO`), meson, and a host Ruby of exactly the same version as
baseruby on `PATH`. The rmxp3ds dev container provides all of them.

```sh
3ds/ruby/build.sh --jobs 4          # output in build-3ds/ruby/
3ds/ruby/build.sh --help
```

Steps: `meson subprojects download ruby-3.1` (same source and patches as the desktop build), a
private copy in `build-3ds/ruby/src` with the patches below applied, the shim library, configure
(re-run when its arguments change), make, and an ABI check of the coroutine object.

Output (`build-3ds/ruby/`): `build/libruby-static.a`, `build/ext/extinit.a`,
`build/ext/zlib/zlib.a`, `build/enc/{encinit,libenc,libtrans}.a`, `shim/librb3dsshim.a`;
headers in `build/.ext/include/arm-eabi` and `src/include`. Link them in one group with
`-lz -lctru -lm`. Ruby's stack overflow check needs the size of the stack of the thread that
calls `RUBY_INIT_STACK`: with main()'s stack, give it a large stack
(`u32 __stacksize__ = 1024 * 1024;`); with a thread you create (mkxp-z's "rgss" thread), set
`rb3ds_main_stack_size` from `3ds/ruby/shim/include/rb3ds.h` to that thread's stack size first.

## Configuration
- `--with-coroutine=arm32`, `--disable-dln --with-static-linked-ext`, extensions: zlib only
  (RGSS1 needs Marshal, which is core, and Zlib).
- `-std=gnu17` (Ruby 3.1 predates C23), `-D_GNU_SOURCE` (newlib declares e.g. `memrchr` only
  then), `TRANSIENT_HEAP_TOTAL_SIZE` 4 MiB instead of 32 MiB, `USE_THREAD_CACHE=0`.
- `ac_cv_*` overrides for what newlib declares but the 3DS lacks (fork, mmap, sigaction,
  dlopen, ...); `rb_cv_binary_elf=no` (no addr2line/dladdr).

## Patches (`patches/`, applied in order)
| Patch | Why |
|---|---|
| 0001 thread_pthread | `signal()` instead of `posix_signal()`; main-thread stack size from libctru's `__stacksize__` (otherwise no stack overflow detection); thread stacks sized and located; new threads at the creator's priority; `native_thread_yield` via `svcSleepThread(0)` (newlib's `sched_yield` is an ENOSYS stub) |
| 0002 gc | no read-barrier signal handlers without sigaction (compaction is unavailable anyway) |
| 0003 file | `sdmc:/`, `romfs:/` are absolute paths; `File.dirname` keeps the device |
| 0004 coroutine/arm32 | save d8–d15 in Fiber switches (hard-float AAPCS) |
| 0005 cont | fiber pool starts with 2 stacks instead of 8 (zeroed heap memory: ~0.65 instead of ~2.6 MB at boot) |

## Shim (`shim/`)
POSIX pieces newlib/libctru lack: `<sys/mman.h>` with anonymous `mmap` on `memalign` (Fiber
stacks), `waitpid`, `get*id`, `getrusage` (system ticks since app start), no-op `pthread_kill`,
`umask`, `setmode`; `pipe`, `popen`, `exec*`, `chown` fail with `ENOSYS`; `rb3ds_thread_created`
(gives a new thread the creator's priority: libctru creates pthreads at 0x3F, and the 3DS never
runs a lower-priority thread while a higher one is runnable; and detaches it, because libsysbase
ignores `PTHREAD_CREATE_DETACHED` and finished threads would keep their ~1 MiB stacks) and
`rb3ds_yield`.
`rb3ds_compat.h` is force-included into Ruby's core objects (not into configure tests) and
removes the close-on-exec macros: libctru's `fcntl` only handles sockets, and Ruby would call
`rb_bug` on the first `File.open`.

## Known limitations
- **Threads:** creation, `join`/`value`, `Thread.pass`, Queue, Mutex, ConditionVariable and
  `sleep` work (patch 0001 + shim: thread stacks, creator's priority, a real yield). There is no
  preemption: the timer thread's pipe cannot be created ("scheduling broken" warning), so a
  CPU-bound thread runs until it blocks, sleeps or passes. `pthread_kill` is a no-op, so a
  thread blocked in a system call with the GVL released (e.g. a socket read) cannot be
  interrupted: `Thread#kill` and VM shutdown wait for it. No thread cache (`USE_THREAD_CACHE=0`).
  mkxp-z runs Ruby single-threaded.
- **Deep C recursion:** C paths that bypass Ruby's explicit stack check (e.g. very deep nested
  `inspect`, ~2000 levels with a 1 MiB stack) overflow the stack instead of raising
  SystemStackError. Linux catches these with a guard page and SIGSEGV; the 3DS needs an
  equivalent (memory permissions + libctru exception handler).
- `GC.compact` raises NotImplementedError.
