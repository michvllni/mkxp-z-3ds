/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Public interface of the CRuby 3DS port for embedders (mkxp-z-3ds runtime, rmxp3ds tools).
 */
#ifndef RB3DS_H
#define RB3DS_H

#include <stddef.h>

/*
 * Stack size in bytes of the thread that calls RUBY_INIT_STACK / ruby_init. libctru offers no
 * way to query it. 0 (default): main()'s stack, libctru's __stacksize__. Set it before
 * RUBY_INIT_STACK when Ruby runs on a thread you created (e.g. mkxp-z's "rgss" thread);
 * otherwise Ruby's stack overflow check uses the wrong limit.
 */
extern size_t rb3ds_main_stack_size;

#endif
