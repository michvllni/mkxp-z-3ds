/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Force-included (-include) into every CRuby translation unit of the 3DS build.
 * Declares the POSIX functions newlib/libctru lack and rb3ds_shim.c provides.
 */
#ifndef RB3DS_COMPAT_H
#define RB3DS_COMPAT_H

/*
 * Only for CRuby's core objects (Makefile XCFLAGS adds -DRUBY_EXPORT). configure's test programs
 * must not see it: autoconf declares dummy prototypes (`char clock_gettime(void);`) that clash
 * with the real declarations pulled in here.
 */
#ifdef RUBY_EXPORT

#include <sys/types.h>

/*
 * Close-on-exec: there is no exec on the 3DS, and libctru's fcntl() only handles sockets
 * (F_GETFD on a file fails with ENOTSOCK, which CRuby treats as a bug: rb_bug in
 * rb_fix_detect_o_cloexec on the first File.open). Removing the macros makes CRuby skip its
 * cloexec handling (io.c, process.c; dir.c falls back to O_CLOEXEC 0). <fcntl.h> has include
 * guards, so later includes do not bring them back.
 */
#include <fcntl.h>
#undef O_CLOEXEC
#undef FD_CLOEXEC
#undef F_GETFD
#undef F_SETFD

/* DOS-style text/binary mode switch; files are always binary on the 3DS. */
int setmode(int fd, int mode);

#endif /* RUBY_EXPORT */

#endif
