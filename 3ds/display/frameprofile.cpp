// SPDX-License-Identifier: GPL-2.0-or-later
// frameprofile.cpp: see frameprofile.h. Ruby module RMXP3DSProfile (read).
#include "frameprofile.h"

#include "binding-util.h"
#include "gl3ds.h"

#include <3ds.h>
#include <malloc.h>

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
	/* memory now, in KiB: linear (textures, command buffers), VRAM, heap (malloc) */
	struct mallinfo mi = mallinfo();
	rb_hash_aset(h, rb_str_new_cstr("linear_free_kb"), UINT2NUM(linearSpaceFree() / 1024));
	rb_hash_aset(h, rb_str_new_cstr("vram_free_kb"), UINT2NUM(vramSpaceFree() / 1024));
	rb_hash_aset(h, rb_str_new_cstr("heap_used_kb"), UINT2NUM((unsigned)mi.uordblks / 1024));
	rb_hash_aset(h, rb_str_new_cstr("heap_free_kb"), UINT2NUM((unsigned)mi.fordblks / 1024));
	unsigned lin, vram, count, dead;
	gl3dsTextureStats(&lin, &vram, &count, &dead);
	rb_hash_aset(h, rb_str_new_cstr("tex_linear_kb"), UINT2NUM(lin));
	rb_hash_aset(h, rb_str_new_cstr("tex_vram_kb"), UINT2NUM(vram));
	rb_hash_aset(h, rb_str_new_cstr("tex_count"), UINT2NUM(count));
	rb_hash_aset(h, rb_str_new_cstr("tex_dead_kb"), UINT2NUM(dead));
	return h;
}

void rmxp3dsProfileBindingInit()
{
	VALUE mod = rb_define_module("RMXP3DSProfile");
	_rb_define_module_function(mod, "read", rmxp3dsProfileRead);
}
