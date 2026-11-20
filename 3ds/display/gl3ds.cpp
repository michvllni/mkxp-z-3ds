// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: the GL function table (`gl`, src/display/gl/gl-fun.h) for the Nintendo 3DS on
// citro3d; replaces src/display/gl/gl-fun.cpp. mkxp-z's display classes draw only through this
// table (via its wrapper layer: TEX/FBO, buffers, Quad, ShaderSet, GLState), so the 3DS renderer
// is an implementation of the GL ES 2 subset mkxp-z uses (rmxp3ds docs/DECISIONS.md D14).
//
// Design:
// - Textures: RGBA8 C3D_Tex, padded to powers of two (min 8 px), swizzled on the CPU. The PICA
//   keeps the top row (t = 1, framebuffer top) first in memory: GL row r (t = r / height, row 0 =
//   framebuffer bottom) is memory row ph - 1 - r, for uploads, read-back, sampling and rendering
//   alike. Textures larger than 1024 px are not stored yet (logged; draws using them are skipped).
//   A texture moves from linear memory to VRAM when it is first drawn into (PICA render targets
//   must be in VRAM: citro3d renderqueue.c, C3D_RenderTargetCreateFromTex). Clears of textures
//   still in linear memory are done on the CPU, so bitmaps that are only filled stay there. When
//   VRAM is full, the least recently used VRAM textures move back to linear memory.
// - Framebuffers: a texture's render target is created when it is first drawn to. Framebuffer 0
//   is the top screen (400x240 for mkxp-z; the physical framebuffer is rotated).
// - Programs: mkxp-z's programs are recognised by their embedded source (the pointers passed to
//   ShaderSource). Vertex programs run on the CPU at draw time (projection, sprite matrix,
//   translation, texture coordinates incl. padding, tilemap autotile animation); one pass-through
//   PICA shader (passthrough.v.pica) draws the result. Fragment programs are TEV presets.
// - Clear honours the scissor box (mkxp-z's fill_rect is scissor + clear): it draws a quad.
// - GPU synchronisation: work is queued per citro3d frame and runs at C3D_FrameEnd. Every texture
//   records the serial of the last frame that used it; CPU access to a texture (upload, clear,
//   ReadPixels) waits for the GPU only if that frame has not finished yet.
// - Deletions are deferred until the GPU has finished every frame that may use the objects.
#include "gl-fun.h"
#include "gl3ds.h"
#include "frameprofile.h"

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "passthrough.shbin.xxd"
#include "util/exception.h"

// Embedded GLSL sources of mkxp-z (generated in the build, src/display/gl/shader.cpp includes
// the same arrays): their addresses identify the programs.
#define SHADER_SRC(name) extern const uint8_t mkxp_shader_##name[];
SHADER_SRC(simple_vert)
SHADER_SRC(simpleColor_vert)
SHADER_SRC(sprite_vert)
SHADER_SRC(minimal_vert)
SHADER_SRC(tilemap_vert)
SHADER_SRC(simpleMatrix_vert)
SHADER_SRC(simple_frag)
SHADER_SRC(simpleColor_frag)
SHADER_SRC(simpleAlpha_frag)
SHADER_SRC(simpleAlphaUni_frag)
SHADER_SRC(flatColor_frag)
SHADER_SRC(sprite_frag)
SHADER_SRC(bitmapBlit_frag)
SHADER_SRC(plane_frag)
SHADER_SRC(transSimple_frag)
SHADER_SRC(tilemap_frag)
SHADER_SRC(flashMap_frag)
SHADER_SRC(trans_frag)
SHADER_SRC(gray_frag)
#undef SHADER_SRC

GLFunctions gl;

namespace {

// ---- conventions to verify on the GPU (see the engine/render checks in rmxp3ds) --------------
// Top screen: mkxp-z draws a 400x240 window; the framebuffer is 240x400 (rotated). A window-space
// clip position (x, y) becomes (SCREEN_XX * y, SCREEN_YX * x) in framebuffer clip space: window y
// runs along framebuffer x, window x against framebuffer y (verified with the render check).
const float SCREEN_XX = 1.0f;
const float SCREEN_YX = -1.0f;

const int MAX_TEX = 1024; // PICA200
const size_t VERTEX_RING_BYTES = 1024 * 1024;
const size_t CMDBUF_BYTES = 512 * 1024;
// Command words one draw call may add at most (framebuffer, TEV, blend, textures, attributes:
// a few hundred bytes); the frame is submitted before the buffer gets this close to full
// (libctru's GPUCMD_Add breaks into the debugger on overflow).
const u32 CMDBUF_RESERVE_WORDS = 1024;

struct Vtx {
	float pos[4];
	float tc[2];
	float color[4];
	float tc1[2];
	float tc2[2];
};
const size_t RING_VERTICES = VERTEX_RING_BYTES / sizeof(Vtx) / 3 * 3; // whole triangles

struct Tex {
	GLsizei w = 0, h = 0;
	u16 pw = 0, ph = 0;
	bool allocated = false;
	C3D_Tex tex;
	C3D_RenderTarget *target = nullptr;
	bool linear = false;
	bool repeat = false;
	bool inVram = false; // moved to VRAM when first rendered into (PICA render targets must be in VRAM)
	u32 useSerial = 0;   // last frame that sampled or rendered into it (see State::frameSerial)
};

struct Buffer {
	std::vector<uint8_t> data;
};

struct AttribPointer {
	bool enabled = false;
	GLint size = 0;
	GLsizei stride = 0;
	uintptr_t offset = 0;
	GLuint buffer = 0;
};

struct VertexArray {
	AttribPointer attribs[3]; // Position, TexCoord, Color (mkxp-z Shader::Attribute)
	GLuint elementBuffer = 0;
};

enum class VertKind { Unknown, Simple, SimpleColor, Sprite, Minimal, Tilemap, SimpleMatrix };
enum class FragKind { Unknown, Simple, SimpleColor, SimpleAlpha, SimpleAlphaUni, FlatColor, Sprite, Tilemap, FlashMap, Blit, Plane, TransSimple, Trans, Gray };

struct ShaderObj {
	GLenum type = 0;
	const void *src = nullptr; // last source string: mkxp-z's program-specific part
};

struct Program {
	std::vector<GLuint> shaders;
	VertKind vert = VertKind::Unknown;
	FragKind frag = FragKind::Unknown;
	std::map<std::string, GLint> locations;
	std::vector<std::array<float, 16>> values; // per location
	std::array<float, 16> &value(GLint loc) {
		if (loc >= (GLint)values.size()) values.resize(loc + 1, std::array<float, 16>{});
		return values[loc];
	}
	const float *get(const char *name) const {
		auto it = locations.find(name);
		if (it == locations.end() || it->second >= (GLint)values.size()) return nullptr;
		return values[it->second].data();
	}
};

struct State {
	bool c3dReady = false;
	bool inFrame = false;
	// Serial of the frame being built (or the next one); every frame up to completedSerial has
	// finished on the GPU.
	u32 frameSerial = 1;
	u32 completedSerial = 0;
	// GX queue entries used by the frame being built: citro3d's queue holds 32 (base.c
	// gxQueue.maxEntries) until C3D_FrameBegin empties it; libctru panics when it is full
	// (gxqueue.c). An in-frame C3D_SyncTextureCopy adds two (command list split + copy).
	int gxEntries = 0;
	C3D_RenderTarget *top = nullptr;
	shaderProgram_s shader;
	DVLB_s *dvlb = nullptr;
	C3D_AttrInfo attrInfo;
	Vtx *ring = nullptr;
	C3D_Tex dummyTex; // bound to unused texture units 1 and 2 (C3D_TexBind rejects NULL there)
	size_t ringUsed = 0; // in vertices

	GLuint nextName = 1;
	std::unordered_map<GLuint, Tex> textures;
	std::unordered_map<GLuint, Buffer> buffers;
	std::unordered_map<GLuint, GLuint> framebuffers; // fbo -> texture
	std::unordered_map<GLuint, VertexArray> vertexArrays;
	std::unordered_map<GLuint, ShaderObj> shaders;
	std::unordered_map<GLuint, Program> programs;
	VertexArray defaultVertexArray;
	GLuint boundVertexArray = 0, arrayBuffer = 0, framebuffer = 0, program = 0;
	GLuint boundTex[4] = {0, 0, 0, 0}; // units 0-3 as mkxp-z binds them; draws sample 0 (and 1 for blt)
	int activeUnit = 0;

	float clearColor[4] = {0, 0, 0, 0};
	bool blend = false, scissorTest = false;
	GLenum blendEq = GL_FUNC_ADD;
	GLenum blendSrcRgb = GL_ONE, blendDstRgb = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO;
	GLint scissor[4] = {0, 0, 0, 0};
	GLint viewport[4] = {0, 0, 400, 240};
	GLint unpackRowLength = 0, unpackSkipRows = 0, unpackSkipPixels = 0;

	// deferred deletions: released objects that a submitted or queued frame may still use
	std::vector<C3D_Tex> deadTextures;
	std::vector<C3D_RenderTarget *> deadTargets;

	std::map<std::string, bool> warned;
} st;

void warnOnce(const std::string &what) {
	if (st.warned[what]) return;
	st.warned[what] = true;
	fprintf(stderr, "gl3ds: %s\n", what.c_str());
}

u16 pow2(GLsizei v) {
	u16 p = 8;
	while (p < v) p <<= 1;
	return p;
}

// Byte offset of texel (x, y) in a tiled RGBA8 PICA texture of width pw: 8x8 tiles in rows,
// Morton (Z) order inside a tile.
inline size_t tiledOffset(u32 x, u32 y, u32 pw) {
	u32 tile = (y >> 3) * (pw >> 3) + (x >> 3);
	u32 tx = x & 7, ty = y & 7;
	u32 morton = (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3);
	return ((size_t)tile * 64 + morton) * 4;
}

// ---- frames and render targets -----------------------------------------------------------------

// HOME and sleep mode. citro3d's APT hook runs on the main thread (aptMainLoop) and, on suspend,
// waits for the GX queue and clears it (base.c C3Di_AptEventHook -> C3Di_WaitAndClearQueue). The
// RGSS thread draws at the same time: while a citro3d frame is open the queue is stopped, so the
// wait returns at once (libctru gxqueue.c gxCmdQueueWait), the queue is cleared under the RGSS
// thread, and it keeps submitting GPU work after the app gave up the GPU.
// gpuLock is held by the RGSS thread while a frame is open (beginFrame..endFrame) and while it
// waits for GPU work outside a frame. Our APT hook takes it on suspend and sleep, so rendering
// pauses at a frame boundary before citro3d's hook runs (libctru calls the newest hook first;
// ours is registered after C3D_Init). The main loop releases it once aptMainLoop returns, after
// citro3d's restore hook (gl3dsAptResumed).
RecursiveLock gpuLock;
aptHookCookie aptCookie;
bool aptHeld = false; // main thread only

struct GpuScope {
	GpuScope() { RecursiveLock_Lock(&gpuLock); }
	~GpuScope() { RecursiveLock_Unlock(&gpuLock); }
};

void onAptEvent(APT_HookType type, void *) {
	if ((type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP) && !aptHeld) {
		RecursiveLock_Lock(&gpuLock);
		aptHeld = true;
	}
}

void initC3D() {
	if (st.c3dReady) return;
	RecursiveLock_Init(&gpuLock);
	if (!C3D_Init(CMDBUF_BYTES)) throw Exception(Exception::MKXPError, "gl3ds: C3D_Init failed (out of linear memory)");
	aptHook(&aptCookie, onAptEvent, nullptr); // after C3D_Init: runs before citro3d's hook
	st.top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
	if (!st.top) throw Exception(Exception::MKXPError, "gl3ds: cannot create the top screen render target (out of VRAM)");
	C3D_RenderTargetSetOutput(st.top, GFX_TOP, GFX_LEFT,
	                          GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
	// DVLB_ParseFile reads u32 words and keeps pointers into the data: a 4-byte aligned copy that
	// lives as long as the program (the embedded array is a byte array without alignment).
	u32 *shbin = (u32 *)memalign(4, mkxp_shader_passthrough_shbin_len);
	if (!shbin) throw Exception(Exception::MKXPError, "gl3ds: out of memory");
	memcpy(shbin, mkxp_shader_passthrough_shbin, mkxp_shader_passthrough_shbin_len);
	st.dvlb = DVLB_ParseFile(shbin, mkxp_shader_passthrough_shbin_len);
	if (!st.dvlb) throw Exception(Exception::MKXPError, "gl3ds: cannot parse the PICA shader");
	shaderProgramInit(&st.shader);
	shaderProgramSetVsh(&st.shader, &st.dvlb->DVLE[0]);
	AttrInfo_Init(&st.attrInfo);
	AttrInfo_AddLoader(&st.attrInfo, 0, GPU_FLOAT, 4);
	AttrInfo_AddLoader(&st.attrInfo, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&st.attrInfo, 2, GPU_FLOAT, 4);
	AttrInfo_AddLoader(&st.attrInfo, 3, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&st.attrInfo, 4, GPU_FLOAT, 2);
	st.ring = (Vtx *)linearAlloc(VERTEX_RING_BYTES);
	if (!st.ring) throw Exception(Exception::MKXPError, "gl3ds: out of linear memory for the vertex buffer");
	if (!C3D_TexInit(&st.dummyTex, 8, 8, GPU_RGBA8)) throw Exception(Exception::MKXPError, "gl3ds: out of linear memory");
	memset(st.dummyTex.data, 0, st.dummyTex.size);
	C3D_TexFlush(&st.dummyTex);
	st.c3dReady = true;
}

void freeDeferred() {
	for (C3D_RenderTarget *t : st.deadTargets) C3D_RenderTargetDelete(t);
	for (C3D_Tex &t : st.deadTextures) C3D_TexDelete(&t);
	st.deadTargets.clear();
	st.deadTextures.clear();
}

// Submits the frame being built (C3D_FrameEnd only queues it; the GPU runs it asynchronously).
// C3D_FrameEnd also copies the top screen target to the display if it was drawn into: only
// gl3dsPresent submits with present; early submits (full buffers, syncs) skip that copy, the
// content stays in the target until the frame is presented.
void endFrame(bool present = false) {
	if (!st.inFrame) return;
	if (!present && st.top) st.top->used = false;
	C3D_FrameEnd(0); // also flushes the CPU data cache of the linear heap (vertices, textures)
	st.inFrame = false;
	st.frameSerial++;
	RecursiveLock_Unlock(&gpuLock); // taken in beginFrame
}

// Waits until the GPU has finished every submitted frame (outside a frame), then frees the
// deferred objects. C3D_FrameBegin without C3D_FRAME_NONBLOCK waits for the GPU queue; the
// empty frame submits nothing (GX_CMDLIST_FLUSH skips the linear heap cache flush).
void waitGpu() {
	initC3D();
	GpuScope lock;
	C3D_FrameBegin(0);
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	st.completedSerial = st.frameSerial - 1;
	freeDeferred(); // outside a frame: C3D_RenderTargetDelete panics inside one
}

// Submits the queued work and waits for all of it.
void syncGpu() {
	endFrame();
	waitGpu();
}

// Before the CPU touches a texture's memory: waits if a frame that used it may still be running.
void syncIfUsed(const Tex &t) {
	if (t.useSerial > st.completedSerial) syncGpu();
}

void beginFrame() {
	initC3D();
	if (st.inFrame) return;
	RecursiveLock_Lock(&gpuLock); // held until endFrame (see gpuLock)
	// C3D_FrameBegin waits for the previous frame anyway: free what it may have used first
	// (render targets can only be deleted outside a frame).
	if (!st.deadTargets.empty() || !st.deadTextures.empty()) waitGpu();
	C3D_FrameBegin(0);
	st.completedSerial = st.frameSerial - 1;
	st.inFrame = true;
	st.ringUsed = 0;
	st.gxEntries = 0;
}

Tex *texture(GLuint name) {
	auto it = st.textures.find(name);
	return it == st.textures.end() ? nullptr : &it->second;
}

// Allocates a linear-memory texture. If memory is short while deferred deletions or unfinished
// frames hold memory, waits for the GPU (which frees them) and tries again.
bool allocLinear(C3D_Tex *out, u16 pw, u16 ph) {
	if (C3D_TexInit(out, pw, ph, GPU_RGBA8)) return true;
	if (st.deadTextures.empty() && st.deadTargets.empty()) return false;
	syncGpu();
	return C3D_TexInit(out, pw, ph, GPU_RGBA8);
}

// Before the CPU writes a texture: if a frame that used it may still be running, a linear texture
// gets new memory (the GPU never writes linear textures; the old memory is freed once the GPU is
// done, keepContent copies it first), so the CPU does not wait. VRAM textures (render targets)
// wait for the GPU.
void prepareCpuWrite(Tex &t, bool keepContent) {
	if (t.useSerial <= st.completedSerial) return;
	if (!t.inVram) {
		C3D_Tex fresh;
		if (C3D_TexInit(&fresh, t.pw, t.ph, GPU_RGBA8)) {
			if (keepContent) memcpy(fresh.data, t.tex.data, t.tex.size);
			st.deadTextures.push_back(t.tex);
			t.tex = fresh;
			t.useSerial = 0;
			return;
		}
	}
	syncGpu();
}

// Moves a VRAM texture back to linear memory (GPU idle, outside a frame). False if linear memory
// is full.
bool evictToLinear(Tex &t) {
	GpuScope lock;
	C3D_Tex lin;
	if (!allocLinear(&lin, t.pw, t.ph)) return false;
	// The CPU cannot access VRAM on the console (citro3d texture.c: "CPU can't write to VRAM";
	// a CPU write is a permission fault, Azahar allows it): copy with the GX engine. Called
	// outside a frame. No dirty cache lines may be written back over the copy (flush first), and
	// C3D_SyncTextureCopy's gspWaitForPPF can return early (the PPF event stays latched from
	// earlier transfers), so wait for the GX queue explicitly before using or freeing anything.
	GSPGPU_FlushDataCache(lin.data, lin.size);
	C3D_SyncTextureCopy((u32 *)t.tex.data, 0, (u32 *)lin.data, 0, t.tex.size, GX_TRANSFER_RAW_COPY(1));
	waitGpu();
	GSPGPU_InvalidateDataCache(lin.data, lin.size); // written by the GPU
	if (t.target) C3D_RenderTargetDelete(t.target);
	t.target = nullptr;
	C3D_TexDelete(&t.tex);
	t.tex = lin;
	t.inVram = false;
	return true;
}

// Allocates VRAM for `need`; when VRAM is full, moves the least recently used VRAM textures back
// to linear memory until it fits. May submit and wait for the queued frame (leaves no frame open).
bool allocVram(Tex &need, C3D_Tex *out) {
	if (C3D_TexInitVRAM(out, need.pw, need.ph, GPU_RGBA8)) return true;
	syncGpu(); // frees deferred VRAM; queued draws may still use the textures that get evicted
	if (C3D_TexInitVRAM(out, need.pw, need.ph, GPU_RGBA8)) return true;
	// Give up early if evicting every other VRAM texture could not make enough room.
	size_t reclaimable = vramSpaceFree();
	for (auto &kv : st.textures)
		if (kv.second.allocated && kv.second.inVram && &kv.second != &need) reclaimable += kv.second.tex.size;
	if (reclaimable < (size_t)need.pw * need.ph * 4) return false;
	for (;;) {
		Tex *victim = nullptr;
		for (auto &kv : st.textures) {
			Tex &c = kv.second;
			if (c.allocated && c.inVram && &c != &need && (!victim || c.useSerial < victim->useSerial)) victim = &c;
		}
		if (!victim || !evictToLinear(*victim)) return false;
		if (C3D_TexInitVRAM(out, need.pw, need.ph, GPU_RGBA8)) return true;
	}
}

// Before the CPU reads or writes a texture's memory: a texture in VRAM (a render target) moves
// back to linear memory first, the CPU cannot access VRAM on the console. A later draw into it
// moves it to VRAM again. False if linear memory is full.
bool makeCpuAccessible(Tex &t) {
	if (!t.inVram) return true;
	syncGpu(); // draws into it must be done; eviction needs the GPU idle
	if (evictToLinear(t)) {
		t.useSerial = 0;
		return true;
	}
	warnOnce("out of linear memory: texture in VRAM not accessible");
	return false;
}

// Texture bound to the current framebuffer (nullptr for the screen or an incomplete framebuffer).
Tex *framebufferTexture() {
	if (st.framebuffer == 0) return nullptr;
	auto it = st.framebuffers.find(st.framebuffer);
	if (it == st.framebuffers.end()) return nullptr;
	Tex *t = texture(it->second);
	return t && t->allocated ? t : nullptr;
}

// Render target of the bound framebuffer (nullptr: nothing to draw into). Sets *screen for the
// top screen. May end the frame being built (VRAM eviction): call before beginFrame().
C3D_RenderTarget *currentTarget(bool *screen, Tex **texOut) {
	*screen = st.framebuffer == 0;
	*texOut = nullptr;
	if (*screen) return st.top;
	Tex *t = framebufferTexture();
	if (!t) return nullptr;
	if (!t->inVram) {
		// Move the texture to VRAM. The GPU never writes a linear texture, so the CPU copy is
		// current; queued draws that sample the old copy keep it until it is freed (deferred).
		C3D_Tex vram;
		if (!allocVram(*t, &vram)) {
			warnOnce("out of VRAM for render targets (" + std::to_string(t->pw) + "x" + std::to_string(t->ph) + ")");
			return nullptr;
		}
		// GX copy (the CPU cannot write VRAM), queued in the frame before the draws that follow
		C3D_TexFlush(&t->tex);
		beginFrame();
		if (st.gxEntries + 2 > 28) { // keep room for the final split and the screen transfer
			endFrame();
			beginFrame();
		}
		st.gxEntries += 2;
		C3D_SyncTextureCopy((u32 *)t->tex.data, 0, (u32 *)vram.data, 0, t->tex.size, GX_TRANSFER_RAW_COPY(1));
		st.deadTextures.push_back(t->tex); // freed once the GPU is done (the copy reads it)
		t->tex = vram;
		t->inVram = true;
	}
	if (!t->target) t->target = C3D_RenderTargetCreateFromTex(&t->tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
	if (!t->target) warnOnce("C3D_RenderTargetCreateFromTex failed");
	*texOut = t;
	return t->target;
}

// Clears a texture that is still in linear memory on the CPU (scissor box if enabled), so that
// creating and filling a bitmap does not move it to VRAM.
void cpuClear(Tex &t, const float *color) {
	if (t.inVram) { // never: clear() draws a quad for VRAM textures
		warnOnce("BUG: CPU clear of a VRAM texture skipped");
		return;
	}
	GLint x0 = 0, y0 = 0, x1 = t.w, y1 = t.h;
	if (st.scissorTest) {
		x0 = std::max(x0, st.scissor[0]);
		y0 = std::max(y0, st.scissor[1]);
		x1 = std::min(x1, st.scissor[0] + st.scissor[2]);
		y1 = std::min(y1, st.scissor[1] + st.scissor[3]);
	}
	if (x0 >= x1 || y0 >= y1) return;
	prepareCpuWrite(t, !(x0 == 0 && y0 == 0 && x1 == t.w && y1 == t.h));
	auto b = [](float v) { return (uint8_t)(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
	const uint8_t px[4] = {b(color[3]), b(color[2]), b(color[1]), b(color[0])}; // A B G R
	uint8_t *dst = (uint8_t *)t.tex.data;
	if (x0 == 0 && y0 == 0 && x1 == t.w && y1 == t.h && px[0] == px[1] && px[1] == px[2] && px[2] == px[3]) {
		memset(dst, px[0], t.tex.size); // whole texture incl. padding
	} else {
		for (GLint y = y0; y < y1; y++)
			for (GLint x = x0; x < x1; x++) memcpy(dst + tiledOffset(x, t.ph - 1 - y, t.pw), px, 4);
	}
	C3D_TexFlush(&t.tex);
}

GPU_BLENDFACTOR blendFactor(GLenum f) {
	switch (f) {
	case GL_ZERO: return GPU_ZERO;
	case GL_ONE: return GPU_ONE;
	case GL_SRC_COLOR: return GPU_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR: return GPU_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA: return GPU_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
	default: warnOnce("unsupported blend factor " + std::to_string(f)); return GPU_ONE;
	}
}

GPU_BLENDEQUATION toBlendEquation(GLenum e) {
	switch (e) {
	case GL_FUNC_SUBTRACT: return GPU_BLEND_SUBTRACT;
	case GL_FUNC_REVERSE_SUBTRACT: return GPU_BLEND_REVERSE_SUBTRACT;
	default: return GPU_BLEND_ADD;
	}
}

// Viewport and scissor of the current target in PICA framebuffer coordinates (bottom-left
// origin like GL for texture targets; the top screen is rotated). fullTarget: the viewport covers
// the whole framebuffer (glClear ignores the viewport). Viewport first: citro3d's
// C3D_SetViewport also resets the scissor.
// Returns false when the scissor box is empty (nothing may be drawn): citro3d writes right-1 and
// bottom-1, so an empty box at the origin would wrap to the whole target.
bool applyViewportScissor(bool screen, bool scissorOn, const Tex *target, bool fullTarget) {
	const GLint *v = st.viewport;
	if (fullTarget) {
		if (screen)
			C3D_SetViewport(0, 0, 240, 400);
		else
			C3D_SetViewport(0, 0, target->pw, target->ph);
	} else if (screen) {
		// rotated, see SCREEN_XX/SCREEN_YX
		C3D_SetViewport(v[1], 400 - (v[0] + v[2]), v[3], v[2]);
	} else {
		C3D_SetViewport(v[0], v[1], v[2], v[3]);
	}
	if (!scissorOn) {
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		return true;
	}
	// box clamped to the target (the PICA takes unsigned coordinates)
	const GLint tw = screen ? 400 : target->pw, th = screen ? 240 : target->ph;
	const GLint x0 = std::min(std::max(st.scissor[0], 0), tw), y0 = std::min(std::max(st.scissor[1], 0), th);
	const GLint x1 = std::min(std::max(st.scissor[0] + st.scissor[2], x0), tw), y1 = std::min(std::max(st.scissor[1] + st.scissor[3], y0), th);
	if (x0 >= x1 || y0 >= y1) return false;
	if (screen)
		C3D_SetScissor(GPU_SCISSOR_NORMAL, y0, 400 - x1, y1, 400 - x0);
	else
		C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1);
	return true;
}

u32 packColor(const float *c) {
	auto b = [](float v) { return (u32)(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
	return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (b(c[3]) << 24);
}

// GL texture units a fragment program samples, in PICA unit order (PICA unit i uses texture
// coordinate i).
struct Units {
	int n = 0;
	int gl[3] = {0, 0, 0};
};

Units unitsFor(const Program *prog) {
	Units u;
	switch (prog ? prog->frag : FragKind::Unknown) {
	case FragKind::FlatColor:
	case FragKind::SimpleColor:
	case FragKind::FlashMap: break;
	case FragKind::Blit: u.n = 2, u.gl[0] = 0, u.gl[1] = 1; break;                       // source, destination copy
	case FragKind::TransSimple: u.n = 2, u.gl[0] = 1, u.gl[1] = 2; break;                // current, frozen scene
	case FragKind::Trans: u.n = 3, u.gl[0] = 1, u.gl[1] = 2, u.gl[2] = 3; break;         // + transition map
	default: u.n = 1; break;
	}
	return u;
}

float uniform1(const Program *prog, const char *name, float def) {
	const float *v = prog->get(name);
	return v ? v[0] : def;
}

// TEV stages for gray, tone, opacity, color, flash and invert on texture unit 0 (sprite.frag,
// plane.frag, tilemap.frag, gray.frag). GLSL clamps only at the end, each TEV stage clamps, so
// the order is rearranged to keep intermediate values in [0, 1]:
//   GLSL:  y = mix(mix(gray(x) + t, C, c.a), F, f.a); invert: 1 - y
//   here:  y = mix(mix(gray(x), C, c.a), F, f.a) + t * (1 - c.a) * (1 - f.a)   (same value)
// gray(x) = x * (1 - g) + g * (0.299 r + 0.587 g + 0.114 b) (4 stages, luma via per-channel
// multiply-add). The tone is one ADD_SIGNED stage when every channel is within ±0.5 after
// scaling, else an add and a subtract stage (a channel's tone has one sign, so those clamps are
// exact too). Invert is folded into the tone stage (1 - (y + t) = (1 - y) - t; clamping commutes
// with 1 - v). Alpha: tex.a * opacity (stage 0's constant: the combiner buffer cannot be used
// there, stage 0 reads it as 0). Stages that would do nothing are left out; the PICA has six.
void effectChain(float gray, const float *tone, float opacity, const float *color, const float *flash, bool invert) {
	const float *mixes[2] = {color && color[3] > 0 ? color : nullptr, flash && flash[3] > 0 ? flash : nullptr};
	float keep = 1.0f; // share of the pre-mix value left after the mixes
	for (const float *m : mixes)
		if (m) keep *= 1.0f - m[3];
	float t[3] = {0, 0, 0};
	bool hasTone = false, small = true;
	for (int i = 0; i < 3 && tone; i++) {
		t[i] = tone[i] * keep;
		hasTone |= t[i] != 0;
		small &= std::fabs(t[i]) <= 0.5f;
	}
	int s = 0;
	auto stage = [&]() -> C3D_TexEnv * {
		if (s < 6) return C3D_GetTexEnv(s++);
		warnOnce("sprite effects need more than six TEV stages: some are left out");
		return nullptr;
	};
	C3D_TexEnv *e = stage();
	if (gray > 0) {
		float c[4] = {1 - gray, 1 - gray, 1 - gray, opacity};
		C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
		C3D_TexEnvColor(e, packColor(c));
		static const float lumaF[3] = {0.299f, 0.587f, 0.114f};
		static const GPU_TEVOP_RGB channel[3] = {GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B};
		for (int i = 0; i < 3; i++) {
			if (!(e = stage())) return;
			float w = lumaF[i] * gray, cw[4] = {w, w, w, 0};
			C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
			C3D_TexEnvOpRgb(e, channel[i], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
			C3D_TexEnvColor(e, packColor(cw));
		}
	} else {
		float c[4] = {0, 0, 0, opacity};
		C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e, packColor(c));
	}
	for (int i = 0; i < 2; i++) {
		if (!mixes[i]) continue;
		if (!(e = stage())) return;
		C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
		C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvColor(e, packColor(mixes[i]));
	}
	const GPU_TEVOP_RGB prevOp = invert ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR;
	if (hasTone && !small && s + 2 > 6) {
		// no room for the add and the subtract stage: one ADD_SIGNED stage with the tone clamped
		// to +-0.5 keeps both signs (error only beyond +-0.5)
		warnOnce("sprite effects: tone limited to +-0.5 (gray + color/flash use the other TEV stages)");
		for (float &v : t) v = std::min(std::max(v, -127.0f / 255.0f), 127.0f / 255.0f);
		small = true;
	}
	if (hasTone && small) {
		// ADD_SIGNED: prev + c - 0.5 with c = 0.5 + t (inverted: (1 - prev) + 0.5 - t)
		if (!(e = stage())) return;
		float c[4];
		for (int i = 0; i < 3; i++) c[i] = (128.0f + (invert ? -t[i] : t[i]) * 255.0f) / 255.0f;
		c[3] = 0;
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvOpRgb(e, prevOp, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_ADD_SIGNED);
		C3D_TexEnvColor(e, packColor(c));
	} else if (hasTone) {
		// add the positive part, subtract the negative part (inverted: 1 - prev first, then the
		// signs swap)
		float pos[4] = {0, 0, 0, 0}, neg[4] = {0, 0, 0, 0};
		for (int i = 0; i < 3; i++) {
			float v = invert ? -t[i] : t[i];
			pos[i] = std::max(v, 0.0f);
			neg[i] = std::max(-v, 0.0f);
		}
		if (!(e = stage())) return;
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvOpRgb(e, prevOp, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_ADD);
		C3D_TexEnvColor(e, packColor(pos));
		if (!(e = stage())) return;
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_SUBTRACT);
		C3D_TexEnvColor(e, packColor(neg));
	} else if (invert) {
		if (!(e = stage())) return;
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
	}
}

// trans.frag: alpha = clamp((map.r - prog) / vague, 0, 1); mix(current, frozen, alpha).
// PICA units: 0 current, 1 frozen, 2 transition map. The TEV cannot divide: map.r - prog is
// scaled by 4 per stage while the factor 1/vague is above 4, then multiplied by the rest.
void transChain(float prog, float vague) {
	int s = 0;
	C3D_TexEnv *e = C3D_GetTexEnv(s++);
	float p[4] = {prog, prog, prog, 0};
	C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE2, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_SUBTRACT);
	C3D_TexEnvColor(e, packColor(p));
	float k = vague > 0 ? 1.0f / vague : 256.0f;
	while (k > 4 && s < 4) {
		e = C3D_GetTexEnv(s++);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_4);
		k /= 4;
	}
	e = C3D_GetTexEnv(s++);
	float kc = k > 1 ? k / 4 : k, c[4] = {kc, kc, kc, 0};
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvColor(e, packColor(c));
	if (k > 1) C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_4);
	e = C3D_GetTexEnv(s++);
	C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE1, GPU_TEXTURE0, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R);
	C3D_TexEnvOpAlpha(e, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_R);
	C3D_TexEnvFunc(e, C3D_Both, GPU_INTERPOLATE);
}

// TEV stages for a fragment program (texture units as in unitsFor). alphaFactor multiplies the
// opacity (sprite bush).
void setupTexEnv(const Program *prog, float alphaFactor = 1.0f) {
	for (int i = 0; i < 6; i++) {
		C3D_TexEnv *env = C3D_GetTexEnv(i);
		C3D_TexEnvInit(env); // pass-through of the previous stage
	}
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	FragKind kind = prog ? prog->frag : FragKind::Unknown;
	switch (kind) {
	case FragKind::FlatColor: {
		const float *c = prog->get("color");
		C3D_TexEnvSrc(e0, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
		C3D_TexEnvColor(e0, c ? packColor(c) : 0);
		return;
	}
	case FragKind::SimpleColor:
		C3D_TexEnvSrc(e0, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
		return;
	case FragKind::FlashMap: {
		// flashMap.frag: vec4(v_color.rgb * alpha, 1)
		float a = uniform1(prog, "alpha", 1.0f), c[4] = {a, a, a, 1.0f};
		C3D_TexEnvSrc(e0, C3D_RGB, GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvSrc(e0, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e0, packColor(c));
		return;
	}
	case FragKind::SimpleAlpha:
		C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
		return;
	case FragKind::SimpleAlphaUni:
		effectChain(0, nullptr, uniform1(prog, "alpha", 1.0f) * alphaFactor, nullptr, nullptr, false);
		return;
	case FragKind::Sprite:
		if (uniform1(prog, "renderPattern", 0) != 0) warnOnce("sprite patterns not supported yet");
		/* fallthrough */
	case FragKind::Tilemap:
	case FragKind::Plane:
	case FragKind::Gray: {
		const float *tone = prog->get("tone");
		float gray = kind == FragKind::Gray ? uniform1(prog, "gray", 0) : tone ? tone[3] : 0;
		effectChain(gray, tone, uniform1(prog, "opacity", 1.0f) * alphaFactor, kind == FragKind::Gray ? nullptr : prog->get("color"),
		            kind == FragKind::Plane ? prog->get("flash") : nullptr, kind == FragKind::Sprite && uniform1(prog, "invert", 0) != 0);
		return;
	}
	case FragKind::Blit: {
		// bitmapBlit.frag (unit 0 source, unit 1 a copy of the destination):
		//   co1 = src.a * opacity, w = dst.a * (1 - co1), a = co1 + w,
		//   rgb = (co1 * src.rgb + w * dst.rgb) / a   (src.rgb where a == 0)
		// The TEV cannot divide; this computes rgb = mix(src.rgb, dst.rgb, w), which is exact
		// when dst.a is 0 or 1 (transparent or opaque destination pixels) and close in between.
		// Stage 0: rgb = src, a = co1 (also into the combiner buffer, read by stage 2).
		float c[4] = {0, 0, 0, uniform1(prog, "opacity", 1.0f)};
		C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e0, packColor(c));
		C3D_TexEnvBufUpdate(C3D_Alpha, BIT(0));
		// Stage 1: a = dst.a * (1 - co1) = w.
		C3D_TexEnv *e1 = C3D_GetTexEnv(1);
		C3D_TexEnvSrc(e1, C3D_Alpha, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvOpAlpha(e1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
		C3D_TexEnvFunc(e1, C3D_Alpha, GPU_MODULATE);
		// Stage 2: rgb = dst * w + src * (1 - w); a = w + co1 (buffer: stage 0's output).
		C3D_TexEnv *e2 = C3D_GetTexEnv(2);
		C3D_TexEnvSrc(e2, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(e2, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS);
		C3D_TexEnvFunc(e2, C3D_Alpha, GPU_ADD);
		return;
	}
	case FragKind::TransSimple: {
		// transSimple.frag: mix(frozen, current, prog); unit 0 current, unit 1 frozen
		float p = uniform1(prog, "prog", 0), c[4] = {p, p, p, p};
		C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE1, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_Both, GPU_INTERPOLATE);
		C3D_TexEnvColor(e0, packColor(c));
		return;
	}
	case FragKind::Trans:
		transChain(uniform1(prog, "prog", 0), uniform1(prog, "vague", 0));
		return;
	default: {
		if (kind == FragKind::Unknown) warnOnce("unknown fragment program: drawn as plain texture");
		effectChain(0, nullptr, uniform1(prog, "opacity", 1.0f) * alphaFactor, nullptr, nullptr, false);
		return;
	}
	}
}

// Multiplies a column-major 4x4 matrix with (x, y, 0, 1).
inline void mulMat(const float *m, float x, float y, float out[4]) {
	for (int r = 0; r < 4; r++) out[r] = m[r] * x + m[4 + r] * y + m[12 + r];
}

// Multiplies a column-major 4x4 matrix with a vec4.
inline void mulMat4(const float *m, const float *v, float out[4]) {
	for (int r = 0; r < 4; r++) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
}

// One vertex attribute of a draw, resolved once per draw: base pointer, stride, vertex count.
struct AttribSource {
	const uint8_t *base = nullptr;
	size_t stride = 0, count = 0; // count: vertices readable (bounds)
	const float *at(size_t vi) const { return (base && vi < count) ? (const float *)(base + vi * stride) : nullptr; }
};

AttribSource attribSource(const AttribPointer &a, bool enabled) {
	AttribSource s;
	if (!enabled) return s;
	auto it = st.buffers.find(a.buffer);
	if (it == st.buffers.end()) return s;
	size_t size = it->second.data.size(), need = a.size * sizeof(float);
	if (a.offset + need > size) return s;
	if (need == 0) return s; // never set (size 0)
	s.base = it->second.data.data() + a.offset;
	s.stride = a.stride ? a.stride : need;
	s.count = (size - a.offset - need) / s.stride + 1;
	return s;
}

// Everything transformVertex needs that is constant during a draw (uniforms by name, buffer
// lookups, texture scales): resolved once per draw instead of once per vertex.
const float XFORM_IDENTITY[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

struct Xform {
	const float *proj = XFORM_IDENTITY, *model = nullptr, *texSizeInv = nullptr, *subRect = nullptr;
	float tx = 0, ty = 0;
	bool screen = false, tilemap = false, blit = false;
	const float *aniIndex = nullptr, *atFrames = nullptr; // tilemap.vert autotile animation
	AttribSource pos, tc, color;
	float scaleU[3] = {1, 1, 1}, scaleV[3] = {1, 1, 1}; // logical -> padded texture per unit
};

Xform makeXform(const Program *prog, const VertexArray &va, bool screen, Tex *const tex[3]) {
	Xform x;
	if (const float *pm = prog->get("projMat")) x.proj = pm;
	x.texSizeInv = prog->get("texSizeInv");
	x.model = prog->vert == VertKind::Sprite ? prog->get("spriteMat")
	          : prog->vert == VertKind::SimpleMatrix ? prog->get("matrix")
	                                                 : nullptr;
	const float *translation = prog->get("translation");
	if (translation && (prog->vert == VertKind::Simple || prog->vert == VertKind::SimpleColor || prog->vert == VertKind::Tilemap))
		x.tx = translation[0], x.ty = translation[1];
	x.screen = screen;
	x.tilemap = prog->vert == VertKind::Tilemap;
	if (x.tilemap) {
		x.aniIndex = prog->get("aniIndex");
		x.atFrames = prog->get("atFrames");
	}
	x.blit = prog->frag == FragKind::Blit;
	if (x.blit) x.subRect = prog->get("subRect");
	x.pos = attribSource(va.attribs[0], true);
	x.tc = attribSource(va.attribs[1], va.attribs[1].enabled);
	x.color = attribSource(va.attribs[2], va.attribs[2].enabled);
	for (int i = 0; i < 3; i++)
		if (tex[i]) x.scaleU[i] = (float)tex[i]->w / tex[i]->pw, x.scaleV[i] = (float)tex[i]->h / tex[i]->ph;
	return x;
}

// tilemap.vert's autotile animation: texture coordinate offset of animated autotiles.
void tilemapAnimate(const Xform &x, float &u, float &v) {
	const float tileW = 32, tileH = 32, autotileW = 3 * tileW, autotileH = 4 * tileW;
	const int nAutotiles = 7;
	if (!x.aniIndex || !x.atFrames) return;
	if (!(u <= autotileW && v <= autotileH * nAutotiles)) return;
	int atIndex = std::min(std::max((int)(v / autotileH), 0), nAutotiles - 1);
	int frames = (int)x.atFrames[atIndex], ani = (int)x.aniIndex[0];
	if (frames <= 0) return;
	int frame = ani % frames;
	int row = frame / 8, col = frame - 8 * row;
	u += 3 * tileW * col;
	v += tileH * row;
}

// A vertex before it goes to the GPU: Vtx plus the program's texture coordinate (v_texCoord,
// normalized; used for the sprite bush).
struct CpuVtx {
	Vtx v;
	float u, w;
};

// Runs the vertex program on the CPU for one vertex (per-draw state in x, see makeXform).
void transformVertex(const Xform &x, size_t vi, CpuVtx &cv) {
	Vtx &v = cv.v;
	const float *p = x.pos.at(vi);
	const float *t = x.tc.at(vi);
	const float *c = x.color.at(vi);
	float px = (p ? p[0] : 0) + x.tx, py = (p ? p[1] : 0) + x.ty;
	float clip[4];
	if (x.model) {
		float s[4];
		mulMat(x.model, px, py, s);
		mulMat4(x.proj, s, clip);
	} else {
		mulMat(x.proj, px, py, clip);
	}
	if (x.screen) {
		float cx = clip[0];
		clip[0] = SCREEN_XX * clip[1];
		clip[1] = SCREEN_YX * cx;
	}
	v.pos[0] = clip[0];
	v.pos[1] = clip[1];
	v.pos[2] = -0.5f * clip[3]; // PICA clip space: -w <= z <= 0
	v.pos[3] = clip[3];
	float u = t ? t[0] : 0, w = t ? t[1] : 0;
	if (x.tilemap) tilemapAnimate(x, u, w);
	if (x.texSizeInv) {
		u *= x.texSizeInv[0];
		w *= x.texSizeInv[1];
	}
	cv.u = u, cv.w = w;
	// logical [0, 1] texture coordinates -> each unit's padded texture
	float *tcs[3] = {v.tc, v.tc1, v.tc2};
	for (int i = 0; i < 3; i++) {
		float du = u, dv = w;
		// bitmapBlit.frag: dstCoor = (v_texCoord - subRect.xy) * subRect.zw
		if (i == 1 && x.subRect) du = (u - x.subRect[0]) * x.subRect[2], dv = (w - x.subRect[1]) * x.subRect[3];
		tcs[i][0] = du * x.scaleU[i];
		tcs[i][1] = dv * x.scaleV[i];
	}
	if (c)
		memcpy(v.color, c, sizeof v.color);
	else
		v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1;
}

CpuVtx lerpVtx(const CpuVtx &a, const CpuVtx &b, float t) {
	CpuVtx r;
	const float *fa = (const float *)&a, *fb = (const float *)&b;
	float *fr = (float *)&r;
	for (size_t i = 0; i < sizeof(CpuVtx) / sizeof(float); i++) fr[i] = fa[i] + (fb[i] - fa[i]) * t;
	return r;
}

// Sprite bush (sprite.frag): fragments where (v_texCoord on one side of the bush line) ==
// bushUnder keep their alpha, the others get bushOpacity. The TEV cannot test texture
// coordinates, so the triangles are split along the line on the CPU (all attributes are linear
// over a triangle). Returns false if the program has no bush to apply.
bool splitBush(const Program *prog, const std::vector<CpuVtx> &in, std::vector<CpuVtx> &keep, std::vector<CpuVtx> &faded, float *fadedFactor) {
	if (prog->frag != FragKind::Sprite) return false;
	float bushOpacity = uniform1(prog, "bushOpacity", 1.0f);
	if (bushOpacity >= 1.0f) return false;
	bool bushY = uniform1(prog, "bushY", 1) != 0, bushUnder = uniform1(prog, "bushUnder", 1) != 0;
	float slope = uniform1(prog, "bushSlope", 0), intercept = uniform1(prog, "bushIntercept", 1);
	// f < 0: underBush
	auto f = [&](const CpuVtx &v) { return bushY ? v.w - (slope * v.u + intercept) : v.u - (slope * v.w + intercept); };
	*fadedFactor = std::max(bushOpacity, 0.0f);
	for (size_t t = 0; t + 2 < in.size(); t += 3) {
		const CpuVtx *tri = &in[t];
		for (int side = 0; side < 2; side++) {
			bool wantUnder = side == 0;
			std::vector<CpuVtx> &out = (wantUnder == bushUnder) ? keep : faded;
			// Sutherland-Hodgman against the half-plane (f < 0 for under, f >= 0 above)
			CpuVtx poly[4];
			int n = 0;
			for (int i = 0; i < 3; i++) {
				const CpuVtx &a = tri[i], &b = tri[(i + 1) % 3];
				float fa = wantUnder ? -f(a) : f(a), fb = wantUnder ? -f(b) : f(b);
				if (fa >= 0) poly[n++] = a;
				if ((fa >= 0) != (fb >= 0) && n < 4) poly[n++] = lerpVtx(a, b, fa / (fa - fb));
			}
			for (int i = 1; i + 1 < n; i++) {
				out.push_back(poly[0]);
				out.push_back(poly[i]);
				out.push_back(poly[i + 1]);
			}
		}
	}
	return true;
}

// Binds t to a PICA texture unit, or unbinds the unit. citro3d 1.7.1 C3D_TexBind accepts NULL only
// for unit 0 (texture.c: C3D_TexGetType(tex) for units 1 and 2 dereferences it; a data abort on the
// console, Azahar reads 0): units 1 and 2 get a dummy texture no TEV preset samples. Either way
// citro3d keeps no pointer to a texture that may be deleted.
void bindSampled(int unit, Tex *t) {
	if (!t) {
		C3D_TexBind(unit, unit == 0 ? nullptr : &st.dummyTex);
		return;
	}
	C3D_TexSetFilter(&t->tex, t->linear ? GPU_LINEAR : GPU_NEAREST, t->linear ? GPU_LINEAR : GPU_NEAREST);
	C3D_TexSetWrap(&t->tex, t->repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, t->repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
	C3D_TexBind(unit, &t->tex);
}

// Sends vertices (whole triangles) to the GPU in chunks that fit the vertex ring, with the TEV
// and blend state of prog (nullptr: the clear quad).
void emit(const std::vector<CpuVtx> &verts, C3D_RenderTarget *target, bool screen, Tex *targetTex, const Program *prog, Tex *const tex[3],
          float alphaFactor, bool clearQuad) {
	for (size_t first = 0; first < verts.size();) {
		size_t n = std::min(RING_VERTICES, verts.size() - first);
		beginFrame();
		if (st.ringUsed + n > RING_VERTICES || gpuCmdBufOffset + CMDBUF_RESERVE_WORDS > gpuCmdBufSize) {
			endFrame(); // vertex ring or command buffer full: submit; beginFrame waits for it
			beginFrame();
		}
		C3D_FrameDrawOn(target);
		Vtx *out = st.ring + st.ringUsed;
		for (size_t i = 0; i < n; i++) out[i] = verts[first + i].v;
		if (clearQuad) {
			for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
			C3D_TexEnvBufUpdate(C3D_Both, 0);
			C3D_TexEnv *e0 = C3D_GetTexEnv(0);
			C3D_TexEnvSrc(e0, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		} else {
			setupTexEnv(prog, alphaFactor);
			if (prog->frag == FragKind::Blit)
				// the TEV preset already composites with the destination copy (unit 1)
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
			else if (st.blend)
				C3D_AlphaBlend(toBlendEquation(st.blendEq), toBlendEquation(st.blendEq), blendFactor(st.blendSrcRgb),
				               blendFactor(st.blendDstRgb), blendFactor(st.blendSrcA), blendFactor(st.blendDstA));
			else
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		}
		// unbind what is not sampled: citro3d keeps pointers to bound C3D_Tex structs
		for (int i = 0; i < 3; i++) {
			bindSampled(i, tex[i]);
			if (tex[i]) tex[i]->useSerial = st.frameSerial;
		}
		if (targetTex) targetTex->useSerial = st.frameSerial;
		if (!applyViewportScissor(screen, st.scissorTest, targetTex, clearQuad)) return; // empty scissor box
		C3D_BindProgram(&st.shader);
		C3D_SetAttrInfo(&st.attrInfo);
		C3D_BufInfo *bi = C3D_GetBufInfo();
		BufInfo_Init(bi);
		BufInfo_Add(bi, out, sizeof(Vtx), 5, 0x43210);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
		C3D_CullFace(GPU_CULL_NONE);
		C3D_DrawArrays(GPU_TRIANGLES, 0, (int)n);
		st.ringUsed += n;
		first += n;
	}
}

// Draws count indexed vertices (GL_TRIANGLES, uint16 indices at byte offset) with the current
// program, textures and state; clearQuad draws a quad of quadColor over the whole target.
void draw(GLsizei count, uintptr_t indexOffset, bool clearQuad = false, const float *quadColor = nullptr) {
	bool screen;
	Tex *targetTex;
	C3D_RenderTarget *target = currentTarget(&screen, &targetTex); // may end the frame
	if (!target) return;
	static std::vector<CpuVtx> verts, faded;
	verts.clear();
	Tex *tex[3] = {nullptr, nullptr, nullptr};
	Program *prog = nullptr;
	if (clearQuad) {
		// two triangles over the target in clip space, constant color, no blending
		static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {1, 1}, {-1, 1}, {-1, -1}};
		verts.resize(6);
		for (int i = 0; i < 6; i++) {
			Vtx &v = verts[i].v;
			memset(&verts[i], 0, sizeof(CpuVtx));
			float x = corners[i][0], y = corners[i][1];
			v.pos[0] = screen ? SCREEN_XX * y : x;
			v.pos[1] = screen ? SCREEN_YX * x : y;
			v.pos[2] = -0.5f; // PICA clip space: -w <= z <= 0
			v.pos[3] = 1;
			memcpy(v.color, quadColor, sizeof v.color);
		}
		emit(verts, target, screen, targetTex, nullptr, tex, 1.0f, true);
		return;
	}
	auto pit = st.programs.find(st.program);
	prog = pit == st.programs.end() ? nullptr : &pit->second;
	if (!prog) return;
	const VertexArray &va = st.boundVertexArray ? st.vertexArrays[st.boundVertexArray] : st.defaultVertexArray;
	auto ib = st.buffers.find(va.elementBuffer);
	if (ib == st.buffers.end() || indexOffset + (size_t)count * 2 > ib->second.data.size()) return;
	const uint16_t *indices = (const uint16_t *)(ib->second.data.data() + indexOffset);
	Units units = unitsFor(prog);
	for (int i = 0; i < units.n; i++) {
		tex[i] = texture(st.boundTex[units.gl[i]]);
		if (!tex[i] || !tex[i]->allocated) return;
		if (tex[i] == targetTex) {
			warnOnce("feedback loop (texture is the render target): skipped");
			return;
		}
	}
	count -= count % 3;
	verts.resize(count);
	rmxp3dsProfileAdd(RP_GLDRAWS, 1);
	{
		Rmxp3dsProfileScope prof(RP_GLXFORM);
		// each distinct vertex once (quads use 4 of every 6 indices), then gathered by index
		const Xform x = makeXform(prog, va, screen, tex);
		uint16_t lo = 0xffff, hi = 0;
		for (GLsizei i = 0; i < count; i++) lo = std::min(lo, indices[i]), hi = std::max(hi, indices[i]);
		static std::vector<CpuVtx> unique;
		if (count > 0) {
			unique.resize((size_t)hi - lo + 1);
			for (size_t vi = lo; vi <= hi; vi++) transformVertex(x, vi, unique[vi - lo]);
			for (GLsizei i = 0; i < count; i++) verts[i] = unique[indices[i] - lo];
		}
	}
	static std::vector<CpuVtx> keep;
	keep.clear();
	faded.clear();
	float fadedFactor = 1.0f;
	if (splitBush(prog, verts, keep, faded, &fadedFactor)) {
		emit(keep, target, screen, targetTex, prog, tex, 1.0f, false);
		emit(faded, target, screen, targetTex, prog, tex, fadedFactor, false);
	} else {
		emit(verts, target, screen, targetTex, prog, tex, 1.0f, false);
	}
}

// ---- GL entry points ---------------------------------------------------------------------------

GLenum APIENTRY getError() { return GL_NO_ERROR; }

void APIENTRY clearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
	st.clearColor[0] = r;
	st.clearColor[1] = g;
	st.clearColor[2] = b;
	st.clearColor[3] = a;
}

void APIENTRY clear(GLbitfield mask) {
	if (!(mask & GL_COLOR_BUFFER_BIT)) return;
	Tex *t = framebufferTexture();
	if (t && !t->inVram)
		cpuClear(*t, st.clearColor); // not a render target yet: keep it in linear memory
	else
		draw(6, 0, true, st.clearColor);
}

const GLubyte *APIENTRY getString(GLenum name) {
	switch (name) {
	case GL_VENDOR: return (const GLubyte *)"mkxp-z-3ds";
	case GL_RENDERER: return (const GLubyte *)"PICA200 (citro3d)";
	case GL_VERSION: return (const GLubyte *)"OpenGL ES 2.0 (3DS subset)";
	case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00 (programs mapped to TEV)";
	default: return (const GLubyte *)"";
	}
}

void APIENTRY getIntegerv(GLenum pname, GLint *params) {
	switch (pname) {
	// The PICA200 limit. Larger bitmaps become CPU "mega surfaces" in mkxp-z: the native
	// Essentials map renderer (tilerenderer.cpp, rmxp3ds D17) slices them into 1024 px atlas pages.
	// mkxp-z's RGSS Tilemap rejects tilesets taller than about 1300 px at this size; the rmxp3ds
	// converter splits those (PLAN 3.2).
	case GL_MAX_TEXTURE_SIZE: *params = 1024; break;
	case GL_MAX_TEXTURE_IMAGE_UNITS: *params = 3; break;
	case GL_MAX_VERTEX_ATTRIBS: *params = 12; break;
	case GL_VIEWPORT: memcpy(params, st.viewport, sizeof st.viewport); break;
	default: *params = 0; break;
	}
}

void APIENTRY pixelStorei(GLenum pname, GLint param) {
	if (pname == GL_UNPACK_ROW_LENGTH) st.unpackRowLength = param;
	else if (pname == GL_UNPACK_SKIP_ROWS) st.unpackSkipRows = param;
	else if (pname == GL_UNPACK_SKIP_PIXELS) st.unpackSkipPixels = param;
}

void APIENTRY readPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum, GLvoid *pixels) {
	size_t bpp = (format == GL_RGBA || format == GL_BGRA) ? 4 : 3;
	memset(pixels, 0, (size_t)w * (size_t)h * bpp);
	auto it = st.framebuffers.find(st.framebuffer);
	Tex *t = it == st.framebuffers.end() ? nullptr : texture(it->second);
	if (!t || !t->allocated) {
		warnOnce("ReadPixels only supports texture framebuffers");
		return;
	}
	if (!makeCpuAccessible(*t)) return;
	syncIfUsed(*t); // queued or running draws into it
	GSPGPU_InvalidateDataCache(t->tex.data, t->tex.size);
	const uint8_t *src = (const uint8_t *)t->tex.data;
	uint8_t *dst = (uint8_t *)pixels;
	for (GLsizei row = 0; row < h; row++) {
		for (GLsizei col = 0; col < w; col++) {
			GLint tx = x + col, ty = y + row;
			if (tx < 0 || ty < 0 || tx >= t->w || ty >= t->h) continue;
			const uint8_t *s = src + tiledOffset(tx, t->ph - 1 - ty, t->pw); // A B G R
			uint8_t *d = dst + ((size_t)row * w + col) * bpp;
			if (format == GL_BGRA) {
				d[0] = s[1], d[1] = s[2], d[2] = s[3];
			} else {
				d[0] = s[3], d[1] = s[2], d[2] = s[1];
			}
			if (bpp == 4) d[3] = s[0];
		}
	}
}

void APIENTRY enable(GLenum cap) {
	if (cap == GL_BLEND) st.blend = true;
	else if (cap == GL_SCISSOR_TEST) st.scissorTest = true;
}
void APIENTRY disable(GLenum cap) {
	if (cap == GL_BLEND) st.blend = false;
	else if (cap == GL_SCISSOR_TEST) st.scissorTest = false;
}
void APIENTRY scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
	st.scissor[0] = x, st.scissor[1] = y, st.scissor[2] = w, st.scissor[3] = h;
}
void APIENTRY viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
	st.viewport[0] = x, st.viewport[1] = y, st.viewport[2] = w, st.viewport[3] = h;
}
void APIENTRY blendFunc(GLenum s, GLenum d) {
	st.blendSrcRgb = st.blendSrcA = s;
	st.blendDstRgb = st.blendDstA = d;
}
void APIENTRY blendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
	st.blendSrcRgb = sr, st.blendDstRgb = dr, st.blendSrcA = sa, st.blendDstA = da;
}
void APIENTRY blendEquation(GLenum e) { st.blendEq = e; }
void APIENTRY drawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
	Rmxp3dsProfileScope prof_(RP_GLDRAW); // frame profile (frameprofile.h)
	if (mode != GL_TRIANGLES || type != GL_UNSIGNED_SHORT) {
		warnOnce("DrawElements: only GL_TRIANGLES with GL_UNSIGNED_SHORT");
		return;
	}
	draw(count, (uintptr_t)indices);
}

void genNames(GLsizei n, GLuint *out) {
	for (GLsizei i = 0; i < n; i++) out[i] = st.nextName++;
}

void APIENTRY genTextures(GLsizei n, GLuint *t) {
	genNames(n, t);
	for (GLsizei i = 0; i < n; i++) st.textures[t[i]];
}

void releaseTex(Tex &t) {
	if (t.target) st.deadTargets.push_back(t.target);
	if (t.allocated) st.deadTextures.push_back(t.tex);
	t.target = nullptr;
	t.allocated = false;
	t.inVram = false;
}

void APIENTRY deleteTextures(GLsizei n, const GLuint *t) {
	for (GLsizei i = 0; i < n; i++) {
		Tex *tex = texture(t[i]);
		if (!tex) continue;
		releaseTex(*tex);
		st.textures.erase(t[i]);
		for (GLuint &b : st.boundTex)
			if (b == t[i]) b = 0;
	}
	// freed once the GPU has finished the frames that may use them (beginFrame/waitGpu)
}

void APIENTRY bindTexture(GLenum, GLuint t) { st.boundTex[st.activeUnit] = t; }

// Copies rows of RGBA bytes (GL order: first row = t 0) into the tiled texture at (x, y).
void uploadRect(Tex &t, GLint x, GLint y, GLsizei w, GLsizei h, const uint8_t *data, GLenum format) {
	if (t.inVram) { // never: callers move the texture to linear memory first
		warnOnce("BUG: CPU upload into a VRAM texture skipped");
		return;
	}
	GLint rowLen = st.unpackRowLength > 0 ? st.unpackRowLength : w;
	data += ((size_t)st.unpackSkipRows * rowLen + st.unpackSkipPixels) * 4;
	uint8_t *dst = (uint8_t *)t.tex.data;
	for (GLsizei row = 0; row < h; row++) {
		const uint8_t *src = data + (size_t)row * rowLen * 4;
		for (GLsizei col = 0; col < w; col++, src += 4) {
			uint8_t *d = dst + tiledOffset(x + col, t.ph - 1 - (y + row), t.pw); // A B G R
			if (format == GL_BGRA) {
				d[0] = src[3], d[1] = src[0], d[2] = src[1], d[3] = src[2];
			} else {
				d[0] = src[3], d[1] = src[2], d[2] = src[1], d[3] = src[0];
			}
		}
	}
}

void APIENTRY texImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum, const GLvoid *data) {
	Rmxp3dsProfileScope prof_(RP_GLUPLOAD); // frame profile (frameprofile.h)
	Tex *t = texture(st.boundTex[st.activeUnit]);
	if (!t) return;
	// freed when the GPU is done with it; a texture in VRAM gets new linear memory (its content is
	// replaced anyway, and the CPU cannot write VRAM)
	if (t->allocated && (t->w != w || t->h != h || t->inVram)) releaseTex(*t);
	t->w = w, t->h = h;
	if (w > MAX_TEX || h > MAX_TEX) {
		warnOnce("texture larger than 1024 px not supported yet: " + std::to_string(w) + "x" + std::to_string(h));
		return;
	}
	if (!t->allocated) {
		t->pw = pow2(w), t->ph = pow2(h);
		if (!allocLinear(&t->tex, t->pw, t->ph)) {
			warnOnce("out of linear memory for textures");
			return;
		}
		t->allocated = true;
		t->inVram = false;
		t->useSerial = 0;
	} else {
		prepareCpuWrite(*t, false); // same size: draws queued before keep the old content
	}
	memset(t->tex.data, 0, t->tex.size);
	if (data) uploadRect(*t, 0, 0, w, h, (const uint8_t *)data, format);
	C3D_TexFlush(&t->tex);
}

void APIENTRY texSubImage2D(GLenum, GLint, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum, const GLvoid *data) {
	Rmxp3dsProfileScope prof_(RP_GLUPLOAD); // frame profile (frameprofile.h)
	Tex *t = texture(st.boundTex[st.activeUnit]);
	if (!t || !t->allocated || !data) return;
	if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
	if (!makeCpuAccessible(*t)) return;
	prepareCpuWrite(*t, true); // draws queued before must see the old content
	uploadRect(*t, x, y, w, h, (const uint8_t *)data, format);
	C3D_TexFlush(&t->tex);
}

void APIENTRY texParameteri(GLenum, GLenum pname, GLint param) {
	Tex *t = texture(st.boundTex[st.activeUnit]);
	if (!t) return;
	if (pname == GL_TEXTURE_MAG_FILTER) t->linear = param == GL_LINEAR;
	else if (pname == GL_TEXTURE_WRAP_S) t->repeat = param == GL_REPEAT;
}
void APIENTRY activeTexture(GLenum unit) {
	GLuint u = unit - GL_TEXTURE0;
	if (u < 4) st.activeUnit = (int)u;
	else warnOnce("ActiveTexture: unit " + std::to_string(u) + " not supported");
}
void APIENTRY generateMipmap(GLenum) {}

void APIENTRY genBuffers(GLsizei n, GLuint *b) { genNames(n, b); }
void APIENTRY deleteBuffers(GLsizei n, const GLuint *b) {
	for (GLsizei i = 0; i < n; i++) st.buffers.erase(b[i]);
}
void APIENTRY bindBuffer(GLenum target, GLuint b) {
	if (target == GL_ARRAY_BUFFER) st.arrayBuffer = b;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
		(st.boundVertexArray ? st.vertexArrays[st.boundVertexArray] : st.defaultVertexArray).elementBuffer = b;
}
GLuint boundBufferFor(GLenum target) {
	if (target == GL_ARRAY_BUFFER) return st.arrayBuffer;
	return (st.boundVertexArray ? st.vertexArrays[st.boundVertexArray] : st.defaultVertexArray).elementBuffer;
}
// Vertex data is read on the CPU at draw time; draws copy it into the GPU ring first, so the
// buffer can change right away.
void APIENTRY bufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum) {
	Buffer &b = st.buffers[boundBufferFor(target)];
	b.data.assign((size_t)size, 0);
	if (data) memcpy(b.data.data(), data, (size_t)size);
}
void APIENTRY bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data) {
	Buffer &b = st.buffers[boundBufferFor(target)];
	if ((size_t)(offset + size) > b.data.size()) b.data.resize((size_t)(offset + size));
	memcpy(b.data.data() + offset, data, (size_t)size);
}

GLuint APIENTRY createShader(GLenum type) {
	GLuint n = st.nextName++;
	st.shaders[n].type = type;
	return n;
}
void APIENTRY deleteShader(GLuint s) { st.shaders.erase(s); }
void APIENTRY shaderSource(GLuint s, GLsizei count, const GLchar *const *strings, const GLint *) {
	if (count > 0) st.shaders[s].src = strings[count - 1];
}
void APIENTRY compileShader(GLuint) {}
void APIENTRY attachShader(GLuint p, GLuint s) { st.programs[p].shaders.push_back(s); }
void APIENTRY getShaderiv(GLuint, GLenum pname, GLint *params) { *params = (pname == GL_COMPILE_STATUS) ? GL_TRUE : 0; }
void APIENTRY getShaderInfoLog(GLuint, GLsizei, GLsizei *length, GLchar *log) {
	if (length) *length = 0;
	if (log) log[0] = 0;
}

GLuint APIENTRY createProgram() {
	GLuint n = st.nextName++;
	st.programs[n];
	return n;
}
void APIENTRY deleteProgram(GLuint p) { st.programs.erase(p); }
void APIENTRY useProgram(GLuint p) { st.program = p; }

// Identifies the program by the sources of its shaders.
void APIENTRY linkProgram(GLuint p) {
	Program &prog = st.programs[p];
	for (GLuint s : prog.shaders) {
		const void *src = st.shaders[s].src;
		if (src == mkxp_shader_simple_vert) prog.vert = VertKind::Simple;
		else if (src == mkxp_shader_simpleColor_vert) prog.vert = VertKind::SimpleColor;
		else if (src == mkxp_shader_sprite_vert) prog.vert = VertKind::Sprite;
		else if (src == mkxp_shader_minimal_vert) prog.vert = VertKind::Minimal;
		else if (src == mkxp_shader_tilemap_vert) prog.vert = VertKind::Tilemap;
		else if (src == mkxp_shader_simpleMatrix_vert) prog.vert = VertKind::SimpleMatrix;
		else if (src == mkxp_shader_simple_frag) prog.frag = FragKind::Simple;
		else if (src == mkxp_shader_simpleColor_frag) prog.frag = FragKind::SimpleColor;
		else if (src == mkxp_shader_simpleAlpha_frag) prog.frag = FragKind::SimpleAlpha;
		else if (src == mkxp_shader_simpleAlphaUni_frag) prog.frag = FragKind::SimpleAlphaUni;
		else if (src == mkxp_shader_flatColor_frag) prog.frag = FragKind::FlatColor;
		else if (src == mkxp_shader_sprite_frag) prog.frag = FragKind::Sprite;
		else if (src == mkxp_shader_bitmapBlit_frag) prog.frag = FragKind::Blit;
		else if (src == mkxp_shader_plane_frag) prog.frag = FragKind::Plane;
		else if (src == mkxp_shader_transSimple_frag) prog.frag = FragKind::TransSimple;
		else if (src == mkxp_shader_tilemap_frag) prog.frag = FragKind::Tilemap;
		else if (src == mkxp_shader_flashMap_frag) prog.frag = FragKind::FlashMap;
		else if (src == mkxp_shader_trans_frag) prog.frag = FragKind::Trans;
		else if (src == mkxp_shader_gray_frag) prog.frag = FragKind::Gray;
	}
}
void APIENTRY getProgramiv(GLuint, GLenum pname, GLint *params) { *params = (pname == GL_LINK_STATUS) ? GL_TRUE : 0; }
void APIENTRY getProgramInfoLog(GLuint, GLsizei, GLsizei *length, GLchar *log) {
	if (length) *length = 0;
	if (log) log[0] = 0;
}

GLint APIENTRY getUniformLocation(GLuint program, const GLchar *name) {
	auto &locs = st.programs[program].locations;
	auto it = locs.find(name);
	if (it != locs.end()) return it->second;
	GLint loc = (GLint)locs.size();
	locs[name] = loc;
	return loc;
}
Program *current() {
	auto it = st.programs.find(st.program);
	return it == st.programs.end() ? nullptr : &it->second;
}
void APIENTRY uniform1f(GLint loc, GLfloat a) {
	if (Program *p = current()) p->value(loc)[0] = a;
}
void APIENTRY uniform2f(GLint loc, GLfloat a, GLfloat b) {
	if (Program *p = current()) {
		auto &v = p->value(loc);
		v[0] = a, v[1] = b;
	}
}
void APIENTRY uniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
	if (Program *p = current()) {
		auto &v = p->value(loc);
		v[0] = a, v[1] = b, v[2] = c, v[3] = d;
	}
}
void APIENTRY uniform1i(GLint loc, GLint a) {
	if (Program *p = current()) p->value(loc)[0] = (float)a;
}
void APIENTRY uniform1iv(GLint loc, GLsizei n, const GLint *a) {
	if (Program *p = current())
		for (GLsizei i = 0; i < n && i < 16; i++) p->value(loc)[i] = (float)a[i];
}
void APIENTRY uniformMatrix4fv(GLint loc, GLsizei, GLboolean, const GLfloat *m) {
	if (Program *p = current()) memcpy(p->value(loc).data(), m, 16 * sizeof(float));
}
void APIENTRY bindAttribLocation(GLuint, GLuint, const GLchar *) {}

VertexArray &currentVertexArray() {
	return st.boundVertexArray ? st.vertexArrays[st.boundVertexArray] : st.defaultVertexArray;
}
void APIENTRY enableVertexAttribArray(GLuint i) {
	if (i < 3) currentVertexArray().attribs[i].enabled = true;
}
void APIENTRY disableVertexAttribArray(GLuint i) {
	if (i < 3) currentVertexArray().attribs[i].enabled = false;
}
void APIENTRY vertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean, GLsizei stride, const GLvoid *ptr) {
	if (i >= 3) return;
	if (type != GL_FLOAT) warnOnce("VertexAttribPointer: only GL_FLOAT");
	AttribPointer &a = currentVertexArray().attribs[i];
	a.size = size;
	a.stride = stride ? stride : size * (GLsizei)sizeof(float);
	a.offset = (uintptr_t)ptr;
	a.buffer = st.arrayBuffer;
}
void APIENTRY releaseShaderCompiler() {}

void APIENTRY genFramebuffers(GLsizei n, GLuint *f) {
	genNames(n, f);
	for (GLsizei i = 0; i < n; i++) st.framebuffers[f[i]] = 0;
}
void APIENTRY deleteFramebuffers(GLsizei n, const GLuint *f) {
	for (GLsizei i = 0; i < n; i++) st.framebuffers.erase(f[i]);
}
void APIENTRY bindFramebuffer(GLenum, GLuint f) { st.framebuffer = f; }
void APIENTRY framebufferTexture2D(GLenum, GLenum, GLenum, GLuint t, GLint) { st.framebuffers[st.framebuffer] = t; }

void APIENTRY genVertexArrays(GLsizei n, GLuint *a) {
	genNames(n, a);
	for (GLsizei i = 0; i < n; i++) st.vertexArrays[a[i]];
}
void APIENTRY deleteVertexArrays(GLsizei n, const GLuint *a) {
	for (GLsizei i = 0; i < n; i++) st.vertexArrays.erase(a[i]);
}
void APIENTRY bindVertexArray(GLuint a) { st.boundVertexArray = a; }
} // namespace

// Texture memory known to gl3ds (frame profile): allocated textures in linear memory and VRAM
// (KiB) and their count, plus textures waiting for deferred deletion.
void gl3dsTextureStats(unsigned *linearKb, unsigned *vramKb, unsigned *count, unsigned *deadKb) {
	size_t lin = 0, vram = 0, dead = 0;
	unsigned n = 0;
	for (auto &kv : st.textures) {
		if (!kv.second.allocated) continue;
		++n;
		(kv.second.inVram ? vram : lin) += kv.second.tex.size;
	}
	for (C3D_Tex &t : st.deadTextures) dead += t.size;
	*linearKb = lin / 1024, *vramKb = vram / 1024, *count = n, *deadKb = dead / 1024;
}

void gl3dsAptResumed() {
	if (!aptHeld) return;
	aptHeld = false;
	RecursiveLock_Unlock(&gpuLock);
}

void gl3dsShutdown() {
	if (!st.c3dReady) return;
	aptUnhook(&aptCookie);
	if (aptHeld) { // closed while suspended: the RGSS thread has stopped, nothing waits for it
		aptHeld = false;
		RecursiveLock_Unlock(&gpuLock);
	}
	C3D_Fini(); // waits for and clears the GPU queue, destroys the render targets
	st.c3dReady = false;
}

void gl3dsPresent() {
	beginFrame(); // a frame with nothing drawn still shows the (cleared) screen
	C3D_FrameDrawOn(st.top);
	endFrame(true); // submitted; deferred deletions wait for the GPU (next beginFrame)
}

// Fills `gl`. Entries the 3DS does not provide stay null, which mkxp-z checks for:
// BlitFramebuffer (falls back to drawing quads), GenerateTextureMipmap, debug callbacks.
void initGLFunctions() {
	memset(&gl, 0, sizeof gl);
	gl.GetError = getError;
	gl.ClearColor = clearColor;
	gl.Clear = clear;
	gl.GetString = getString;
	gl.GetIntegerv = getIntegerv;
	gl.PixelStorei = pixelStorei;
	gl.ReadPixels = readPixels;
	gl.Enable = enable;
	gl.Disable = disable;
	gl.Scissor = scissor;
	gl.Viewport = viewport;
	gl.BlendFunc = blendFunc;
	gl.BlendFuncSeparate = blendFuncSeparate;
	gl.BlendEquation = blendEquation;
	gl.DrawElements = drawElements;
	gl.GenTextures = genTextures;
	gl.DeleteTextures = deleteTextures;
	gl.BindTexture = bindTexture;
	gl.TexImage2D = texImage2D;
	gl.TexSubImage2D = texSubImage2D;
	gl.TexParameteri = texParameteri;
	gl.ActiveTexture = activeTexture;
	gl.GenerateMipmap = generateMipmap;
	gl.GenBuffers = genBuffers;
	gl.DeleteBuffers = deleteBuffers;
	gl.BindBuffer = bindBuffer;
	gl.BufferData = bufferData;
	gl.BufferSubData = bufferSubData;
	gl.CreateShader = createShader;
	gl.DeleteShader = deleteShader;
	gl.ShaderSource = shaderSource;
	gl.CompileShader = compileShader;
	gl.AttachShader = attachShader;
	gl.GetShaderiv = getShaderiv;
	gl.GetShaderInfoLog = getShaderInfoLog;
	gl.CreateProgram = createProgram;
	gl.DeleteProgram = deleteProgram;
	gl.UseProgram = useProgram;
	gl.LinkProgram = linkProgram;
	gl.GetProgramiv = getProgramiv;
	gl.GetProgramInfoLog = getProgramInfoLog;
	gl.GetUniformLocation = getUniformLocation;
	gl.Uniform1f = uniform1f;
	gl.Uniform2f = uniform2f;
	gl.Uniform4f = uniform4f;
	gl.Uniform1i = uniform1i;
	gl.Uniform1iv = uniform1iv;
	gl.UniformMatrix4fv = uniformMatrix4fv;
	gl.BindAttribLocation = bindAttribLocation;
	gl.EnableVertexAttribArray = enableVertexAttribArray;
	gl.DisableVertexAttribArray = disableVertexAttribArray;
	gl.VertexAttribPointer = vertexAttribPointer;
	gl.ReleaseShaderCompiler = releaseShaderCompiler;
	gl.GenFramebuffers = genFramebuffers;
	gl.DeleteFramebuffers = deleteFramebuffers;
	gl.BindFramebuffer = bindFramebuffer;
	gl.FramebufferTexture2D = framebufferTexture2D;
	gl.GenVertexArrays = genVertexArrays;
	gl.DeleteVertexArrays = deleteVertexArrays;
	gl.BindVertexArray = bindVertexArray;
	gl.glsles = true;
	gl.unpack_subimage = true;
	gl.npot_repeat = false;
	initC3D();
}
