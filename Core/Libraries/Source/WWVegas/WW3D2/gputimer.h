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

// EastWind DX9: GPU time of each part of a frame, for the debug status (EwDebugKeys = yes).
//
// W3DDisplay and EwPostProcess call Mark() at fixed points of the frame. Each mark issues a
// D3D9 timestamp query; the time of a part is the GPU time from the previous mark that ran to
// its own. Results are read a few frames later without waiting (a ring of query sets) and
// averaged over 60 frames. Frames the driver marks disjoint (clock change) are thrown away.
// Timestamps measure when the GPU reaches each mark, so a part also counts time the GPU sat
// waiting for the CPU to send its commands.

#pragma once

class EwGpuTimer
{
public:
	// In frame order. A part is named by the mark that ends it.
	enum Point
	{
		FRAME_BEGIN,		// start of the frame, before the sun shadow map
		SHADOW_MAP,			// sun shadow map
		RENDER_TARGETS,		// water reflection and projected shadow textures
		// The main 3D scene in parts (RTS3DScene). These count only between RENDER_TARGETS and
		// SCENE, so the shadow map and reflection passes, which draw the same code, are left out.
		SCENE_TERRAIN,		// the clear and the terrain (roads, shroud)
		SCENE_MESHES,		// opaque units, buildings and props, decal shadows
		SCENE_TREES,		// trees and stencil shadows
		SCENE_WATER,		// water (the static sort lists)
		SCENE_PARTICLES,	// translucent meshes, particles and other sorted triangles
		SCENE,				// the rest of the 3D scene into the post-processing texture
		AO,					// ambient occlusion and its blur
		BLOOM,				// bloom bright pass, downsamples and blurs
		FINAL,				// final pass (or the copy) onto the back buffer
		FXAA,				// FXAA
		FRAME_END,			// the rest: UI, and the 3D scene too when post-processing is off
		POINT_COUNT
	};

	// Issues the mark's timestamp; only the first of each mark in a frame counts. FRAME_BEGIN
	// starts a frame and FRAME_END closes it. Does nothing unless EwGraphics().DebugKeys.
	static void Mark(Point point);

	// The last finished 60-frame average, in milliseconds. part_ms[p] is the average of mark p's
	// part over the frames it ran in, or negative if it never ran; part_ms[FRAME_BEGIN] is unused.
	// Returns the number of frames averaged: 0 while none finished, -1 if the device has no
	// timestamp queries.
	static int Average(float *total_ms, float part_ms[POINT_COUNT]);

	// Releases the queries. Called before a device reset or release.
	static void Release();
};
