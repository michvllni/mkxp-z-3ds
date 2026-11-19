// SPDX-License-Identifier: GPL-2.0-or-later
// frameprofile.cpp: see frameprofile.h. Ruby module RMXP3DSProfile (read).
#include "frameprofile.h"

#include "binding-util.h"

#include <3ds.h>

static uint64_t ticks[RP_COUNT];
static uint64_t frames;

uint64_t rmxp3dsProfileTicks() { return svcGetSystemTick(); }
void rmxp3dsProfileAdd(Rmxp3dsProfileSlot slot, uint64_t t) { ticks[slot] += t; }
void rmxp3dsProfileFrame() { ++frames; }

RB_METHOD(rmxp3dsProfileRead) {
	RB_UNUSED_PARAM;
	static const char *const names[RP_COUNT] = {"composite", "limiter", "blit", "present", "gl_draw", "gl_upload", "gl_xform", "draws"};
	VALUE h = rb_hash_new();
	rb_hash_aset(h, rb_str_new_cstr("frames"), ULL2NUM(frames));
	for (int i = 0; i < RP_COUNT; ++i) {
		double ms = frames ? (double)ticks[i] / frames / (SYSCLOCK_ARM11 / 1000.0) : 0.0;
		if (i == RP_GLDRAWS) ms = frames ? (double)ticks[i] / frames : 0.0; /* a count per frame */
		rb_hash_aset(h, rb_str_new_cstr(names[i]), rb_float_new(ms));
		ticks[i] = 0;
	}
	frames = 0;
	return h;
}

void rmxp3dsProfileBindingInit()
{
	VALUE mod = rb_define_module("RMXP3DSProfile");
	_rb_define_module_function(mod, "read", rmxp3dsProfileRead);
}
