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

#include "ewshaders_dx9.h"
#include "dx9wrapper.h"
#include "WWDebug/wwdebug.h"

// Bytecode written by fxc (cmake/eastwind_shaders.cmake), one BYTE array g_ew_<name> each.
#include "trees_vs.h"
#include "wave_vs.h"
#include "wave_ps.h"
#include "river_ps.h"
#include "trapezoid_ps.h"
#include "terrain_ps.h"
#include "terrain_noise1_ps.h"
#include "terrain_noise2_ps.h"
#include "road_noise2_ps.h"
#include "flat_terrain_ps.h"
#include "flat_terrain0_ps.h"
#include "flat_terrain_noise1_ps.h"
#include "flat_terrain_noise2_ps.h"
#include "monochrome_ps.h"
#include "profiler_swizzle_ps.h"
#include "mesh_n_t0_vs.h"
#include "mesh_nd_t0_vs.h"
#include "mesh_ns_t0_vs.h"
#include "mesh_nds_t0_vs.h"
#include "mesh_n_t1_vs.h"
#include "mesh_nd_t1_vs.h"
#include "mesh_ns_t1_vs.h"
#include "mesh_nds_t1_vs.h"
#include "mesh_n_t2_vs.h"
#include "mesh_nd_t2_vs.h"
#include "mesh_ns_t2_vs.h"
#include "mesh_nds_t2_vs.h"
#include "mesh_ps.h"

namespace
{
	struct ShaderCode
	{
		const BYTE *code;
		const char *name;
	};

	const ShaderCode VertexCode[] =
	{
		{ g_ew_trees_vs, "trees_vs" },
		{ g_ew_wave_vs, "wave_vs" },
		{ g_ew_mesh_n_t0_vs, "mesh_n_t0_vs" },
		{ g_ew_mesh_nd_t0_vs, "mesh_nd_t0_vs" },
		{ g_ew_mesh_ns_t0_vs, "mesh_ns_t0_vs" },
		{ g_ew_mesh_nds_t0_vs, "mesh_nds_t0_vs" },
		{ g_ew_mesh_n_t1_vs, "mesh_n_t1_vs" },
		{ g_ew_mesh_nd_t1_vs, "mesh_nd_t1_vs" },
		{ g_ew_mesh_ns_t1_vs, "mesh_ns_t1_vs" },
		{ g_ew_mesh_nds_t1_vs, "mesh_nds_t1_vs" },
		{ g_ew_mesh_n_t2_vs, "mesh_n_t2_vs" },
		{ g_ew_mesh_nd_t2_vs, "mesh_nd_t2_vs" },
		{ g_ew_mesh_ns_t2_vs, "mesh_ns_t2_vs" },
		{ g_ew_mesh_nds_t2_vs, "mesh_nds_t2_vs" },
	};
	static_assert(sizeof(VertexCode) / sizeof(VertexCode[0]) == EwShaders::VS_COUNT, "VertexCode must match EwShaders::VertexShaderId");

	const ShaderCode PixelCode[] =
	{
		{ g_ew_wave_ps, "wave_ps" },
		{ g_ew_river_ps, "river_ps" },
		{ g_ew_trapezoid_ps, "trapezoid_ps" },
		{ g_ew_terrain_ps, "terrain_ps" },
		{ g_ew_terrain_noise1_ps, "terrain_noise1_ps" },
		{ g_ew_terrain_noise2_ps, "terrain_noise2_ps" },
		{ g_ew_road_noise2_ps, "road_noise2_ps" },
		{ g_ew_flat_terrain_ps, "flat_terrain_ps" },
		{ g_ew_flat_terrain0_ps, "flat_terrain0_ps" },
		{ g_ew_flat_terrain_noise1_ps, "flat_terrain_noise1_ps" },
		{ g_ew_flat_terrain_noise2_ps, "flat_terrain_noise2_ps" },
		{ g_ew_monochrome_ps, "monochrome_ps" },
		{ g_ew_profiler_swizzle_ps, "profiler_swizzle_ps" },
		{ g_ew_mesh_ps, "mesh_ps" },
	};
	static_assert(sizeof(PixelCode) / sizeof(PixelCode[0]) == EwShaders::PS_COUNT, "PixelCode must match EwShaders::PixelShaderId");

	IDirect3DVertexShader9 *VertexShaders[EwShaders::VS_COUNT];
	IDirect3DPixelShader9 *PixelShaders[EwShaders::PS_COUNT];

	// A failed creation is not retried every frame.
	bool VertexFailed[EwShaders::VS_COUNT];
	bool PixelFailed[EwShaders::PS_COUNT];
}

IDirect3DVertexShader9 *EwShaders::Vertex(VertexShaderId id)
{
	IDirect3DVertexShader9 *shader = Peek_Vertex(id);
	if (shader)
		shader->AddRef();
	return shader;
}

IDirect3DPixelShader9 *EwShaders::Pixel(PixelShaderId id)
{
	IDirect3DPixelShader9 *shader = Peek_Pixel(id);
	if (shader)
		shader->AddRef();
	return shader;
}

IDirect3DVertexShader9 *EwShaders::Peek_Vertex(VertexShaderId id)
{
	WWASSERT(id >= 0 && id < VS_COUNT);
	if (!VertexShaders[id] && !VertexFailed[id])
	{
		IDirect3DDevice9Ex *device = DX8Wrapper::_Get_D3D_Device9();
		if (device == nullptr)
			return nullptr;
		HRESULT hr = device->CreateVertexShader(reinterpret_cast<const DWORD *>(VertexCode[id].code), &VertexShaders[id]);
		if (FAILED(hr))
		{
			WWDEBUG_SAY(("EwShaders: CreateVertexShader(%s) failed, 0x%08x", VertexCode[id].name, hr));
			VertexShaders[id] = nullptr;
			VertexFailed[id] = true;
		}
	}
	return VertexShaders[id];
}

IDirect3DPixelShader9 *EwShaders::Peek_Pixel(PixelShaderId id)
{
	WWASSERT(id >= 0 && id < PS_COUNT);
	if (!PixelShaders[id] && !PixelFailed[id])
	{
		IDirect3DDevice9Ex *device = DX8Wrapper::_Get_D3D_Device9();
		if (device == nullptr)
			return nullptr;
		HRESULT hr = device->CreatePixelShader(reinterpret_cast<const DWORD *>(PixelCode[id].code), &PixelShaders[id]);
		if (FAILED(hr))
		{
			WWDEBUG_SAY(("EwShaders: CreatePixelShader(%s) failed, 0x%08x", PixelCode[id].name, hr));
			PixelShaders[id] = nullptr;
			PixelFailed[id] = true;
		}
	}
	return PixelShaders[id];
}

void EwShaders::Shutdown()
{
	for (int i = 0; i < VS_COUNT; ++i)
	{
		if (VertexShaders[i])
		{
			VertexShaders[i]->Release();
			VertexShaders[i] = nullptr;
		}
		VertexFailed[i] = false;
	}
	for (int i = 0; i < PS_COUNT; ++i)
	{
		if (PixelShaders[i])
		{
			PixelShaders[i]->Release();
			PixelShaders[i] = nullptr;
		}
		PixelFailed[i] = false;
	}
}
