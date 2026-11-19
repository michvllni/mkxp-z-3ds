// SPDX-License-Identifier: GPL-2.0-or-later
// tilerenderer-binding.cpp: Ruby class RMXP3DSTileRenderer (rmxp3ds D17), the native part of the
// Essentials map renderer. The Ruby side (rmxp3ds patch set) keeps Essentials' TilemapRenderer
// interface and calls:
//   RMXP3DSTileRenderer.new(viewport = nil)
//   add_source(bitmap, kind, frame_seconds) -> handle   kind 0: tileset, 1: autotile
//   remove_source(handle), frame_count(handle)
//   update(maps, pixel_x, pixel_y, bridge, seconds)
//     maps: [[data, flags, tile_x, tile_y, tileset, [7 autotiles], [large extras], [single extras]], ...]
//     (Tables, Integers, handles; nil or -1 for none; later maps win per screen cell)
//   tone, tone=, color, color=, viewport, dispose, disposed?

#include "tilerenderer.h"

#include "binding-types.h"
#include "binding-util.h"
#include "bitmap.h"
#include "disposable-binding.h"
#include "etc.h"
#include "table.h"
#include "viewport.h"

#include <new>

DEF_TYPE(TileRenderer);

/* Runs a native call; std::bad_alloc becomes an MKXPError (raised as a Ruby error by
 * RB_METHOD_GUARD instead of terminating the app). */
template<typename F> static void nativeCall(F f)
{
	try {
		GFX_GUARD_EXC(f(););
	} catch (const std::bad_alloc &) {
		throw Exception(Exception::MKXPError, "tile renderer: out of memory");
	}
}

static Table *tableOrNull(VALUE v)
{
	return NIL_P(v) ? nullptr : getPrivateDataCheck<Table>(v, TableType);
}

static int intOr(VALUE v, int fallback)
{
	return NIL_P(v) ? fallback : NUM2INT(v);
}

static void handleList(VALUE ary, std::vector<int> &out)
{
	out.clear();
	if (NIL_P(ary))
		return;
	Check_Type(ary, T_ARRAY);
	for (long i = 0; i < RARRAY_LEN(ary); ++i)
		out.push_back(intOr(rb_ary_entry(ary, i), -1));
}

RB_METHOD(tileRendererInitialize) {
	VALUE viewportObj = Qnil;
	Viewport *viewport = 0;
	rb_get_args(argc, argv, "|o", &viewportObj RB_ARG_END);
	if (!NIL_P(viewportObj))
		viewport = getPrivateDataCheck<Viewport>(viewportObj, ViewportType);

	GFX_LOCK;
	TileRenderer *r = new TileRenderer(viewport);
	rb_iv_set(self, "viewport", viewportObj);
	setPrivateData(self, r);
	r->initDynAttribs();
	wrapProperty(self, &r->getColor(), "color", ColorType);
	wrapProperty(self, &r->getTone(), "tone", ToneType);
	GFX_UNLOCK;
	return self;
}

// Guarded: C++ Exceptions (disposed renderer or bitmap, out of memory) become Ruby errors.
RB_METHOD_GUARD(tileRendererAddSource) {
	VALUE bitmapObj;
	int kind;
	double seconds;
	rb_get_args(argc, argv, "oif", &bitmapObj, &kind, &seconds RB_ARG_END);
	TileRenderer *r = getPrivateData<TileRenderer>(self);
	Bitmap *b = getPrivateDataCheck<Bitmap>(bitmapObj, BitmapType);
	int handle = -1;
	nativeCall([&] { handle = r->addSource(b, kind == 1 ? TileRenderer::Autotile : TileRenderer::Tileset, seconds); });
	return INT2NUM(handle);
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(tileRendererRemoveSource) {
	int handle;
	rb_get_args(argc, argv, "i", &handle RB_ARG_END);
	TileRenderer *r = getPrivateData<TileRenderer>(self);
	nativeCall([&] { r->removeSource(handle); });
	return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(tileRendererFrameCount) {
	int handle;
	rb_get_args(argc, argv, "i", &handle RB_ARG_END);
	TileRenderer *r = getPrivateData<TileRenderer>(self);
	return INT2NUM(r->frameCount(handle));
}
RB_METHOD_GUARD_END

/* Raises (longjmp) on a malformed map entry; called before any C++ object is built. */
static void validateMaps(VALUE mapsObj)
{
	Check_Type(mapsObj, T_ARRAY);
	for (long i = 0; i < RARRAY_LEN(mapsObj); ++i) {
		VALUE m = rb_ary_entry(mapsObj, i);
		Check_Type(m, T_ARRAY);
		for (int t = 0; t < 2; ++t)
			if (!NIL_P(rb_ary_entry(m, t)))
				getPrivateDataCheck<Table>(rb_ary_entry(m, t), TableType);
		for (int n = 2; n < 5; ++n)
			if (!NIL_P(rb_ary_entry(m, n)))
				(void)NUM2INT(rb_ary_entry(m, n));
		for (int a = 5; a < 8; ++a) {
			VALUE ary = rb_ary_entry(m, a);
			if (NIL_P(ary))
				continue;
			Check_Type(ary, T_ARRAY);
			for (long k = 0; k < RARRAY_LEN(ary); ++k)
				if (!NIL_P(rb_ary_entry(ary, k)))
					(void)NUM2INT(rb_ary_entry(ary, k));
		}
	}
}

RB_METHOD_GUARD(tileRendererUpdate) {
	VALUE mapsObj, bridgeObj;
	int px, py;
	double seconds;
	rb_get_args(argc, argv, "oiiof", &mapsObj, &px, &py, &bridgeObj, &seconds RB_ARG_END);
	TileRenderer *r = getPrivateData<TileRenderer>(self);
	validateMaps(mapsObj);

	std::vector<TileRenderer::MapInput> maps;
	try {
		maps.resize(RARRAY_LEN(mapsObj));
	} catch (const std::bad_alloc &) {
		throw Exception(Exception::MKXPError, "tile renderer: out of memory");
	}
	for (long i = 0; i < RARRAY_LEN(mapsObj); ++i) {
		VALUE m = rb_ary_entry(mapsObj, i);
		TileRenderer::MapInput &in = maps[i];
		in.data = tableOrNull(rb_ary_entry(m, 0));
		in.flags = tableOrNull(rb_ary_entry(m, 1));
		in.tileX = intOr(rb_ary_entry(m, 2), 0);
		in.tileY = intOr(rb_ary_entry(m, 3), 0);
		in.tileset = intOr(rb_ary_entry(m, 4), -1);
		std::vector<int> at;
		handleList(rb_ary_entry(m, 5), at);
		for (int a = 0; a < 7; ++a)
			in.autotiles[a] = (a < (int)at.size()) ? at[a] : -1;
		handleList(rb_ary_entry(m, 6), in.largeExtras);
		handleList(rb_ary_entry(m, 7), in.singleExtras);
	}
	nativeCall([&] { r->update(maps, px, py, RTEST(bridgeObj), seconds); });
	return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD(tileRendererGetViewport) {
	RB_UNUSED_PARAM;
	checkDisposed<TileRenderer>(self);
	return rb_iv_get(self, "viewport");
}

DEF_GFX_PROP_OBJ_VAL(TileRenderer, Color, Color, "color")
DEF_GFX_PROP_OBJ_VAL(TileRenderer, Tone, Tone, "tone")

void rmxp3dsTileRendererBindingInit()
{
	VALUE klass = rb_define_class("RMXP3DSTileRenderer", rb_cObject);
	rb_define_alloc_func(klass, classAllocate<&TileRendererType>);
	disposableBindingInit<TileRenderer>(klass);
	_rb_define_method(klass, "initialize", tileRendererInitialize);
	_rb_define_method(klass, "add_source", tileRendererAddSource);
	_rb_define_method(klass, "remove_source", tileRendererRemoveSource);
	_rb_define_method(klass, "frame_count", tileRendererFrameCount);
	_rb_define_method(klass, "update", tileRendererUpdate);
	_rb_define_method(klass, "viewport", tileRendererGetViewport);
	INIT_PROP_BIND(TileRenderer, Color, "color");
	INIT_PROP_BIND(TileRenderer, Tone, "tone");
}
