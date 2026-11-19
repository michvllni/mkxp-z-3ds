// SPDX-License-Identifier: GPL-2.0-or-later
// frameprofile.h: per-frame timing of the 3DS present path (mkxp-z-3ds), read from Ruby as
// RMXP3DSProfile.read -> {"frames"=>n, "composite", "limiter", "blit", "present", "gl_draw",
// "gl_upload", "gl_xform" (ms per presented frame), "draws" (count per frame)}; averages since
// the last read, counters reset on read. Also "linear_free_kb", "vram_free_kb", "heap_used_kb",
// "heap_free_kb" (memory at the time of the read). composite, limiter, blit and present are disjoint parts
// of a presented frame. Frames skipped by the frame limiter, frozen frames and repaintWait are
// not counted (frame skip is off in rmxp3ds). Cheap: a few svcGetSystemTick calls per frame.
#ifndef RMXP3DS_FRAMEPROFILE_H
#define RMXP3DS_FRAMEPROFILE_H

#include <stdint.h>

/* RP_GLDRAW: time inside gl3ds draw calls (CPU vertex work, command building) at any time:
 * composite, blit, but also bitmap drawing during the Ruby update (blt, fill_rect, draw_text,
 * transitions). RP_GLUPLOAD: texture uploads (any time). */
enum Rmxp3dsProfileSlot { RP_COMPOSITE = 0, RP_LIMITER, RP_BLIT, RP_PRESENT, RP_GLDRAW, RP_GLUPLOAD, RP_GLXFORM, RP_GLDRAWS, RP_COUNT };
/* RP_GLXFORM: CPU vertex transform inside draws; RP_GLDRAWS counts draws that reached the
 * vertex transform (not those skipped for a missing texture or a feedback loop). */

/* Adds ticks to a slot; rmxp3dsProfileFrame() counts one presented frame. */
void rmxp3dsProfileAdd(Rmxp3dsProfileSlot slot, uint64_t ticks);
void rmxp3dsProfileFrame();
uint64_t rmxp3dsProfileTicks();

/* Scoped timer for one slot. */
struct Rmxp3dsProfileScope
{
	Rmxp3dsProfileSlot slot;
	uint64_t start;
	explicit Rmxp3dsProfileScope(Rmxp3dsProfileSlot s) : slot(s), start(rmxp3dsProfileTicks()) {}
	~Rmxp3dsProfileScope() { rmxp3dsProfileAdd(slot, rmxp3dsProfileTicks() - start); }
};

#endif // RMXP3DS_FRAMEPROFILE_H
