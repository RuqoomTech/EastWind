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

// EastWind DX9 shader store.
//
// Creates each EastWind shader once from the bytecode fxc embedded at build time
// (EastWind/Shaders/*.hlsl, compiled by cmake/eastwind_shaders.cmake). D3D9Ex keeps shaders
// across device resets, so they live until the device is released.
// The register layout the shaders expect is in EastWind/Shaders/ew_constants.h.

#pragma once

struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;

class EwShaders
{
public:
	enum VertexShaderId
	{
		VS_TREES,		// trees.hlsl, was Trees.vso
		VS_WAVE,		// wave.hlsl, was wave.vso
		VS_COUNT
	};

	enum PixelShaderId
	{
		PS_WAVE,					// wave.hlsl, was wave.pso
		PS_RIVER,					// water.hlsl, was assembled at runtime in W3DWater.cpp
		PS_TRAPEZOID,				// water.hlsl, was assembled at runtime in W3DWater.cpp
		PS_TERRAIN,					// terrain.hlsl, was terrain.pso
		PS_TERRAIN_NOISE1,			// terrain.hlsl, was terrainnoise.pso
		PS_TERRAIN_NOISE2,			// terrain.hlsl, was terrainnoise2.pso
		PS_ROAD_NOISE2,				// terrain.hlsl, was roadnoise2.pso
		PS_FLAT_TERRAIN,			// terrain.hlsl, was fterrain.pso
		PS_FLAT_TERRAIN0,			// terrain.hlsl, was fterrain0.pso
		PS_FLAT_TERRAIN_NOISE1,		// terrain.hlsl, was fterrainnoise.pso
		PS_FLAT_TERRAIN_NOISE2,		// terrain.hlsl, was fterrainnoise2.pso
		PS_MONOCHROME,				// monochrome.hlsl, was monochrome.pso
		PS_PROFILER_SWIZZLE,		// profiler.hlsl, was assembled at runtime in W3DProfilerFrameCapture.cpp
		PS_COUNT
	};

	// Returns the shader with a reference added, which the caller releases, so code that
	// used to create its own shader keeps its Release calls. Null if the device has no
	// support for the shader's profile or creation failed.
	static IDirect3DVertexShader9 *Vertex(VertexShaderId id);
	static IDirect3DPixelShader9 *Pixel(PixelShaderId id);

	// Releases the store's references. Called before the device is released.
	static void Shutdown();
};
