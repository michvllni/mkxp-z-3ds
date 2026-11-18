// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: the GL function table (`gl`, src/display/gl/gl-fun.h) for the Nintendo 3DS; replaces
// src/display/gl/gl-fun.cpp. mkxp-z's display classes draw only through this table (via its
// wrapper layer: TEX/FBO, buffers, Quad, ShaderSet, GLState), so the 3DS renderer is an
// implementation of the GL ES 2 subset in this table (docs/DECISIONS.md D14 in rmxp3ds).
//
// This version keeps the bookkeeping only (object names, shader/program status, uniform
// locations, capabilities): nothing is drawn and ReadPixels returns transparent black. It lets
// the engine run scripts headlessly; the citro3d implementation replaces the function bodies.
#include "gl-fun.h"

#include <string.h>

#include <map>
#include <string>

GLFunctions gl;

namespace {
GLuint nextName = 1; // shared counter for every object kind

// Gives n fresh names. Names are never reused, so a stale name can never alias a new object.
void genNames(GLsizei n, GLuint *out) {
	for (GLsizei i = 0; i < n; i++) out[i] = nextName++;
}

// Uniform locations per program: name -> location (stable per program and name).
std::map<GLuint, std::map<std::string, GLint>> uniformLocations;

GLenum APIENTRY getError() { return GL_NO_ERROR; }
void APIENTRY clearColor(GLclampf, GLclampf, GLclampf, GLclampf) {}
void APIENTRY clear(GLbitfield) {}

const GLubyte *APIENTRY getString(GLenum name) {
	switch (name) {
	case GL_VENDOR: return (const GLubyte *)"mkxp-z-3ds";
	case GL_RENDERER: return (const GLubyte *)"PICA200 (headless)";
	case GL_VERSION: return (const GLubyte *)"OpenGL ES 2.0 (3DS subset)";
	case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00 (not compiled)";
	case GL_EXTENSIONS: return (const GLubyte *)"";
	default: return (const GLubyte *)"";
	}
}

void APIENTRY getIntegerv(GLenum pname, GLint *params) {
	switch (pname) {
	// The PICA200 limit is 1024 px, but mkxp-z sizes its tileset atlas by this value: with 1024,
	// tilesets taller than about 1300 px are rejected. The table reports a desktop-like limit;
	// the citro3d implementation stores larger textures as several <= 1024 px pieces.
	case GL_MAX_TEXTURE_SIZE: *params = 4096; break;
	case GL_MAX_TEXTURE_IMAGE_UNITS: *params = 3; break;
	case GL_MAX_VERTEX_ATTRIBS: *params = 12; break;
	case GL_VIEWPORT: params[0] = params[1] = 0; params[2] = 400; params[3] = 240; break;
	default: *params = 0; break;
	}
}

void APIENTRY pixelStorei(GLenum, GLint) {}

void APIENTRY readPixels(GLint, GLint, GLsizei w, GLsizei h, GLenum format, GLenum, GLvoid *pixels) {
	size_t bpp = (format == GL_RGBA || format == GL_BGRA) ? 4 : 3;
	memset(pixels, 0, (size_t)w * (size_t)h * bpp);
}

void APIENTRY enable(GLenum) {}
void APIENTRY disable(GLenum) {}
void APIENTRY scissor(GLint, GLint, GLsizei, GLsizei) {}
void APIENTRY viewport(GLint, GLint, GLsizei, GLsizei) {}
void APIENTRY blendFunc(GLenum, GLenum) {}
void APIENTRY blendFuncSeparate(GLenum, GLenum, GLenum, GLenum) {}
void APIENTRY blendEquation(GLenum) {}
void APIENTRY drawElements(GLenum, GLsizei, GLenum, const GLvoid *) {}

void APIENTRY genTextures(GLsizei n, GLuint *t) { genNames(n, t); }
void APIENTRY deleteTextures(GLsizei, const GLuint *) {}
void APIENTRY bindTexture(GLenum, GLuint) {}
void APIENTRY texImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *) {}
void APIENTRY texSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *) {}
void APIENTRY texParameteri(GLenum, GLenum, GLint) {}
void APIENTRY activeTexture(GLenum) {}
void APIENTRY generateMipmap(GLenum) {}

void APIENTRY genBuffers(GLsizei n, GLuint *b) { genNames(n, b); }
void APIENTRY deleteBuffers(GLsizei, const GLuint *) {}
void APIENTRY bindBuffer(GLenum, GLuint) {}
void APIENTRY bufferData(GLenum, GLsizeiptr, const GLvoid *, GLenum) {}
void APIENTRY bufferSubData(GLenum, GLintptr, GLsizeiptr, const GLvoid *) {}

GLuint APIENTRY createShader(GLenum) { return nextName++; }
void APIENTRY deleteShader(GLuint) {}
void APIENTRY shaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *) {}
void APIENTRY compileShader(GLuint) {}
void APIENTRY attachShader(GLuint, GLuint) {}

// Shaders and programs always "compile" and "link": the 3DS renderer maps mkxp-z's known
// programs onto fixed GPU configurations instead of compiling GLSL.
void APIENTRY getShaderiv(GLuint, GLenum pname, GLint *params) {
	*params = (pname == GL_COMPILE_STATUS) ? GL_TRUE : 0;
}
void APIENTRY getShaderInfoLog(GLuint, GLsizei, GLsizei *length, GLchar *log) {
	if (length) *length = 0;
	if (log) log[0] = 0;
}

GLuint APIENTRY createProgram() { return nextName++; }
void APIENTRY deleteProgram(GLuint program) { uniformLocations.erase(program); }
void APIENTRY useProgram(GLuint) {}
void APIENTRY linkProgram(GLuint) {}
void APIENTRY getProgramiv(GLuint, GLenum pname, GLint *params) {
	*params = (pname == GL_LINK_STATUS) ? GL_TRUE : 0;
}
void APIENTRY getProgramInfoLog(GLuint, GLsizei, GLsizei *length, GLchar *log) {
	if (length) *length = 0;
	if (log) log[0] = 0;
}

GLint APIENTRY getUniformLocation(GLuint program, const GLchar *name) {
	auto &locs = uniformLocations[program];
	auto it = locs.find(name);
	if (it != locs.end()) return it->second;
	GLint loc = (GLint)locs.size();
	locs[name] = loc;
	return loc;
}
void APIENTRY uniform1f(GLint, GLfloat) {}
void APIENTRY uniform2f(GLint, GLfloat, GLfloat) {}
void APIENTRY uniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat) {}
void APIENTRY uniform1i(GLint, GLint) {}
void APIENTRY uniform1iv(GLint, GLsizei, const GLint *) {}
void APIENTRY uniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat *) {}
void APIENTRY bindAttribLocation(GLuint, GLuint, const GLchar *) {}
void APIENTRY enableVertexAttribArray(GLuint) {}
void APIENTRY disableVertexAttribArray(GLuint) {}
void APIENTRY vertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *) {}
void APIENTRY releaseShaderCompiler() {}

void APIENTRY genFramebuffers(GLsizei n, GLuint *f) { genNames(n, f); }
void APIENTRY deleteFramebuffers(GLsizei, const GLuint *) {}
void APIENTRY bindFramebuffer(GLenum, GLuint) {}
void APIENTRY framebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}

void APIENTRY genVertexArrays(GLsizei n, GLuint *a) { genNames(n, a); }
void APIENTRY deleteVertexArrays(GLsizei, const GLuint *) {}
void APIENTRY bindVertexArray(GLuint) {}
} // namespace

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
}
