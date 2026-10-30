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
// The register layout the shaders expect is in EastWind/Shaders/shader_constants.h.

#pragma once

#include <d3d9.h>

class EwShaders
{
public:
	enum VertexShaderId
	{
		VS_TREES,		// trees.hlsl, was Trees.vso
		VS_WAVE,		// wave.hlsl, was wave.vso

		// mesh.hlsl lit-mesh variants, ordered so Mesh_Vertex_Id() can compute the id:
		// N normal only, ND + diffuse, NS + specular, NDS both; T0..T2 texcoord sets.
		VS_MESH_N_T0,
		VS_MESH_ND_T0,
		VS_MESH_NS_T0,
		VS_MESH_NDS_T0,
		VS_MESH_N_T1,
		VS_MESH_ND_T1,
		VS_MESH_NS_T1,
		VS_MESH_NDS_T1,
		VS_MESH_N_T2,
		VS_MESH_ND_T2,
		VS_MESH_NS_T2,
		VS_MESH_NDS_T2,
		VS_TERRAIN_LIT,		// terrain_lit.hlsl, per-pixel lit terrain (M3.1c)
		VS_ROAD_LIT,		// terrain_lit.hlsl, per-pixel lit roads and blend tiles (M3.1d)
		VS_TREES_LIT,		// trees.hlsl, per-pixel lit trees (M3.1d), replaces VS_TREES
		VS_POST,			// post.hlsl, screen quad for the post-processing passes (M3.2)
		VS_WATER,			// water.hlsl, trapezoid and river water with waves and reflection (M3.3a)
		VS_PARTICLE,		// mesh.hlsl, soft particles (EwSoftParticles)
		// mesh.hlsl variants without a normal, for unlit draws, in the order of the lit ones.
		VS_MESH_T0,
		VS_MESH_D_T0,
		VS_MESH_S_T0,
		VS_MESH_DS_T0,
		VS_MESH_T1,
		VS_MESH_D_T1,
		VS_MESH_S_T1,
		VS_MESH_DS_T1,
		VS_MESH_T2,
		VS_MESH_D_T2,
		VS_MESH_S_T2,
		VS_MESH_DS_T2,
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
		PS_MESH,					// mesh.hlsl, lit meshes (M3.1a)
		PS_MESH_MATERIAL,			// mesh.hlsl, lit meshes with normal map and SpecMap (M3.1b)
		PS_TERRAIN_LIT,				// terrain_lit.hlsl, per-pixel lit terrain (M3.1c), replaces PS_TERRAIN
		PS_TERRAIN_LIT_NOISE1,		// terrain_lit.hlsl, replaces PS_TERRAIN_NOISE1
		PS_TERRAIN_LIT_NOISE2,		// terrain_lit.hlsl, replaces PS_TERRAIN_NOISE2
		PS_ROAD_LIT,				// terrain_lit.hlsl, per-pixel lit roads and blend tiles (M3.1d)
		PS_ROAD_LIT_NOISE1,			// terrain_lit.hlsl, with one noise stage
		PS_ROAD_LIT_NOISE2,			// terrain_lit.hlsl, with two noise stages, replaces PS_ROAD_NOISE2
		PS_TREES_LIT,				// trees.hlsl, per-pixel lit trees (M3.1d)
		PS_POST_COPY,				// post.hlsl, copies the scene texture to the back buffer (M3.2a)
		PS_POST_BRIGHT,				// post.hlsl, bloom bright pass and first downsample (M3.2b)
		PS_POST_DOWN,				// post.hlsl, bloom downsample (M3.2b)
		PS_POST_BLUR,				// post.hlsl, bloom Gaussian blur (M3.2b)
		PS_POST_FINAL,				// post.hlsl, scene plus bloom, tone curve and colour LUT onto the back buffer (M3.2b/c)
		PS_POST_FXAA,				// post.hlsl, edge anti-aliasing of the finished colours (M3.2d)
		PS_POST_AO,					// post.hlsl, ambient occlusion from the scene depth (M3.5)
		PS_POST_AO_BLUR,			// post.hlsl, depth-aware blur of the ambient occlusion (M3.5)
		PS_WATER_TRAPEZOID,			// water.hlsl, trapezoid water with waves, Fresnel and reflection (M3.3a)
		PS_WATER_RIVER,				// water.hlsl, river water with waves, Fresnel and reflection (M3.3a)
		PS_WATER_TRAPEZOID_HQ,		// water.hlsl, trapezoid water seen into: depth colour, refraction, shore foam (M3.3b)
		PS_WATER_RIVER_HQ,			// water.hlsl, river water seen into: depth colour, refraction, shore foam (M3.3b)
		PS_PARTICLE,				// mesh.hlsl, soft particles: fade where they meet the scene (EwSoftParticles)
		PS_POST_DEPTH_COPY,			// post.hlsl, the scene's view depth into R32F for the soft particles
		PS_UNLIT,					// mesh.hlsl, draws with lighting off, as fixed function
		PS_COUNT
	};

	// Returns the shader with a reference added, which the caller releases, so code that
	// used to create its own shader keeps its Release calls. Null if the device has no
	// support for the shader's profile or creation failed.
	static IDirect3DVertexShader9 *Vertex(VertexShaderId id);
	static IDirect3DPixelShader9 *Pixel(PixelShaderId id);

	// As above but without adding a reference, for code that binds a shader for one draw.
	static IDirect3DVertexShader9 *Peek_Vertex(VertexShaderId id);
	static IDirect3DPixelShader9 *Peek_Pixel(PixelShaderId id);

	static VertexShaderId Mesh_Vertex_Id(bool normal, bool diffuse, bool specular, int texcoord_sets)
	{
		return VertexShaderId((normal ? VS_MESH_N_T0 : VS_MESH_T0) + texcoord_sets * 4 + (diffuse ? 1 : 0) + (specular ? 2 : 0));
	}

	// Releases the store's references. Called before the device is released.
	static void Shutdown();

	// True when the device's adapter supports the format for this usage, at the desktop's format.
	static bool Format_Supported(IDirect3DDevice9 *device, DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format);
};
