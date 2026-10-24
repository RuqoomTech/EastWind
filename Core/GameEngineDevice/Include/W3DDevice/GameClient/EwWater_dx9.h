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

// EastWind DX9: the water shader (M3.3a, Options.ini EwWaterQuality = 1).
//
// WaterRenderObjClass::drawTrapezoidWater and drawRiverWater call Begin() right before they draw,
// after the pass is set up with the parity water pixel shader (water.hlsl TrapezoidPS/RiverPS),
// and End() after the draw. Begin() swaps in water.hlsl WaterVS with WaterTrapezoidPS or
// WaterRiverPS, which keep the classic textures and add:
// - waves: two layers of a tiling normal map, made here from a sum of sine waves, scrolling in
//   different directions. Rivers lay them out along the river so they follow the flow;
// - Fresnel: more reflection where the view is flat to the water;
// - reflection: the scene mirrored about the water, drawn into a texture before the frame
//   (Begin_Reflection), read at the pixel's screen position and moved by the waves;
// - the sun's specular highlight on the waves.
//
// The reflection: before the frame, WaterRenderObjClass::updateRenderTargetTextures picks the
// visible water area nearest the middle of the view and draws the scene mirrored about its
// height into a texture half the size of the back buffer (the full size at EwWaterQuality = 2),
// with its own depth buffer. Water at another height shows a plain sky colour instead.
//
// EwWaterQuality 2 (M3.3b) draws the water seen into instead (water.hlsl WaterTrapezoidHqPS and
// WaterRiverHqPS): the scene behind it, copied before the first water draw of the frame, bent by
// the waves and coloured by how deep the water is, from a texture of the terrain heights; caustics
// in the shallows and foam along the shore, from a cellular noise texture made here. The classic
// water texture gives only the colour of the deep water.
//
// Anything the shader does not handle (no vs_3_0/ps_3_0, clip planes, non-linear fog) leaves
// the pass as it was, and the reason is logged once.

#pragma once

struct IDirect3DSurface9;

class EwWater
{
public:
	enum Kind
	{
		TRAPEZOID,
		RIVER
	};

	// Binds the water shader over the pass that is set up on the device. water_level is the
	// height of the water being drawn; for rivers, river_width is their width in world units and
	// river_alpha the vertex alpha from the INI (0..1) before the shroud scaled it. Returns true if
	// it did; the caller then calls End() after the draw.
	static bool Begin(Kind kind, float water_level, float river_width, float river_alpha);
	static void End();

	// The reflection for this frame. Wants_Reflection() says whether to draw one. Begin_Reflection()
	// creates the texture and depth buffer if needed and returns them (not AddRef'd); the caller
	// draws the mirrored scene into them and calls End_Reflection() with the mirror height.
	static bool Wants_Reflection();
	static bool Begin_Reflection(IDirect3DSurface9 **target, IDirect3DSurface9 **depth);
	static void End_Reflection(float level);
	static void Skip_Reflection(const char *why);

	// The colour the reflection is cleared to, and the water mirrors where there is no reflection:
	// a sky blue lit by the map's ambient and sun.
	static void Sky_Color(float rgb[3]);

	// Frees the textures and surfaces. Called from W3DShaderManager::shutdown, which also runs
	// before a device reset; they are created again when needed.
	static void Release();

	// For the debug keys (EwDebugKeys): W3DDisplay::draw calls New_Frame() once a frame.
	static void New_Frame();
	static unsigned Last_Frame_Passes();	// water draws that used the shader in the last frame
	static const char *Last_Reason();		// why the last draw that did not fell back
	// The reflection of the last frame: true if one was drawn, with its height and size;
	// else why not.
	static bool Last_Reflection(float *level, int *width, int *height, const char **why_not);
	// EwWaterQuality 2: null if the last frame drew water seen into, with terrain heights; else why not.
	static const char *Last_Seen_Into();
};
