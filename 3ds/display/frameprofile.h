// SPDX-License-Identifier: GPL-2.0-or-later
// frameprofile.h: per-frame timing of the 3DS present path (mkxp-z-3ds), read from Ruby as
// RMXP3DSProfile.read -> {"frames"=>n, "composite"=>ms, "limiter"=>ms, "blit"=>ms,
// "present"=>ms} (averages since the last read; the counters reset on read). Cheap enough to run
// always: a few svcGetSystemTick calls per frame.
#ifndef RMXP3DS_FRAMEPROFILE_H
#define RMXP3DS_FRAMEPROFILE_H

#include <stdint.h>

/* RP_GLDRAW: time inside gl3ds draw calls (CPU vertex work, command building), part of
 * RP_COMPOSITE or RP_BLIT; RP_GLUPLOAD: texture uploads (any time in the frame). */
enum Rmxp3dsProfileSlot { RP_COMPOSITE = 0, RP_LIMITER, RP_BLIT, RP_PRESENT, RP_GLDRAW, RP_GLUPLOAD, RP_GLXFORM, RP_GLDRAWS, RP_COUNT };
/* RP_GLXFORM: CPU vertex transform inside draws; RP_GLDRAWS counts draw calls (1 per call). */

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
