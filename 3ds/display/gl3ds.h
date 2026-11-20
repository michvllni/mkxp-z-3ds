// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: entry points of the 3DS GL implementation (gl3ds.cpp) besides the `gl` table.
#ifndef GL3DS_H
#define GL3DS_H

// Ends the frame: shows what was drawn into framebuffer 0 on the top screen. Called by
// Graphics where the desktop build calls SDL_GL_SwapWindow (hook in src/display/graphics.cpp).
void gl3dsPresent();

// Called by the main thread after every aptMainLoop(): resumes rendering paused for HOME or sleep
// mode (gl3ds.cpp, gpuLock).
void gl3dsAptResumed();
// Called by the main thread when the system asks the app to close, before rendering resumes:
// draws, clears and presents submit nothing from then on.
void gl3dsCloseRequested();
// Called by the main thread once the RGSS thread has stopped, before gfxExit: waits up to 2 s for
// the GPU, then ends citro3d (C3D_Fini: its GPU command queue, VBlank callbacks and APT hook), so
// the next app finds the GPU as the system expects. False if the GPU did not finish in time
// (citro3d is then left as it is). Nothing renders afterwards.
bool gl3dsShutdown();
// Texture memory known to gl3ds, for the frame profile (KiB, count).
void gl3dsTextureStats(unsigned *linearKb, unsigned *vramKb, unsigned *count, unsigned *deadKb);

#endif
