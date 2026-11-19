// SPDX-License-Identifier: GPL-2.0-or-later
// tilerenderer.cpp: native map renderer for Pokémon Essentials v21 games (rmxp3ds D17), see
// tilerenderer.h. The behaviour reproduces Essentials' Ruby TilemapRenderer: screen grid of
// (W/32+1) x (H/32+1) cells with 3 layers, later maps win per cell, z per screen row.

#include "tilerenderer.h"

#include "bitmap.h"
#include "etc.h"
#include "etc-internal.h"
#include "gl-util.h"
#include "glstate.h"
#include "graphics.h"
#include "quad.h"
#include "quadarray.h"
#include "shader.h"
#include "sharedstate.h"
#include "table.h"
#include "transform.h"
#include "viewport.h"

#include <SDL_surface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include "exception.h"

/* RMXP autotile pieces: 4 source rects (16x16, in a 96x128 frame) per sub-tile 0..47
 * (src/display/autotiles.cpp; checked identical to Essentials' AUTOTILE_PATTERNS). */
extern const StaticRect autotileRects[];

namespace
{
const int TILE = 32;
const int PAGE = 1024;
const int SLOTS_PER_ROW = PAGE / TILE;
const int SLOTS_PER_PAGE = SLOTS_PER_ROW * SLOTS_PER_ROW;
const int AUTOTILE_IDS = 48;
const int TILESET_START = 384;
const int MAX_PRIORITY = 7;

/* Floor division (Ruby semantics; display coordinates can be negative). */
int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }


struct Source
{
	bool used = false;
	bool autotile = false;
	bool single = false;     /* single-tile autotile (frames 32 px wide) */
	bool expanded = false;   /* Essentials' 192 px tall block autotile */
	int frames = 1;
	double frameSeconds = 0.25;
	/* tileset: n -> slot; block: f*48+k -> slot; single: f -> slot; -1 until first drawn */
	std::vector<int> slots;
	/* Source image (RGBA8): tiles are composed and uploaded to the atlas on first use, so only
	 * the tiles a map shows take texture memory. Mega bitmaps (above 1024 px, CPU surfaces) are
	 * read in place: the caller keeps the bitmap alive until removeSource. Other bitmaps are
	 * copied (they are GPU textures, at most 1024x1024). */
	const uint8_t *data = nullptr;
	int pitch = 0;
	std::vector<uint8_t> own;
	int width = 0, height = 0;
	/* mega bitmaps read in place: their disposal clears 'data' (tiles not yet in the atlas
	 * then draw blank instead of reading freed memory) */
	sigslot::connection disposedCon;
};

/* Copies a w x h block at (sx, sy) of an RGBA8 image into a TILE x TILE tile at (dx, dy). */
void blitTile(const Source &s, uint8_t *dst, int dx, int dy, int sx, int sy, int w, int h)
{
	for (int y = 0; y < h; ++y) {
		if (!s.data) {
			memset(dst + ((dy + y) * TILE + dx) * 4, 0, (size_t)w * 4);
			continue;
		}
		uint8_t *d = dst + ((dy + y) * TILE + dx) * 4;
		int yy = sy + y;
		for (int x = 0; x < w; ++x) {
			int xx = sx + x;
			if (xx >= 0 && yy >= 0 && xx < s.width && yy < s.height)
				memcpy(d + x * 4, s.data + (size_t)yy * s.pitch + xx * 4, 4);
			else
				memset(d + x * 4, 0, 4);
		}
	}
}
} // namespace

struct TileLayer;

struct TileRendererPrivate
{
	Viewport *viewport;
	Tone *tone = nullptr;
	Color *color = nullptr;

	std::vector<TEX::ID> pages;
	std::vector<int> freeSlots;   /* slot = page * SLOTS_PER_PAGE + index */
	std::vector<Source> sources;

	int cols, rows;               /* screen grid */
	Scene::Geometry geo;

	TileLayer *reflect = nullptr; /* z -100 */
	TileLayer *ground = nullptr;  /* z 0 */
	std::vector<TileLayer *> rowLayers; /* z 32*k+1, k = index */

	struct Cell { int slot; int z; };
	std::vector<Cell> cells;      /* cols * rows * 3 */

	TileRendererPrivate(Viewport *vp);
	~TileRendererPrivate();

	int allocSlot()
	{
		if (freeSlots.empty()) {
			TEX::ID tex = TEX::gen();
			TEX::bind(tex);
			TEX::setRepeat(false);
			TEX::setSmooth(false);
			TEX::allocEmpty(PAGE, PAGE);
			int page = (int)pages.size();
			pages.push_back(tex);
			for (int i = SLOTS_PER_PAGE - 1; i >= 0; --i)
				freeSlots.push_back(page * SLOTS_PER_PAGE + i);
		}
		int s = freeSlots.back();
		freeSlots.pop_back();
		return s;
	}

	void upload(int slot, const uint8_t *tile)
	{
		int page = slot / SLOTS_PER_PAGE, i = slot % SLOTS_PER_PAGE;
		TEX::bind(pages[page]);
		TEX::uploadSubImage((i % SLOTS_PER_ROW) * TILE, (i / SLOTS_PER_ROW) * TILE, TILE, TILE, tile, GL_RGBA);
	}

	/* Current frame of an autotile source. */
	int frameOf(const Source &s, double seconds) const
	{
		if (s.frames < 2 || s.frameSeconds <= 0)
			return 0;
		long n = (long)std::floor(seconds / s.frameSeconds);
		return (int)(((n % s.frames) + s.frames) % s.frames);
	}

	/* Slot of a tile id on a map, or -1 (composes and uploads the tile on first use). */
	int resolve(const TileRenderer::MapInput &m, int id, double seconds);

	/* Atlas slot of tile 'index' of a source (see Source::slots), uploading it if needed. */
	int tileSlot(Source &s, int index)
	{
		if (index < 0 || index >= (int)s.slots.size())
			return -1;
		if (s.slots[index] >= 0)
			return s.slots[index];
		if (s.autotile) {
			/* all frames of this sub-tile at once: an animated tile then never uploads into a
			 * page again while it cycles */
			int per = s.single ? 1 : AUTOTILE_IDS;
			int k = index % per, slot = -1;
			for (int f = 0; f < s.frames; ++f) {
				int at = f * per + k;
				if (s.slots[at] < 0)
					s.slots[at] = composeTile(s, at);
				if (at == index)
					slot = s.slots[at];
			}
			return slot;
		}
		s.slots[index] = composeTile(s, index);
		return s.slots[index];
	}

	/* Composes tile 'index' of a source into a new atlas slot. */
	int composeTile(const Source &s, int index)
	{
		std::vector<uint8_t> tile(TILE * TILE * 4);
		if (!s.autotile) {
			blitTile(s, tile.data(), 0, 0, (index % 8) * TILE, (index / 8) * TILE, TILE, TILE);
		} else if (s.single) {
			blitTile(s, tile.data(), 0, 0, index * TILE, 0, TILE, TILE);
		} else {
			int f = index / AUTOTILE_IDS, k = index % AUTOTILE_IDS, fx = f * 96;
			if (s.expanded && (k == 1 || k == 2 || k == 4 || k == 8)) {
				int sx = fx + ((k == 2 || k == 4) ? 32 : 0);
				int sy = (k == 1 || k == 2) ? 128 : 160;
				blitTile(s, tile.data(), 0, 0, sx, sy, TILE, TILE);
			} else {
				const StaticRect *piece = &autotileRects[k * 4];
				for (int q = 0; q < 4; ++q)
					blitTile(s, tile.data(), (q % 2) * 16, (q / 2) * 16, fx + piece[q].x, piece[q].y, 16, 16);
			}
		}
		int slot = allocSlot();
		upload(slot, tile.data());
		return slot;
	}
};

struct TileLayer : public ViewportElement
{
	TileRendererPrivate *p;
	SimpleQuadArray quads;
	struct Range { int page; size_t offset, count; };
	std::vector<Range> ranges;
	/* per layer 0..2: (slot, x, y), sorted by page before building quads */
	std::vector<std::array<int, 3>> pending[3];

	TileLayer(TileRendererPrivate *p, Viewport *vp, int z) : ViewportElement(vp, z), p(p)
	{
		onGeometryChange(scene->getGeometry());
	}

	void aboutToAccess() const {}

	void onGeometryChange(const Scene::Geometry &geo) { p->geo = geo; }

	void build()
	{
		quads.clear();
		ranges.clear();
		size_t total = pending[0].size() + pending[1].size() + pending[2].size();
		quads.resize(total);
		size_t q = 0;
		for (int l = 0; l < 3; ++l) {
			std::vector<std::array<int, 3>> &v = pending[l];
			std::stable_sort(v.begin(), v.end(), [](const std::array<int, 3> &a, const std::array<int, 3> &b) {
				return a[0] / SLOTS_PER_PAGE < b[0] / SLOTS_PER_PAGE;
			});
			for (const std::array<int, 3> &t : v) {
				int page = t[0] / SLOTS_PER_PAGE, i = t[0] % SLOTS_PER_PAGE;
				FloatRect tex((i % SLOTS_PER_ROW) * TILE, (i / SLOTS_PER_ROW) * TILE, TILE, TILE);
				FloatRect pos(t[1], t[2], TILE, TILE);
				Quad::setTexPosRect(&quads.vertices[q * 4], tex, pos);
				if (!ranges.empty() && ranges.back().page == page && ranges.back().offset + ranges.back().count == q)
					ranges.back().count++;
				else
					ranges.push_back({page, q, 1});
				++q;
			}
			v.clear();
		}
		if (total)
			quads.commit();
	}

	void draw()
	{
		if (ranges.empty())
			return;

		ShaderBase *base;
		Vec2i offset = p->geo.offset();
		bool effect = p->tone->hasEffect() || p->color->hasEffect();
		if (effect) {
			SpriteShader &shader = shState->shaders().sprite;
			shader.bind();
			shader.applyViewportProj();
			Transform trans;
			trans.setGlobalOffset(offset);
			shader.setSpriteMat(trans.getMatrix());
			shader.setTone(p->tone->norm);
			shader.setColor(p->color->norm);
			shader.setOpacity(1.0f);
			shader.setBushDepth(false, false, 0, 0);
			shader.setBushOpacity(1.0f);
			shader.setShouldRenderPattern(false);
			shader.setInvert(false);
			base = &shader;
		} else {
			SimpleShader &shader = shState->shaders().simple;
			shader.bind();
			shader.applyViewportProj();
			shader.setTranslation(offset);
			base = &shader;
		}
		base->setTexSize(Vec2i(PAGE, PAGE));
		glState.blendMode.pushSet(BlendNormal);
		for (const Range &r : ranges) {
			TEX::bind(p->pages[r.page]);
			quads.draw(r.offset, r.count);
		}
		glState.blendMode.pop();
	}
};

TileRendererPrivate::TileRendererPrivate(Viewport *vp) : viewport(vp)
{
	int w = shState->graphics().width(), h = shState->graphics().height();
	cols = (w + TILE - 1) / TILE + 1;
	rows = (h + TILE - 1) / TILE + 1;
	cells.resize((size_t)cols * rows * 3);
	reflect = new TileLayer(this, vp, -100);
	ground = new TileLayer(this, vp, 0);
	for (int k = 0; k <= rows + MAX_PRIORITY + 1; ++k)
		rowLayers.push_back(new TileLayer(this, vp, TILE * k + 1));
}

TileRendererPrivate::~TileRendererPrivate()
{
	for (Source &s : sources)
		s.disposedCon.disconnect();
	delete reflect;
	delete ground;
	for (TileLayer *l : rowLayers)
		delete l;
	for (TEX::ID t : pages)
		TEX::del(t);
}

int TileRendererPrivate::resolve(const TileRenderer::MapInput &m, int id, double seconds)
{
	if (id < AUTOTILE_IDS)
		return -1;
	int handle = -1, sub = id % AUTOTILE_IDS;
	int large = (int)m.largeExtras.size(), single = (int)m.singleExtras.size();
	int singleStart = TILESET_START + AUTOTILE_IDS * large, trueStart = singleStart + single;
	if (id < TILESET_START)
		handle = m.autotiles[id / AUTOTILE_IDS - 1];
	else if (id < singleStart)
		handle = m.largeExtras[(id - TILESET_START) / AUTOTILE_IDS];
	else if (id < trueStart)
		handle = m.singleExtras[id - singleStart];
	else {
		/* tileset tile: index from 384, extra autotiles cover the first tiles */
		if (m.tileset < 0 || m.tileset >= (int)sources.size() || !sources[m.tileset].used)
			return -1;
		return tileSlot(sources[m.tileset], id - TILESET_START);
	}
	if (handle < 0 || handle >= (int)sources.size() || !sources[handle].used)
		return -1;
	Source &s = sources[handle];
	if (!s.autotile || s.slots.empty())
		return -1;
	int f = frameOf(s, seconds);
	return tileSlot(s, s.single ? f : f * AUTOTILE_IDS + sub);
}

TileRenderer::TileRenderer(Viewport *viewport)
{
	p = new TileRendererPrivate(viewport);
	initDynAttribs();
}

TileRenderer::~TileRenderer()
{
	dispose();
}

void TileRenderer::initDynAttribs()
{
	if (!p->tone) p->tone = new Tone;
	if (!p->color) p->color = new Color;
}

Viewport *TileRenderer::getViewport() const { return p->viewport; }

Tone &TileRenderer::getTone() const { guardDisposed(); return *p->tone; }
Color &TileRenderer::getColor() const { guardDisposed(); return *p->color; }
void TileRenderer::setTone(Tone &value) { guardDisposed(); *p->tone = value; }
void TileRenderer::setColor(Color &value) { guardDisposed(); *p->color = value; }

int TileRenderer::addSource(Bitmap *bitmap, SourceKind kind, double frameSeconds)
{
	guardDisposed();
	Source s;
	s.used = true;
	s.autotile = (kind == Autotile);
	s.frameSeconds = frameSeconds;
	s.width = bitmap->width();
	s.height = bitmap->height();
	bool inPlace = bitmap->isMega() && bitmap->megaSurface();
	if (inPlace) {
		SDL_Surface *surf = bitmap->megaSurface();
		s.data = static_cast<const uint8_t *>(surf->pixels);
		s.pitch = surf->pitch;
	} else {
		try {
			s.own.resize((size_t)s.width * s.height * 4);
		} catch (const std::bad_alloc &) {
			throw Exception(Exception::MKXPError, "tile renderer: out of memory for a %dx%d image",
			                s.width, s.height);
		}
		if (!bitmap->getRaw(s.own.data(), (int)s.own.size()))
			std::fill(s.own.begin(), s.own.end(), 0);
		s.data = s.own.data();
		s.pitch = s.width * 4;
	}

	if (!s.autotile) {
		/* tileset: 8 tiles per row (columns beyond 8 are never addressed) */
		s.slots.assign((size_t)((s.height + TILE - 1) / TILE) * 8, -1); /* partial last row too */
	} else if (s.height == TILE) {
		s.single = true;
		s.frames = std::max(s.width / TILE, 1);
		s.slots.assign(s.frames, -1);
	} else {
		s.frames = std::max(s.width / 96, 1);
		s.expanded = (s.height == 192);
		s.slots.assign((size_t)s.frames * AUTOTILE_IDS, -1);
	}

	int handle = -1;
	for (size_t i = 0; i < p->sources.size() && handle < 0; ++i)
		if (!p->sources[i].used)
			handle = (int)i;
	if (handle < 0) {
		handle = (int)p->sources.size();
		p->sources.emplace_back();
	}
	p->sources[handle] = std::move(s);
	if (inPlace) {
		TileRendererPrivate *priv = p;
		p->sources[handle].disposedCon = bitmap->wasDisposed.connect([priv, handle] {
			priv->sources[handle].data = nullptr;
		});
	}
	return handle;
}

void TileRenderer::removeSource(int handle)
{
	guardDisposed();
	if (handle < 0 || handle >= (int)p->sources.size() || !p->sources[handle].used)
		return;
	Source &s = p->sources[handle];
	s.disposedCon.disconnect();
	for (int slot : s.slots)
		if (slot >= 0)
			p->freeSlots.push_back(slot);
	s = Source();
}

int TileRenderer::frameCount(int handle) const
{
	if (!p)
		return 0; /* disposed */
	if (handle < 0 || handle >= (int)p->sources.size() || !p->sources[handle].used)
		return 0;
	return p->sources[handle].frames;
}

void TileRenderer::update(const std::vector<MapInput> &maps, int pixelX, int pixelY, bool bridge, double seconds)
{
	guardDisposed();
	const int cols = p->cols, rows = p->rows;
	std::fill(p->cells.begin(), p->cells.end(), TileRendererPrivate::Cell{-1, 0});

	for (const MapInput &m : maps) {
		if (!m.data)
			continue;
		const int mw = m.data->xSize(), mh = m.data->ySize(), mz = m.data->zSize();
		int sx = std::max(-m.tileX, 0), ex = std::min(cols - 1, mw - m.tileX - 1);
		int sy = std::max(-m.tileY, 0), ey = std::min(rows - 1, mh - m.tileY - 1);
		if (sx > ex || sy > ey)
			continue;
		for (int j = sy; j <= ey; ++j)
			for (int i = sx; i <= ex; ++i)
				for (int l = 0; l < 3; ++l) {
					TileRendererPrivate::Cell &c = p->cells[((size_t)j * cols + i) * 3 + l];
					c.slot = -1;
					c.z = 0;
					if (l >= mz)
						continue;
					int id = m.data->get(i + m.tileX, j + m.tileY, l);
					int slot = p->resolve(m, id, seconds);
					if (slot < 0)
						continue;
					int f = (m.flags && id >= 0 && id < m.flags->xSize()) ? m.flags->get(id) : 0;
					int prio = std::min(f & 7, MAX_PRIORITY);
					c.slot = slot;
					if (f & 8)
						c.z = -100;
					else if ((f & 16) && bridge)
						c.z = 0;
					else if (prio == 0)
						c.z = 0;
					else
						c.z = TILE * (j + 1 + prio) + 1;
				}
	}

	for (int j = 0; j < rows; ++j)
		for (int i = 0; i < cols; ++i)
			for (int l = 0; l < 3; ++l) {
				const TileRendererPrivate::Cell &c = p->cells[((size_t)j * cols + i) * 3 + l];
				if (c.slot < 0)
					continue;
				TileLayer *layer;
				if (c.z == -100)
					layer = p->reflect;
				else if (c.z == 0)
					layer = p->ground;
				else {
					size_t k = (size_t)(c.z - 1) / TILE;
					if (k >= p->rowLayers.size())
						k = p->rowLayers.size() - 1;
					layer = p->rowLayers[k];
				}
				layer->pending[l].push_back({c.slot, i * TILE - pixelX, j * TILE - pixelY});
			}

	p->reflect->build();
	p->ground->build();
	for (TileLayer *l : p->rowLayers)
		l->build();
}

void TileRenderer::releaseResources()
{
	/* tone and color belong to their Ruby wrapper objects (as in Tilemap) */
	delete p;
	p = nullptr;
}
