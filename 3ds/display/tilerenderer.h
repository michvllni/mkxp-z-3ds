// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tilerenderer.h: native map renderer for Pokémon Essentials v21 games (rmxp3ds D17).
 *
 * Essentials draws maps in Ruby with one Sprite per screen cell and layer. This class draws the
 * same picture natively: the Ruby side (rmxp3ds patch set) keeps Essentials' TilemapRenderer
 * interface and passes, per frame, the maps on screen with their tile tables; TileRenderer
 * resolves tile ids, z values and autotile frames and draws the tiles in batches.
 *
 * Tiles live in an atlas of 1024x1024 pages with 32x32 slots: tileset tiles are copied, autotiles
 * are composed per frame from their 16x16 pieces (RMXP block autotiles) or copied (single-tile
 * autotiles). No texture is larger than 1024 px, so tall tilesets need no wrapping.
 *
 * Z order matches Essentials' tile sprites: one scene element per z value (reflective water -100,
 * ground 0, and 32*k+1 for prioritised tiles), created before the game's character sprites so
 * that tiles draw first on equal z; within an element layer 0, 1, 2 draw in that order.
 */
#ifndef RMXP3DS_TILERENDERER_H
#define RMXP3DS_TILERENDERER_H

#include "disposable.h"
#include "util.h"

#include <vector>

class Viewport;
class Bitmap;
class Table;
struct Tone;
struct Color;
struct TileRendererPrivate;

class TileRenderer : public Disposable
{
public:
	enum SourceKind { Tileset = 0, Autotile = 1 };

	/* One map on screen, as the Ruby side sees it this frame. */
	struct MapInput
	{
		Table *data = nullptr;   /* map data, x * y * 3 layers */
		Table *flags = nullptr;  /* per tile id: priority (bits 0-2), reflective (bit 3), bridge (bit 4) */
		int tileX = 0;           /* map tile shown in screen column 0: floor(round(display_x / 4) / 32) */
		int tileY = 0;
		int tileset = -1;        /* source handles, -1: none */
		int autotiles[7] = {-1, -1, -1, -1, -1, -1, -1};
		std::vector<int> largeExtras;
		std::vector<int> singleExtras;
	};

	TileRenderer(Viewport *viewport = 0);
	~TileRenderer();

	/* Registers a source image. Bitmaps above 1024 px (CPU mega surfaces) are read in place and
	 * must stay alive until removeSource(); smaller ones are copied. Autotiles:
	 * height 32 is a single-tile autotile (frames side by side), anything else an RMXP block
	 * autotile (96 px per frame; 192 px tall: Essentials' variant with sub-tiles 1, 2, 4, 8
	 * stored whole). 'frameSeconds' is the autotile frame duration. Returns a handle. */
	int addSource(Bitmap *bitmap, SourceKind kind, double frameSeconds);
	void removeSource(int handle);
	int frameCount(int handle) const;

	/* Rebuilds the tiles for this frame. pixelX/Y: sub-tile scroll of the current map (0..31);
	 * bridge: $PokemonGlobal.bridge > 0; seconds: time since the renderer was created. */
	void update(const std::vector<MapInput> &maps, int pixelX, int pixelY, bool bridge, double seconds);

	Viewport *getViewport() const;

	DECL_ATTR( Tone,  Tone& )
	DECL_ATTR( Color, Color& )

	void initDynAttribs();

private:
	TileRendererPrivate *p;

	void releaseResources();
	const char *klassName() const { return "tile renderer"; }
};

#endif // RMXP3DS_TILERENDERER_H
