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

#include "W3DDevice/GameClient/W3DTerrainLighting.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "Common/GlobalData.h"
#include "Common/MapObject.h"
#include "WW3D2/dx9wrapper.h"
#include "WW3D2/dx9caps.h"
#include "WW3D2/shaderlibrary.h"
#include "WW3D2/graphicsoptions.h"
#include "WWDebug/wwdebug.h"
#include "shader_constants.h"

#include <math.h>
#include <string.h>

// Like EwMeshShader, all state is read from the (non-pure) device: TerrainShaderPixelShader::set
// sets the textures, texture matrices and pixel shader on the device directly.

namespace
{
	enum Reason
	{
		REASON_NO_SM3,
		REASON_NO_SHADER,
		REASON_FORMAT,
		REASON_MAP_SIZE,
		REASON_TEXTURE,
		REASON_CLIP_PLANES,
		REASON_FOG_MODE,
		REASON_COUNT
	};

	const char *const ReasonText[] =
	{
		"the card has no vs_3_0/ps_3_0",
		"the terrain shaders failed to load",
		"no vertex texture fetch or filtering for A8R8G8B8",
		"height map size (width << 16 | height)",
		"the normal texture could not be created or locked (HRESULT)",
		"user clip planes",
		"fog mode other than linear",
	};
	static_assert(sizeof(ReasonText) / sizeof(ReasonText[0]) == REASON_COUNT, "ReasonText must match Reason");

	bool Logged[REASON_COUNT];

	// For the debug keys' status message: passes drawn with the shader, and why the last pass
	// that was not drawn with it fell back. Terrain passes and road/blend-tile passes each keep
	// their own; Reason_Slot points at the one the current Begin call reports to.
	unsigned FramePasses;
	unsigned LastFramePasses;
	const char *LastReason = "no terrain pass was drawn";
	unsigned RoadFramePasses;
	unsigned LastRoadFramePasses;
	const char *LastRoadReason = "no road or blend tile pass was drawn";
	unsigned TreeFramePasses;
	unsigned LastTreeFramePasses;
	const char *LastTreeReason = "no tree was drawn";
	const char **Reason_Slot = &LastReason;

	bool Skip(const char *why)
	{
		*Reason_Slot = why;
		return false;
	}

	bool Fallback(Reason reason, unsigned value)
	{
		*Reason_Slot = ReasonText[reason];
		if (!Logged[reason])
		{
			Logged[reason] = true;
			WWDEBUG_SAY(("EwTerrainLighting: per-vertex fallback for %s, value 0x%x (logged once)", ReasonText[reason], value));
		}
		return false;
	}

	struct Float4
	{
		float x, y, z, w;
	};

	IDirect3DDevice9Ex *Device;

	// The grid normals of the whole height map, one texel per height sample, and the heights
	// they were built from.
	IDirect3DTexture9 *NormalTexture;
	int TextureWidth;
	int TextureHeight;
	UnsignedByte *Heights;
	int FormatSupport;	// 0 not checked yet, 1 supported, -1 not supported

	// The pixel sampler that holds the normals for the pixel shader; samplers 0-3 are the
	// terrain's. Begin() saves what it held and End() puts it back.
	const DWORD NORMAL_SAMPLER = 4;
	const D3DSAMPLERSTATETYPE NormalSamplerStates[] =
	{
		D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
	};
	const DWORD NormalSamplerValues[] =
	{
		D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE,
	};
	const int NORMAL_SAMPLER_STATE_COUNT = sizeof(NormalSamplerStates) / sizeof(NormalSamplerStates[0]);
	static_assert(sizeof(NormalSamplerValues) / sizeof(NormalSamplerValues[0]) == NORMAL_SAMPLER_STATE_COUNT, "one value per sampler state");

	IDirect3DBaseTexture9 *SavedNormalSamplerTexture;
	DWORD SavedNormalSamplerState[NORMAL_SAMPLER_STATE_COUNT];
	IDirect3DPixelShader9 *SavedPixelShader;
	bool RestoreFog;

	DWORD Render_State(D3DRENDERSTATETYPE state)
	{
		DWORD value = 0;
		Device->GetRenderState(state, &value);
		return value;
	}

	float Render_State_Float(D3DRENDERSTATETYPE state)
	{
		DWORD value = Render_State(state);
		float f;
		memcpy(&f, &value, sizeof(f));
		return f;
	}

	Float4 Color_Value(D3DCOLOR c)
	{
		Float4 f = { ((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f };
		return f;
	}

	void Multiply(D3DMATRIX &out, const D3DMATRIX &a, const D3DMATRIX &b)
	{
		D3DMATRIX r;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
		out = r;
	}

	// The shader computes out.k = dot(v, c[k]), so the matrix goes up as its columns.
	void Columns(Float4 *out, const D3DMATRIX &m, int count)
	{
		for (int k = 0; k < count; ++k)
		{
			Float4 c = { m.m[0][k], m.m[1][k], m.m[2][k], m.m[3][k] };
			out[k] = c;
		}
	}

	bool Is_Power_Of_Two(int n)
	{
		return n > 0 && (n & (n - 1)) == 0;
	}

	// The normal texture is read by the vertex shader and filtered in the pixel shader.
	bool Format_Supported()
	{
		if (FormatSupport != 0)
			return FormatSupport > 0;

		FormatSupport = -1;
		IDirect3D9 *d3d = nullptr;
		if (FAILED(Device->GetDirect3D(&d3d)) || d3d == nullptr)
			return false;
		D3DDEVICE_CREATION_PARAMETERS params;
		D3DDISPLAYMODE mode;
		if (SUCCEEDED(Device->GetCreationParameters(&params)) && SUCCEEDED(Device->GetDisplayMode(0, &mode))
			&& SUCCEEDED(d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
				D3DUSAGE_DYNAMIC | D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8))
			&& SUCCEEDED(d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
				D3DUSAGE_DYNAMIC | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)))
		{
			FormatSupport = 1;
		}
		d3d->Release();
		return FormatSupport > 0;
	}

	DWORD Pack_Channel(float v)
	{
		int i = int((v * 0.5f + 0.5f) * 255.0f + 0.5f);
		return DWORD(i < 0 ? 0 : (i > 255 ? 255 : i));
	}

	void Release_Texture()
	{
		if (NormalTexture != nullptr)
		{
			NormalTexture->Release();
			NormalTexture = nullptr;
		}
		delete[] Heights;
		Heights = nullptr;
		TextureWidth = 0;
		TextureHeight = 0;
	}

	// Rebuilds the normal texture when the map's size or heights changed.
	bool Update_Texture(WorldHeightMap *map)
	{
		const int width = map->getXExtent();
		const int height = map->getYExtent();
		const UnsignedByte *data = map->getDataPtr();
		if (width < 2 || height < 2 || data == nullptr)
			return Fallback(REASON_MAP_SIZE, (unsigned(width) << 16) | unsigned(height));
		const size_t count = size_t(width) * size_t(height);

		if (NormalTexture != nullptr && width == TextureWidth && height == TextureHeight && memcmp(Heights, data, count) == 0)
			return true;

		if (NormalTexture == nullptr || width != TextureWidth || height != TextureHeight)
		{
			Release_Texture();

			const D3DCAPS9 &caps = DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps();
			const bool pow2_only = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL);
			if (DWORD(width) > caps.MaxTextureWidth || DWORD(height) > caps.MaxTextureHeight
				|| (pow2_only && (!Is_Power_Of_Two(width) || !Is_Power_Of_Two(height))))
			{
				return Fallback(REASON_MAP_SIZE, (unsigned(width) << 16) | unsigned(height));
			}

			HRESULT hr = Device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &NormalTexture, nullptr);
			if (FAILED(hr))
			{
				NormalTexture = nullptr;
				return Fallback(REASON_TEXTURE, unsigned(hr));
			}
			Heights = new UnsignedByte[count];
			TextureWidth = width;
			TextureHeight = height;
		}

		D3DLOCKED_RECT rect;
		HRESULT hr = NormalTexture->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD);
		if (FAILED(hr))
		{
			Release_Texture();
			return Fallback(REASON_TEXTURE, unsigned(hr));
		}

		// As updateVB: the cross product of (2F, 0, S * dhx) and (0, 2F, S * dhy), where dh is
		// the height difference between the two neighbours, clamped at the map edge.
		const float xy = -2.0f * MAP_XY_FACTOR * MAP_HEIGHT_SCALE;
		const float z = 4.0f * MAP_XY_FACTOR * MAP_XY_FACTOR;
		for (int y = 0; y < height; ++y)
		{
			const int y0 = (y > 0) ? y - 1 : 0;
			const int y1 = (y < height - 1) ? y + 1 : height - 1;
			DWORD *row = (DWORD *)((BYTE *)rect.pBits + size_t(y) * size_t(rect.Pitch));
			for (int x = 0; x < width; ++x)
			{
				const int x0 = (x > 0) ? x - 1 : 0;
				const int x1 = (x < width - 1) ? x + 1 : width - 1;
				const float dhx = float(data[x1 + y * width]) - float(data[x0 + y * width]);
				const float dhy = float(data[x + y1 * width]) - float(data[x + y0 * width]);
				float nx = xy * dhx, ny = xy * dhy, nz = z;
				const float s = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
				nx *= s;
				ny *= s;
				nz *= s;
				row[x] = 0xff000000 | (Pack_Channel(nx) << 16) | (Pack_Channel(ny) << 8) | Pack_Channel(nz);
			}
		}
		NormalTexture->UnlockRect(0);
		memcpy(Heights, data, count);
		return true;
	}
}

namespace
{
	// The checks every pass makes before looking at what the pass set up.
	bool Enabled(WorldHeightMap *map)
	{
		if (!EwGraphics().PerPixelLighting)
			return Skip("EwPerPixelLighting is off in Options.ini");
		if (!EwGraphics().TerrainShader())
			return Skip("switched off by a debug key");
		if (map == nullptr || TheGlobalData == nullptr)
			return Skip("no height map");

		Device = DX8Wrapper::_Get_D3D_Device9();
		if (Device == nullptr)
			return Skip("no device");
		return true;
	}

	bool Bind(WorldHeightMap *map, EwShaders::VertexShaderId vertex_id, EwShaders::PixelShaderId pixel_id,
		D3DTRANSFORMSTATETYPE texture_a, D3DTRANSFORMSTATETYPE texture_b);
}

bool EwTerrainLighting::Begin(WorldHeightMap *map)
{
	Reason_Slot = &LastReason;
	if (!Enabled(map))
		return false;

	// Only the passes TerrainShaderPixelShader set up with a parity pixel shader are ours; the
	// bound shader tells which variant. The two-stage fixed-function path has none.
	IDirect3DPixelShader9 *bound_ps = nullptr;
	Device->GetPixelShader(&bound_ps);
	if (bound_ps == nullptr)
		return Skip("the terrain pass uses no pixel shader (fixed-function terrain)");
	bound_ps->Release();
	EwShaders::PixelShaderId lit_id;
	if (bound_ps == EwShaders::Peek_Pixel(EwShaders::PS_TERRAIN))
		lit_id = EwShaders::PS_TERRAIN_LIT;
	else if (bound_ps == EwShaders::Peek_Pixel(EwShaders::PS_TERRAIN_NOISE1))
		lit_id = EwShaders::PS_TERRAIN_LIT_NOISE1;
	else if (bound_ps == EwShaders::Peek_Pixel(EwShaders::PS_TERRAIN_NOISE2))
		lit_id = EwShaders::PS_TERRAIN_LIT_NOISE2;
	else
		return Skip("the terrain pass uses another pixel shader (.pso fallback)");

	if (!Bind(map, EwShaders::VS_TERRAIN_LIT, lit_id, D3DTS_TEXTURE2, D3DTS_TEXTURE3))
		return false;
	++FramePasses;
	return true;
}

bool EwTerrainLighting::Begin_Road(WorldHeightMap *map, int noise_textures, int passes)
{
	Reason_Slot = &LastRoadReason;
	if (!Enabled(map))
		return false;

	// Without ps_1_1 the shader manager draws two noise textures in two fixed-function passes;
	// a card that old has no vs_3_0 either.
	if (passes != 1)
		return Skip("the road shader uses two passes (no pixel shader road path)");

	EwShaders::PixelShaderId lit_id = EwShaders::PS_ROAD_LIT;
	if (noise_textures == 1)
		lit_id = EwShaders::PS_ROAD_LIT_NOISE1;
	else if (noise_textures >= 2)
		lit_id = EwShaders::PS_ROAD_LIT_NOISE2;

	// RoadShader2Stage maps stage 1 through D3DTS_TEXTURE1; RoadShaderPixelShader maps stages 1
	// and 2 through D3DTS_TEXTURE1 and D3DTS_TEXTURE2.
	if (!Bind(map, EwShaders::VS_ROAD_LIT, lit_id, D3DTS_TEXTURE1, D3DTS_TEXTURE2))
		return false;
	++RoadFramePasses;
	return true;
}

// Binds the lit shaders over the pass that is set up on the device. The pass's texture
// coordinates from camera-space position go through texture_a and texture_b.
namespace
{
bool Bind(WorldHeightMap *map, EwShaders::VertexShaderId vertex_id, EwShaders::PixelShaderId pixel_id,
	D3DTRANSFORMSTATETYPE texture_a, D3DTRANSFORMSTATETYPE texture_b)
{
	IDirect3DVertexShader9 *bound_vs = nullptr;
	Device->GetVertexShader(&bound_vs);
	if (bound_vs != nullptr)
	{
		bound_vs->Release();
		return Skip("the pass has a vertex shader bound");
	}

	const DX8Caps *caps = DX8Wrapper::Get_Current_Caps();
	if (caps->Get_Vertex_Shader_Major_Version() < 3 || caps->Get_Pixel_Shader_Major_Version() < 3)
		return Fallback(REASON_NO_SM3, (caps->Get_Vertex_Shader_Major_Version() << 8) | caps->Get_Pixel_Shader_Major_Version());

	IDirect3DVertexShader9 *vertex_shader = EwShaders::Peek_Vertex(vertex_id);
	IDirect3DPixelShader9 *pixel_shader = EwShaders::Peek_Pixel(pixel_id);
	if (vertex_shader == nullptr || pixel_shader == nullptr)
		return Fallback(REASON_NO_SHADER, (unsigned(vertex_id) << 8) | unsigned(pixel_id));

	// With shaders bound, D3D9 reads clip planes in clip space instead of world space.
	DWORD clip_planes = Render_State(D3DRS_CLIPPLANEENABLE);
	if (clip_planes != 0)
		return Fallback(REASON_CLIP_PLANES, clip_planes);

	const bool fog_enabled = Render_State(D3DRS_FOGENABLE) != FALSE;
	Float4 fog = { 0.0f, 0.0f, 0.0f, 0.0f };
	float range_fog = 0.0f;
	if (fog_enabled)
	{
		DWORD table_mode = Render_State(D3DRS_FOGTABLEMODE);
		DWORD mode = (table_mode != D3DFOG_NONE) ? table_mode : Render_State(D3DRS_FOGVERTEXMODE);
		if (mode != D3DFOG_LINEAR)
			return Fallback(REASON_FOG_MODE, mode);
		float start = Render_State_Float(D3DRS_FOGSTART);
		float end = Render_State_Float(D3DRS_FOGEND);
		float range = end - start;
		fog.x = 1.0f;
		fog.y = end;
		fog.z = (fabsf(range) > 1e-6f) ? 1.0f / range : 1e6f;
		if (table_mode == D3DFOG_NONE && Render_State(D3DRS_RANGEFOGENABLE))
			range_fog = 1.0f;
	}

	if (!Format_Supported())
		return Fallback(REASON_FORMAT, 0);
	if (!Update_Texture(map))
		return false;	// Update_Texture records the reason

	D3DMATRIX world, view, projection, world_view, world_view_projection, matrix_a, matrix_b;
	Device->GetTransform(D3DTS_WORLD, &world);
	Device->GetTransform(D3DTS_VIEW, &view);
	Device->GetTransform(D3DTS_PROJECTION, &projection);
	Device->GetTransform(texture_a, &matrix_a);
	Device->GetTransform(texture_b, &matrix_b);
	Multiply(world_view, world, view);
	Multiply(world_view_projection, world_view, projection);

	// The sun lights as BaseHeightMapRenderObjClass::doTheLight adds them: the ambient of the
	// first light, and each light's diffuse against the direction updateVB passes (-position).
	Float4 sun[EW_TERRAIN_SUN_COUNT];
	memset(sun, 0, sizeof(sun));
	sun[0].x = TheGlobalData->m_terrainAmbient[0].red;
	sun[0].y = TheGlobalData->m_terrainAmbient[0].green;
	sun[0].z = TheGlobalData->m_terrainAmbient[0].blue;
	int lights = TheGlobalData->m_numGlobalLights;
	if (lights > 3)
		lights = 3;
	for (int i = 0; i < lights; ++i)
	{
		const Coord3D &position = TheGlobalData->m_terrainLightPos[i];
		const RGBColor &diffuse = TheGlobalData->m_terrainDiffuse[i];
		Float4 direction = { -position.x, -position.y, -position.z, 0.0f };
		Float4 colour = { diffuse.red, diffuse.green, diffuse.blue, 0.0f };
		sun[1 + i] = direction;
		sun[4 + i] = colour;
	}

	// World xy to the centre of the height sample's texel: index = x / F + border.
	const float border = float(map->getBorderSizeInline());
	const float w = float(TextureWidth);
	const float h = float(TextureHeight);
	Float4 grid_map = { 1.0f / (MAP_XY_FACTOR * w), 1.0f / (MAP_XY_FACTOR * h), (border + 0.5f) / w, (border + 0.5f) / h };

	Float4 vs[EW_TERRAIN_VS_COUNT];
	memset(vs, 0, sizeof(vs));
	Columns(&vs[EW_TERRAIN_VS_WVP], world_view_projection, 4);
	Columns(&vs[EW_TERRAIN_VS_WORLD], world, 3);
	Columns(&vs[EW_TERRAIN_VS_WORLDVIEW], world_view, 3);
	Columns(&vs[EW_TERRAIN_VS_TEXMATRIX + 0], matrix_a, 4);
	Columns(&vs[EW_TERRAIN_VS_TEXMATRIX + 4], matrix_b, 4);
	vs[EW_TERRAIN_VS_GRIDMAP] = grid_map;
	vs[EW_TERRAIN_VS_OPTIONS].x = range_fog;
	memcpy(&vs[EW_TERRAIN_SUN], sun, sizeof(sun));

	Float4 ps[EW_TERRAIN_PS_COUNT - EW_TERRAIN_SUN];
	memset(ps, 0, sizeof(ps));
	memcpy(&ps[0], sun, sizeof(sun));
	ps[EW_TERRAIN_PS_FOG - EW_TERRAIN_SUN] = fog;
	ps[EW_TERRAIN_PS_FOGCOLOR - EW_TERRAIN_SUN] = Color_Value(D3DCOLOR(Render_State(D3DRS_FOGCOLOR)));
	ps[EW_TERRAIN_PS_DEBUG - EW_TERRAIN_SUN].x = EwGraphics().TerrainPixelLight() ? 0.0f : 1.0f;
	ps[EW_TERRAIN_PS_DEBUG - EW_TERRAIN_SUN].y = EwGraphics().DebugTerrainLightOnly ? 1.0f : 0.0f;

	Device->SetVertexShaderConstantF(0, &vs[0].x, EW_TERRAIN_VS_COUNT);
	Device->SetPixelShaderConstantF(EW_TERRAIN_SUN, &ps[0].x, EW_TERRAIN_PS_COUNT - EW_TERRAIN_SUN);

	// The pass's own pixel shader (or none) goes back in End(), so the pass ends the way the
	// shader manager set it. GetPixelShader adds the reference End() releases.
	Device->GetPixelShader(&SavedPixelShader);
	Device->SetVertexShader(vertex_shader);
	Device->SetPixelShader(pixel_shader);

	Device->SetTexture(D3DVERTEXTEXTURESAMPLER0, NormalTexture);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

	Device->GetTexture(NORMAL_SAMPLER, &SavedNormalSamplerTexture);
	Device->SetTexture(NORMAL_SAMPLER, NormalTexture);
	for (int i = 0; i < NORMAL_SAMPLER_STATE_COUNT; ++i)
	{
		Device->GetSamplerState(NORMAL_SAMPLER, NormalSamplerStates[i], &SavedNormalSamplerState[i]);
		Device->SetSamplerState(NORMAL_SAMPLER, NormalSamplerStates[i], NormalSamplerValues[i]);
	}

	// The pixel shader fogs itself. The wrapper's cache keeps TRUE, and End() puts it back.
	RestoreFog = fog_enabled;
	if (fog_enabled)
		Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	return true;
}
}

void EwTerrainLighting::New_Frame()
{
	LastFramePasses = FramePasses;
	FramePasses = 0;
	LastRoadFramePasses = RoadFramePasses;
	RoadFramePasses = 0;
	LastTreeFramePasses = TreeFramePasses;
	TreeFramePasses = 0;
}

unsigned EwTerrainLighting::Last_Frame_Passes()
{
	return LastFramePasses;
}

const char *EwTerrainLighting::Last_Reason()
{
	return LastReason;
}

unsigned EwTerrainLighting::Last_Frame_Road_Passes()
{
	return LastRoadFramePasses;
}

const char *EwTerrainLighting::Last_Road_Reason()
{
	return LastRoadReason;
}

void EwTerrainLighting::Tree_Pass(const char *fallback_reason)
{
	if (fallback_reason == nullptr)
		++TreeFramePasses;
	else
		LastTreeReason = fallback_reason;
}

unsigned EwTerrainLighting::Last_Frame_Tree_Passes()
{
	return LastTreeFramePasses;
}

const char *EwTerrainLighting::Last_Tree_Reason()
{
	return LastTreeReason;
}

void EwTerrainLighting::End()
{
	Device->SetVertexShader(nullptr);
	Device->SetPixelShader(SavedPixelShader);
	if (SavedPixelShader != nullptr)
	{
		SavedPixelShader->Release();
		SavedPixelShader = nullptr;
	}

	Device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
	Device->SetTexture(NORMAL_SAMPLER, SavedNormalSamplerTexture);
	if (SavedNormalSamplerTexture != nullptr)
	{
		SavedNormalSamplerTexture->Release();
		SavedNormalSamplerTexture = nullptr;
	}
	for (int i = 0; i < NORMAL_SAMPLER_STATE_COUNT; ++i)
		Device->SetSamplerState(NORMAL_SAMPLER, NormalSamplerStates[i], SavedNormalSamplerState[i]);

	if (RestoreFog)
	{
		Device->SetRenderState(D3DRS_FOGENABLE, TRUE);
		RestoreFog = false;
	}
}

void EwTerrainLighting::Release()
{
	Release_Texture();
	FormatSupport = 0;
}

static_assert(EW_TERRAIN_VS_WVP == 0 && EW_TERRAIN_SUN + EW_TERRAIN_SUN_COUNT == EW_TERRAIN_VS_COUNT, "terrain vertex constants are uploaded as one block from c0");
static_assert(EW_TERRAIN_VS_OPTIONS < EW_TERRAIN_SUN, "terrain vertex constants must not overlap the sun block");
static_assert(EW_TERRAIN_PS_FOG == EW_TERRAIN_VS_COUNT && EW_TERRAIN_PS_DEBUG + 1 == EW_TERRAIN_PS_COUNT, "terrain pixel constants are uploaded as one block from the sun block");
