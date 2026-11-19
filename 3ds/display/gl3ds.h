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
// Texture memory known to gl3ds, for the frame profile (KiB, count).
void gl3dsTextureStats(unsigned *linearKb, unsigned *vramKb, unsigned *count, unsigned *deadKb);

#endif
