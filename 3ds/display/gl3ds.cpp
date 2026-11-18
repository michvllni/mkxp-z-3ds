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
//   A texture moves from linear memory to VRAM when it is first rendered into (PICA render
//   targets must be in VRAM: citro3d renderqueue.c, C3D_RenderTargetCreateFromTex).
// - Framebuffers: a texture's render target is created when it is first drawn to. Framebuffer 0
//   is the top screen (400x240 for mkxp-z; the physical framebuffer is rotated).
// - Programs: mkxp-z's programs are recognised by their embedded source (the pointers passed to
//   ShaderSource). Vertex programs run on the CPU at draw time (projection, sprite matrix,
//   translation, texture coordinates incl. padding); one pass-through PICA shader
//   (passthrough.v.pica) draws the result. Fragment programs are TEV presets.
// - Clear honours the scissor box (mkxp-z's fill_rect is scissor + clear): it draws a quad.
// - ReadPixels finishes the GPU work queued so far and detiles from the texture.
// - Deletions are deferred to the end of the frame (the GPU may still use the objects).
#include "gl-fun.h"
#include "gl3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "passthrough.shbin.xxd"

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

struct Vtx {
	float pos[4];
	float tc[2];
	float color[4];
};

struct Tex {
	GLsizei w = 0, h = 0;
	u16 pw = 0, ph = 0;
	bool allocated = false;
	C3D_Tex tex;
	C3D_RenderTarget *target = nullptr;
	bool linear = false;
	bool repeat = false;
	bool inVram = false; // moved to VRAM when first rendered into (PICA render targets must be in VRAM)
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
enum class FragKind { Unknown, Simple, SimpleColor, SimpleAlpha, SimpleAlphaUni, FlatColor, Sprite, Blit, Plane, TransSimple };

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
	C3D_RenderTarget *top = nullptr;
	shaderProgram_s shader;
	DVLB_s *dvlb = nullptr;
	C3D_AttrInfo attrInfo;
	Vtx *ring = nullptr;
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
	GLuint boundTex[2] = {0, 0};
	int activeUnit = 0;

	float clearColor[4] = {0, 0, 0, 0};
	bool blend = false, scissorTest = false;
	GLenum blendEq = GL_FUNC_ADD;
	GLenum blendSrcRgb = GL_ONE, blendDstRgb = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO;
	GLint scissor[4] = {0, 0, 0, 0};
	GLint viewport[4] = {0, 0, 400, 240};
	GLint unpackRowLength = 0, unpackSkipRows = 0, unpackSkipPixels = 0;

	// deferred deletions
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

void initC3D() {
	if (st.c3dReady) return;
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	st.top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
	C3D_RenderTargetSetOutput(st.top, GFX_TOP, GFX_LEFT,
	                          GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
	// DVLB_ParseFile reads u32 words and keeps pointers into the data: a 4-byte aligned copy that
	// lives as long as the program (the embedded array is a byte array without alignment).
	u32 *shbin = (u32 *)memalign(4, mkxp_shader_passthrough_shbin_len);
	memcpy(shbin, mkxp_shader_passthrough_shbin, mkxp_shader_passthrough_shbin_len);
	st.dvlb = DVLB_ParseFile(shbin, mkxp_shader_passthrough_shbin_len);
	shaderProgramInit(&st.shader);
	shaderProgramSetVsh(&st.shader, &st.dvlb->DVLE[0]);
	AttrInfo_Init(&st.attrInfo);
	AttrInfo_AddLoader(&st.attrInfo, 0, GPU_FLOAT, 4);
	AttrInfo_AddLoader(&st.attrInfo, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&st.attrInfo, 2, GPU_FLOAT, 4);
	st.ring = (Vtx *)linearAlloc(VERTEX_RING_BYTES);
	st.c3dReady = true;
}

void beginFrame() {
	initC3D();
	if (st.inFrame) return;
	C3D_FrameBegin(0);
	st.inFrame = true;
	st.ringUsed = 0;
}

void freeDeferred() {
	for (C3D_RenderTarget *t : st.deadTargets) C3D_RenderTargetDelete(t);
	for (C3D_Tex &t : st.deadTextures) C3D_TexDelete(&t);
	st.deadTargets.clear();
	st.deadTextures.clear();
}

// Ends the frame and waits until the GPU has executed everything (for read-back and buffer
// reuse). Shows the top screen only if it was drawn in this frame.
void finishFrame() {
	if (!st.inFrame) return;
	C3D_FrameEnd(0);
	st.inFrame = false;
	C3D_FrameBegin(0); // waits for the queued GPU work
	C3D_FrameEnd(0);
	freeDeferred();
}

Tex *texture(GLuint name) {
	auto it = st.textures.find(name);
	return it == st.textures.end() ? nullptr : &it->second;
}

// Render target of the bound framebuffer (nullptr: nothing to draw into). Sets *screen for the
// top screen.
C3D_RenderTarget *currentTarget(bool *screen, Tex **texOut) {
	*screen = st.framebuffer == 0;
	*texOut = nullptr;
	if (*screen) return st.top;
	auto it = st.framebuffers.find(st.framebuffer);
	if (it == st.framebuffers.end()) return nullptr;
	Tex *t = texture(it->second);
	if (!t || !t->allocated) return nullptr;
	if (!t->inVram) {
		// Move the texture to VRAM: finish queued GPU work that may use it, copy, free the old
		// linear copy at the end of the frame.
		C3D_Tex vram;
		if (!C3D_TexInitVRAM(&vram, t->pw, t->ph, GPU_RGBA8)) {
			warnOnce("out of VRAM for render targets (" + std::to_string(t->pw) + "x" + std::to_string(t->ph) + ")");
			return nullptr;
		}
		finishFrame();
		beginFrame();
		GSPGPU_InvalidateDataCache(t->tex.data, t->tex.size);
		memcpy(vram.data, t->tex.data, t->tex.size);
		GSPGPU_FlushDataCache(vram.data, vram.size);
		st.deadTextures.push_back(t->tex);
		t->tex = vram;
		t->inVram = true;
	}
	if (!t->target) t->target = C3D_RenderTargetCreateFromTex(&t->tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
	if (!t->target) warnOnce("C3D_RenderTargetCreateFromTex failed");
	*texOut = t;
	return t->target;
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
void applyViewportScissor(bool screen, bool scissorOn, const Tex *target, bool fullTarget) {
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
		return;
	}
	const GLint *s = st.scissor;
	if (screen)
		C3D_SetScissor(GPU_SCISSOR_NORMAL, s[1], 400 - (s[0] + s[2]), s[1] + s[3], 400 - s[0]);
	else
		C3D_SetScissor(GPU_SCISSOR_NORMAL, s[0], s[1], s[0] + s[2], s[1] + s[3]);
}

u32 packColor(const float *c) {
	auto b = [](float v) { return (u32)(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
	return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (b(c[3]) << 24);
}

// TEV stages for a fragment program; true if the texture unit 0 is sampled.
bool setupTexEnv(const Program *prog) {
	for (int i = 0; i < 6; i++) {
		C3D_TexEnv *env = C3D_GetTexEnv(i);
		C3D_TexEnvInit(env); // pass-through of the previous stage
	}
	C3D_TexEnv *e0 = C3D_GetTexEnv(0);
	FragKind kind = prog ? prog->frag : FragKind::Unknown;
	switch (kind) {
	case FragKind::FlatColor: {
		const float *c = prog->get("color");
		C3D_TexEnvSrc(e0, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
		C3D_TexEnvColor(e0, c ? packColor(c) : 0);
		return false;
	}
	case FragKind::SimpleColor:
		C3D_TexEnvSrc(e0, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
		return false;
	case FragKind::SimpleAlpha:
		C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
		return true;
	case FragKind::SimpleAlphaUni: {
		const float *a = prog->get("alpha");
		float c[4] = {0, 0, 0, a ? a[0] : 1.0f};
		C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e0, packColor(c));
		return true;
	}
	case FragKind::Sprite: {
		// sprite.frag without gray, pattern, invert and bush (not yet supported):
		// rgb = mix(tex.rgb + tone.rgb, color.rgb, color.a); a = tex.a * opacity
		const float *tone = prog->get("tone");
		const float *opacity = prog->get("opacity");
		const float *color = prog->get("color");
		float pos[4] = {0, 0, 0, opacity ? opacity[0] : 1.0f}, neg[4] = {0, 0, 0, 1};
		for (int i = 0; i < 3 && tone; i++) {
			pos[i] = std::max(tone[i], 0.0f);
			neg[i] = std::max(-tone[i], 0.0f);
		}
		if (tone && tone[3] > 0) warnOnce("sprite gray tone not supported yet");
		C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_ADD);
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e0, packColor(pos));
		C3D_TexEnv *e1 = C3D_GetTexEnv(1);
		C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e1, C3D_RGB, GPU_SUBTRACT);
		C3D_TexEnvColor(e1, packColor(neg));
		C3D_TexEnv *e2 = C3D_GetTexEnv(2);
		C3D_TexEnvSrc(e2, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
		C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(e2, C3D_RGB, GPU_INTERPOLATE);
		float c[4] = {0, 0, 0, 0};
		if (color) memcpy(c, color, sizeof c);
		C3D_TexEnvColor(e2, packColor(c));
		return true;
	}
	case FragKind::Blit:
		// bitmapBlit.frag composites against the destination with a division the TEV cannot do;
		// the texture is drawn with opacity and normal blending (exact for opaque destinations).
		warnOnce("Bitmap#blt: approximated by alpha blending");
		/* fallthrough */
	default: {
		if (kind == FragKind::Unknown) warnOnce("unknown fragment program: drawn as plain texture");
		if (kind == FragKind::Plane || kind == FragKind::TransSimple) warnOnce("plane/transition effects not supported yet");
		C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
		const float *opacity = prog ? prog->get("opacity") : nullptr;
		float c[4] = {0, 0, 0, opacity ? opacity[0] : 1.0f};
		C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e0, packColor(c));
		return true;
	}
	}
}

// Multiplies a column-major 4x4 matrix with (x, y, 0, 1).
inline void mulMat(const float *m, float x, float y, float out[4]) {
	for (int r = 0; r < 4; r++) out[r] = m[r] * x + m[4 + r] * y + m[12 + r];
}

const uint8_t *attribData(const AttribPointer &a, size_t vertex) {
	auto it = st.buffers.find(a.buffer);
	if (it == st.buffers.end()) return nullptr;
	size_t off = a.offset + vertex * (size_t)a.stride;
	if (off + a.size * sizeof(float) > it->second.data.size()) return nullptr;
	return it->second.data.data() + off;
}

// Draws count indexed vertices (GL_TRIANGLES, uint16 indices at byte offset) with the current
// program, textures and state.
void draw(GLsizei count, uintptr_t indexOffset, bool clearQuad = false, const float *quadColor = nullptr) {
	bool screen;
	Tex *targetTex;
	beginFrame();
	C3D_RenderTarget *target = currentTarget(&screen, &targetTex);
	if (!target) return;
	if (st.ringUsed + (size_t)count > VERTEX_RING_BYTES / sizeof(Vtx)) {
		finishFrame();
		beginFrame();
	}
	C3D_FrameDrawOn(target);
	Vtx *out = st.ring + st.ringUsed;
	Program *prog = nullptr;
	bool sampled = false;
	Tex *tex0 = nullptr;
	float uScale = 1, vScale = 1;

	if (clearQuad) {
		// two triangles over the viewport in clip space, constant color, no blending
		static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {1, 1}, {-1, 1}, {-1, -1}};
		for (int i = 0; i < 6; i++) {
			Vtx &v = out[i];
			float x = corners[i][0], y = corners[i][1];
			v.pos[0] = screen ? SCREEN_XX * y : x;
			v.pos[1] = screen ? SCREEN_YX * x : y;
			v.pos[2] = -0.5f; // PICA clip space: -w <= z <= 0
			v.pos[3] = 1;
			v.tc[0] = v.tc[1] = 0;
			memcpy(v.color, quadColor, sizeof v.color);
		}
		count = 6;
		C3D_TexEnv *e0 = C3D_GetTexEnv(0);
		for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
		C3D_TexEnvSrc(e0, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	} else {
		auto pit = st.programs.find(st.program);
		prog = pit == st.programs.end() ? nullptr : &pit->second;
		if (!prog) return;
		sampled = setupTexEnv(prog);
		if (sampled) {
			tex0 = texture(st.boundTex[0]);
			if (!tex0 || !tex0->allocated) return;
			if (tex0 == targetTex) {
				warnOnce("feedback loop (texture is the render target): skipped");
				return;
			}
			uScale = (float)tex0->w / tex0->pw;
			vScale = (float)tex0->h / tex0->ph;
		}
		const VertexArray &va = st.boundVertexArray ? st.vertexArrays[st.boundVertexArray] : st.defaultVertexArray;
		auto ib = st.buffers.find(va.elementBuffer);
		if (ib == st.buffers.end() || indexOffset + (size_t)count * 2 > ib->second.data.size()) return;
		const uint16_t *indices = (const uint16_t *)(ib->second.data.data() + indexOffset);
		const float *proj = prog->get("projMat");
		const float *spriteMat = prog->get("spriteMat");
		const float *translation = prog->get("translation");
		const float *texSizeInv = prog->get("texSizeInv");
		static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		if (!proj) proj = identity;
		for (GLsizei i = 0; i < count; i++) {
			size_t vi = indices[i];
			const float *p = (const float *)attribData(va.attribs[0], vi);
			const float *t = va.attribs[1].enabled ? (const float *)attribData(va.attribs[1], vi) : nullptr;
			const float *c = va.attribs[2].enabled ? (const float *)attribData(va.attribs[2], vi) : nullptr;
			Vtx &v = out[i];
			float x = p ? p[0] : 0, y = p ? p[1] : 0;
			if (translation && (prog->vert == VertKind::Simple || prog->vert == VertKind::Tilemap)) {
				x += translation[0];
				y += translation[1];
			}
			float clip[4];
			if (prog->vert == VertKind::Sprite && spriteMat) {
				float s[4];
				mulMat(spriteMat, x, y, s);
				for (int r = 0; r < 4; r++)
					clip[r] = proj[r] * s[0] + proj[4 + r] * s[1] + proj[8 + r] * s[2] + proj[12 + r] * s[3];
			} else {
				mulMat(proj, x, y, clip);
			}
			if (screen) {
				float cx = clip[0];
				clip[0] = SCREEN_XX * clip[1];
				clip[1] = SCREEN_YX * cx;
			}
			v.pos[0] = clip[0];
			v.pos[1] = clip[1];
			v.pos[2] = -0.5f * clip[3]; // PICA clip space: -w <= z <= 0
			v.pos[3] = clip[3];
			float u = t ? t[0] : 0, w = t ? t[1] : 0;
			if (texSizeInv) {
				u *= texSizeInv[0];
				w *= texSizeInv[1];
			}
			v.tc[0] = u * uScale;
			v.tc[1] = w * vScale;
			if (c)
				memcpy(v.color, c, sizeof v.color);
			else
				v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1;
		}
		if (prog->frag == FragKind::Blit)
			// approximation of bitmapBlit.frag (see setupTexEnv): normal alpha compositing
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
		else if (st.blend)
			C3D_AlphaBlend(toBlendEquation(st.blendEq), toBlendEquation(st.blendEq), blendFactor(st.blendSrcRgb),
			               blendFactor(st.blendDstRgb), blendFactor(st.blendSrcA), blendFactor(st.blendDstA));
		else
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		if (sampled) {
			C3D_TexSetFilter(&tex0->tex, tex0->linear ? GPU_LINEAR : GPU_NEAREST, tex0->linear ? GPU_LINEAR : GPU_NEAREST);
			C3D_TexSetWrap(&tex0->tex, tex0->repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, tex0->repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
			C3D_TexBind(0, &tex0->tex);
		}
	}
	applyViewportScissor(screen, st.scissorTest, targetTex, clearQuad);
	C3D_BindProgram(&st.shader);
	C3D_SetAttrInfo(&st.attrInfo);
	C3D_BufInfo *bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, out, sizeof(Vtx), 3, 0x210);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DrawArrays(GPU_TRIANGLES, 0, count);
	st.ringUsed += count;
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
	if (mask & GL_COLOR_BUFFER_BIT) draw(6, 0, true, st.clearColor);
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
	// The PICA200 limit is 1024 px, but mkxp-z sizes its tileset atlas by this value: with 1024,
	// tilesets taller than about 1300 px are rejected. Textures above 1024 px are not stored yet
	// (they will be split into pieces inside this table).
	case GL_MAX_TEXTURE_SIZE: *params = 4096; break;
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
	finishFrame();
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
	if (!st.inFrame) freeDeferred();
}

void APIENTRY bindTexture(GLenum, GLuint t) { st.boundTex[st.activeUnit] = t; }

// Copies rows of RGBA bytes (GL order: first row = t 0) into the tiled texture at (x, y).
void uploadRect(Tex &t, GLint x, GLint y, GLsizei w, GLsizei h, const uint8_t *data, GLenum format) {
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
	Tex *t = texture(st.boundTex[st.activeUnit]);
	if (!t) return;
	if (t->allocated && (t->w != w || t->h != h)) releaseTex(*t);
	t->w = w, t->h = h;
	if (w > MAX_TEX || h > MAX_TEX) {
		warnOnce("texture larger than 1024 px not supported yet: " + std::to_string(w) + "x" + std::to_string(h));
		return;
	}
	if (!t->allocated) {
		t->pw = pow2(w), t->ph = pow2(h);
		if (!C3D_TexInit(&t->tex, t->pw, t->ph, GPU_RGBA8)) {
			warnOnce("out of linear memory for textures");
			return;
		}
		t->allocated = true;
	}
	memset(t->tex.data, 0, t->tex.size);
	if (data) uploadRect(*t, 0, 0, w, h, (const uint8_t *)data, format);
	C3D_TexFlush(&t->tex);
}

void APIENTRY texSubImage2D(GLenum, GLint, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum, const GLvoid *data) {
	Tex *t = texture(st.boundTex[st.activeUnit]);
	if (!t || !t->allocated || !data) return;
	if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
	finishFrame(); // the texture may be in use by queued draws
	uploadRect(*t, x, y, w, h, (const uint8_t *)data, format);
	C3D_TexFlush(&t->tex);
}

void APIENTRY texParameteri(GLenum, GLenum pname, GLint param) {
	Tex *t = texture(st.boundTex[st.activeUnit]);
	if (!t) return;
	if (pname == GL_TEXTURE_MAG_FILTER) t->linear = param == GL_LINEAR;
	else if (pname == GL_TEXTURE_WRAP_S) t->repeat = param == GL_REPEAT;
}
void APIENTRY activeTexture(GLenum unit) { st.activeUnit = (unit - GL_TEXTURE0) & 1; }
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

void gl3dsPresent() {
	beginFrame(); // a frame with nothing drawn still shows the (cleared) screen
	C3D_FrameDrawOn(st.top);
	C3D_FrameEnd(0);
	st.inFrame = false;
	freeDeferred();
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
