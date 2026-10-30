/*
**	EastWind: Command & Conquer Generals Zero Hour engine fork
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// EastWind DX9: per-pixel terrain lighting (M3.1c, Options.ini PerPixelLighting).
//
// HeightMapRenderObjClass::Render calls Begin() after W3DShaderManager::setShader has set up a
// terrain base pass, and End() after the pass has drawn its vertex buffer tiles. When the pass
// uses one of the terrain parity pixel shaders (terrain.hlsl), Begin() swaps in terrain_lit.hlsl,
// which lights the sun part per pixel and keeps the map and dynamic lights per vertex (see the
// top of terrain_lit.hlsl).
//
// Begin() keeps a texture of the grid normals of the whole height map, rebuilt when the heights
// change. Anything the shader does not handle (no vs_3_0/ps_3_0, no vertex texture fetch, clip
// planes, non-linear fog) leaves the pass as it was, and the reason is logged once.
//
// M3.1d: W3DRoadBuffer::drawRoads and HeightMapRenderObjClass::renderExtraBlendTiles call
// Begin_Road() after the road shader has set up its pass, which lights roads and the three-way
// blend tiles the same way. Shorelines need nothing: they write only destination alpha (how
// the water fades in at the shore), no colour, so they have no light to change.
// W3DTreeBuffer::drawTrees binds its own lit shaders (trees.hlsl) and reports here with
// Tree_Pass(), so the debug status shows trees too.

#pragma once

class WorldHeightMap;

class W3DTerrainLighting
{
public:
	// Binds the per-pixel terrain shaders for the pass the terrain shader just set up.
	// Returns true if it did; the caller then calls End() after the pass.
	static bool Begin(WorldHeightMap *map);

	// The same for a road or blend tile pass that W3DShaderManager set up with one of the
	// ST_ROAD_BASE* shaders: noise_textures is 0 for ST_ROAD_BASE, 1 for NOISE1 and NOISE2, 2 for
	// NOISE12; passes is what getShaderPasses returned. On true the caller draws the pass once
	// and calls End().
	static bool Begin_Road(WorldHeightMap *map, int noise_textures, int passes);

	static void End();

	// Frees the normal texture. Called from W3DShaderManager::shutdown, which also runs before
	// a device reset.
	static void Release();

	// For the debug keys (DebugKeys): W3DDisplay::draw calls New_Frame() once a frame.
	// Last_Frame_Passes() is how many terrain passes of the last frame used the shader, and
	// Last_Reason() why the last pass that did not fell back.
	static void New_Frame();
	static unsigned Last_Frame_Passes();
	static const char *Last_Reason();
	// The same for the road and blend tile passes.
	static unsigned Last_Frame_Road_Passes();
	static const char *Last_Road_Reason();
	// Trees: drawTrees calls Tree_Pass() once a draw, with null when the lit tree shaders drew
	// and else why not.
	static void Tree_Pass(const char *fallback_reason);
	static unsigned Last_Frame_Tree_Passes();
	static const char *Last_Tree_Reason();
};
