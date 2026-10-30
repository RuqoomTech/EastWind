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

#include "W3DDevice/GameClient/W3DWaterShader.h"
#include "W3DDevice/GameClient/BaseHeightMap.h"
#include "Common/GlobalData.h"
#include "WW3D2/dx9wrapper.h"
#include "WW3D2/dx9caps.h"
#include "WW3D2/shaderlibrary.h"
#include "WW3D2/graphicsoptions.h"
#include "WWDebug/wwdebug.h"
#include "shader_constants.h"

#include <math.h>
#include <string.h>
#include <vector>
#include <mmsystem.h>

// Like W3DTerrainLighting, all state is read from the device: setupFlatWaterShader and
// setupJbaWaterShader set the textures, texture matrices and pixel shader on it directly.

namespace
{
	enum Reason
	{
		REASON_NO_SM3,
		REASON_NO_SHADER,
		REASON_CLIP_PLANES,
		REASON_FOG_MODE,
		REASON_WAVE_TEXTURE,
		REASON_COUNT
	};

	const char *const ReasonText[] =
	{
		"the card has no vs_3_0/ps_3_0",
		"the water shaders failed to load",
		"user clip planes",
		"fog mode other than linear",
		"the wave texture could not be created (HRESULT)",
	};
	static_assert(sizeof(ReasonText) / sizeof(ReasonText[0]) == REASON_COUNT, "ReasonText must match Reason");

	bool Logged[REASON_COUNT];

	unsigned FramePasses;
	unsigned LastFramePasses;
	const char *LastReason = "no water was drawn";

	bool Skip(const char *why)
	{
		LastReason = why;
		return false;
	}

	bool Fallback(Reason reason, unsigned value)
	{
		LastReason = ReasonText[reason];
		if (!Logged[reason])
		{
			Logged[reason] = true;
			WWDEBUG_SAY(("W3DWaterShader: classic water for %s, value 0x%x (logged once)", ReasonText[reason], value));
		}
		return false;
	}

	struct Float4
	{
		float x, y, z, w;
	};

	IDirect3DDevice9Ex *Device;

	// --- Look of the water. Fixed for now; they move to INI data later. ---
	const float WAVE_SCALE_A = 1.0f / 160.0f;	// layer A: one normal map tile per 160 world units
	const float WAVE_SCALE_B = 1.0f / 97.0f;	// layer B: smaller, so the layers do not line up
	const float WAVE_SPEED_A[2] = { 0.010f, 0.006f };	// tiles per second
	const float WAVE_SPEED_B[2] = { -0.008f, 0.011f };
	const float WAVE_NORMAL_STRENGTH = 1.0f;
	const float REFLECTION_DISTORTION = 0.02f;	// screen uv moved per unit of wave normal
	// Fresnel: some reflection looking down on the water, as from the usual RTS camera, and more
	// toward the horizon. reflection = min(bias + scale * (1 - cos)^power, 1) * max.
	const float FRESNEL_BIAS = 0.25f;
	const float FRESNEL_SCALE = 0.75f;
	const float FRESNEL_POWER = 4.0f;
	const float FRESNEL_MAX = 0.75f;
	const float SPECULAR_POWER = 200.0f;
	const float SPECULAR_STRENGTH = 0.8f;
	const float WAVE_SHADING = 0.9f;	// how much the waves light and shade the water texture
	const float TEXTURE_WOBBLE = 0.03f;	// water texture uv moved per unit of wave normal
	const float REFLECTION_OPACITY = 0.4f;	// how much the reflection makes see-through water opaque
	const float SKY_COLOR[3] = { 0.45f, 0.55f, 0.65f };
	const float MIRROR_LEVEL_TOLERANCE = 2.0f;	// water this close to the mirror height uses the reflection

	// --- The wave normal map: tiling, with a full mip chain. ---
	const int WAVE_SIZE = 256;
	const int WAVE_LEVELS = 9;	// 256 down to 1
	IDirect3DTexture9 *WaveTexture;

	// --- The reflection. ---
	IDirect3DTexture9 *ReflectionTexture;
	IDirect3DSurface9 *ReflectionSurface;
	IDirect3DSurface9 *ReflectionDepth;
	int ReflectionWidth;
	int ReflectionHeight;
	bool HasReflection;	// drawn this frame
	float ReflectionLevel;
	bool LastHasReflection;
	float LastReflectionLevel;
	const char *ReflectionReason = "not drawn yet";
	const char *LastReflectionReason = "not drawn yet";

	// --- WaterQuality 2: the water seen into (M3.3b). ---
	const float HQ_FRESNEL_BIAS = 0.15f;
	const float HQ_FRESNEL_SCALE = 0.88f;
	const float HQ_FRESNEL_POWER = 3.0f;
	const float HQ_FRESNEL_MAX = 0.85f;
	const float HQ_SPECULAR_POWER = 400.0f;
	const float HQ_SPECULAR_STRENGTH = 1.2f;
	const float HQ_WAVE_SHADING = 0.35f;	// the swell shades the water gently; the ripples hardly
	const float SWELL_SIZE = 0.3f;		// the swell is the wave map 1 / 0.3 times larger than layer A
	const float SWELL_STRENGTH = 0.8f;
	const float RIPPLE_STRENGTH = 0.35f;
	const float HQ_BROAD_GLINT = 0.12f;	// a softer sun glow around the sharp glints
	const float REFRACTION_DISTORTION = 0.015f;	// refraction uv moved per unit of wave normal
	// Light absorbed per world unit it travels through the water: red goes first, so shallows are
	// blue-green and deep water takes the water's own colour.
	const float ABSORPTION[3] = { 0.16f, 0.07f, 0.045f };
	const float DEEP_COLOR_SCALE = 0.55f;	// deep water: the classic water colour this much darker
	const float FOAM_DEPTH = 2.5f;		// shore foam reaches out to water this deep
	const float FOAM_STRENGTH = 0.9f;
	const float CAUSTIC_STRENGTH = 0.5f;
	const float CAUSTIC_DEPTH = 12.0f;	// caustics fade out by this depth
	const float SHORE_WAVE_SPEED = 1.3f;	// radians a second

	// The terrain heights, one texel per height map vertex, refreshed when the map changes.
	IDirect3DTexture9 *HeightTexture;
	IDirect3DTexture9 *HeightStaging;
	std::vector<unsigned char> HeightCopy;
	const void *HeightSource;
	int HeightWidth;
	int HeightHeight;
	int HeightBorder;
	unsigned FrameNumber;
	unsigned HeightCheckedFrame;
	const unsigned HEIGHT_CHECK_FRAMES = 30;	// how often to look for craters and other changes

	// Foam and caustic noise: r a web of lines (cell edges), g round bubbles. Tiling, mipmapped.
	const int FOAM_SIZE = 256;
	const int FOAM_LEVELS = 9;
	IDirect3DTexture9 *FoamTexture;

	// The scene behind the water, copied from the render target before the first water draw of
	// the frame.
	IDirect3DTexture9 *RefractionTexture;
	IDirect3DSurface9 *RefractionSurface;
	D3DSURFACE_DESC RefractionDesc;
	unsigned RefractionFrame = ~0u;	// the frame it was copied in
	bool RefractionValid;

	// For the debug keys: draws seen into, with terrain heights, and why not.
	unsigned FrameHighQualityPasses;
	unsigned LastHighQualityPasses;
	const char *HighQualityReason = "no water drawn";
	const char *LastHighQualityReason = "no water drawn";

	// Samplers 0-3 are the water's: s4 the waves, s5 the reflection. Begin() saves what they
	// held and End() puts it back.
	const DWORD WAVE_SAMPLER = 4;
	const DWORD REFLECTION_SAMPLER = 5;
	const D3DSAMPLERSTATETYPE SamplerStates[] =
	{
		D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
	};
	const DWORD WaveSamplerValues[] =
	{
		D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR,
	};
	const DWORD ReflectionSamplerValues[] =
	{
		D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE,
	};
	const int SAMPLER_STATE_COUNT = sizeof(SamplerStates) / sizeof(SamplerStates[0]);
	static_assert(sizeof(WaveSamplerValues) / sizeof(WaveSamplerValues[0]) == SAMPLER_STATE_COUNT, "one value per sampler state");
	static_assert(sizeof(ReflectionSamplerValues) / sizeof(ReflectionSamplerValues[0]) == SAMPLER_STATE_COUNT, "one value per sampler state");

	// WaterQuality 2 adds s6 the refraction, s7 the terrain heights and s8 the foam noise.
	const DWORD REFRACTION_SAMPLER = 6;
	const DWORD HEIGHT_SAMPLER = 7;
	const DWORD FOAM_SAMPLER = 8;

	// The samplers Begin() sets, and what they held before, for End().
	struct SamplerSlot
	{
		DWORD stage;
		IDirect3DBaseTexture9 *saved_texture;
		DWORD saved_states[SAMPLER_STATE_COUNT];
	};
	SamplerSlot Slots[8];
	int SlotCount;

	void Bind_Sampler(DWORD stage, IDirect3DBaseTexture9 *texture, const DWORD *values)
	{
		SamplerSlot &slot = Slots[SlotCount++];
		slot.stage = stage;
		Device->GetTexture(stage, &slot.saved_texture);	// adds the reference End() releases
		Device->SetTexture(stage, texture);
		for (int i = 0; i < SAMPLER_STATE_COUNT; ++i)
		{
			Device->GetSamplerState(stage, SamplerStates[i], &slot.saved_states[i]);
			Device->SetSamplerState(stage, SamplerStates[i], values[i]);
		}
	}

	// The water texture is read at its smallest mip for its average colour, which needs mip filtering.
	bool RestoreWaterMip;
	DWORD SavedWaterMip;

	// WaterQuality 2 draws the water over the scene with plain alpha blending.
	bool RestoreBlend;
	DWORD SavedBlend[3];
	const D3DRENDERSTATETYPE BlendStates[3] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND };

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

	BYTE Pack_Channel(float v)
	{
		int i = int((v * 0.5f + 0.5f) * 255.0f + 0.5f);
		return BYTE(i < 0 ? 0 : (i > 255 ? 255 : i));
	}

	void Normalize(float n[3])
	{
		const float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		const float s = (length > 1e-6f) ? 1.0f / length : 0.0f;
		n[0] *= s;
		n[1] *= s;
		n[2] *= s;
	}

	// The waves are a sum of sine waves with whole-number frequencies across the tile, so the
	// texture tiles. The slopes are exact derivatives, scaled to a set average steepness.
	// Frequencies run from 2 to 32 cycles a tile, spread evenly on a log scale, in fixed
	// pseudo-random directions. Amplitude falls a bit faster than 1 / frequency, so the slopes are
	// mostly fine ripples with some broader swell: small glints, no large blobs.
	struct Wave
	{
		int fx, fy;		// cycles across the tile
		float amplitude;
		float phase;
	};

	void Make_Waves(std::vector<Wave> &waves)
	{
		const int WAVE_COUNT = 40;
		const float MIN_FREQUENCY = 2.0f;
		const float MAX_FREQUENCY = 32.0f;
		const float two_pi = 6.28318530718f;
		unsigned seed = 12345u;
		waves.clear();
		for (int k = 0; k < WAVE_COUNT; ++k)
		{
			seed = seed * 1664525u + 1013904223u;
			const float direction = two_pi * float(seed >> 8) / 16777216.0f;
			seed = seed * 1664525u + 1013904223u;
			const float phase = two_pi * float(seed >> 8) / 16777216.0f;
			const float t = (k + 0.5f) / WAVE_COUNT;
			const float frequency = MIN_FREQUENCY * powf(MAX_FREQUENCY / MIN_FREQUENCY, t);
			Wave w;
			w.fx = int(floorf(cosf(direction) * frequency + 0.5f));
			w.fy = int(floorf(sinf(direction) * frequency + 0.5f));
			if (w.fx == 0 && w.fy == 0)
				w.fx = 1;
			const float length = sqrtf(float(w.fx * w.fx + w.fy * w.fy));
			w.amplitude = powf(length, -1.25f);
			w.phase = phase;
			waves.push_back(w);
		}
	}

	void Make_Wave_Normals(std::vector<float> &normals)
	{
		std::vector<Wave> waves;
		Make_Waves(waves);
		const int wave_count = int(waves.size());
		const float two_pi = 6.28318530718f;

		normals.resize(size_t(WAVE_SIZE) * WAVE_SIZE * 3);
		double sum_squares = 0.0;
		for (int y = 0; y < WAVE_SIZE; ++y)
		{
			for (int x = 0; x < WAVE_SIZE; ++x)
			{
				float dx = 0.0f, dy = 0.0f;
				for (int k = 0; k < wave_count; ++k)
				{
					const Wave &w = waves[k];
					const float angle = two_pi * (w.fx * x + w.fy * y) / WAVE_SIZE + w.phase;
					const float c = cosf(angle) * w.amplitude * two_pi / WAVE_SIZE;
					dx += c * w.fx;
					dy += c * w.fy;
				}
				float *n = &normals[(size_t(y) * WAVE_SIZE + x) * 3];
				n[0] = dx;
				n[1] = dy;
				sum_squares += double(dx) * dx + double(dy) * dy;
			}
		}

		// Average slope 0.25: clear ripples without the normal turning away from up.
		const float rms = float(sqrt(sum_squares / (double(WAVE_SIZE) * WAVE_SIZE)));
		const float scale = (rms > 1e-9f) ? 0.25f / rms : 0.0f;
		for (size_t i = 0; i < normals.size(); i += 3)
		{
			float *n = &normals[i];
			n[0] = -n[0] * scale;
			n[1] = -n[1] * scale;
			n[2] = 1.0f;
			Normalize(n);
		}
	}

	bool Create_Wave_Texture()
	{
		if (WaveTexture != nullptr)
			return true;

		IDirect3DTexture9 *staging = nullptr;
		HRESULT hr = Device->CreateTexture(WAVE_SIZE, WAVE_SIZE, WAVE_LEVELS, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr);
		if (FAILED(hr))
			return Fallback(REASON_WAVE_TEXTURE, unsigned(hr));

		std::vector<float> level;
		Make_Wave_Normals(level);
		int size = WAVE_SIZE;
		for (int l = 0; l < WAVE_LEVELS; ++l)
		{
			D3DLOCKED_RECT rect;
			hr = staging->LockRect(l, &rect, nullptr, 0);
			if (FAILED(hr))
			{
				staging->Release();
				return Fallback(REASON_WAVE_TEXTURE, unsigned(hr));
			}
			for (int y = 0; y < size; ++y)
			{
				BYTE *row = (BYTE *)rect.pBits + size_t(y) * size_t(rect.Pitch);
				for (int x = 0; x < size; ++x)
				{
					const float *n = &level[(size_t(y) * size + x) * 3];
					row[x * 4 + 0] = Pack_Channel(n[2]);	// B
					row[x * 4 + 1] = Pack_Channel(n[1]);	// G
					row[x * 4 + 2] = Pack_Channel(n[0]);	// R
					row[x * 4 + 3] = 255;
				}
			}
			staging->UnlockRect(l);

			// The next level averages 2x2 normals and normalizes again; the tile wraps.
			if (size > 1)
			{
				const int half = size / 2;
				std::vector<float> next(size_t(half) * half * 3);
				for (int y = 0; y < half; ++y)
				{
					for (int x = 0; x < half; ++x)
					{
						float *out = &next[(size_t(y) * half + x) * 3];
						out[0] = out[1] = out[2] = 0.0f;
						for (int sy = 0; sy < 2; ++sy)
							for (int sx = 0; sx < 2; ++sx)
							{
								const float *in = &level[(size_t(y * 2 + sy) * size + (x * 2 + sx)) * 3];
								out[0] += in[0];
								out[1] += in[1];
								out[2] += in[2];
							}
						Normalize(out);
					}
				}
				level.swap(next);
				size = half;
			}
		}

		hr = Device->CreateTexture(WAVE_SIZE, WAVE_SIZE, WAVE_LEVELS, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &WaveTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = Device->UpdateTexture(staging, WaveTexture);
		staging->Release();
		if (FAILED(hr))
		{
			if (WaveTexture != nullptr)
			{
				WaveTexture->Release();
				WaveTexture = nullptr;
			}
			return Fallback(REASON_WAVE_TEXTURE, unsigned(hr));
		}
		return true;
	}

	void Release_Reflection()
	{
		if (ReflectionSurface != nullptr)
		{
			ReflectionSurface->Release();
			ReflectionSurface = nullptr;
		}
		if (ReflectionTexture != nullptr)
		{
			ReflectionTexture->Release();
			ReflectionTexture = nullptr;
		}
		if (ReflectionDepth != nullptr)
		{
			ReflectionDepth->Release();
			ReflectionDepth = nullptr;
		}
		ReflectionWidth = 0;
		ReflectionHeight = 0;
	}

	bool Shaders_Supported()
	{
		const DX9Caps *caps = DX9Wrapper::Get_Current_Caps();
		return caps != nullptr && caps->Get_Vertex_Shader_Major_Version() >= 3 && caps->Get_Pixel_Shader_Major_Version() >= 3
			&& ShaderLibrary::Peek_Vertex(ShaderLibrary::VS_WATER) != nullptr
			&& ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_WATER_TRAPEZOID) != nullptr
			&& ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_WATER_RIVER) != nullptr;
	}

	// The camera position from the view matrix: the view rotation is orthonormal, so
	// position = -translation * rotation^T.
	Float4 Camera_Position(const D3DMATRIX &view)
	{
		Float4 p;
		p.x = -(view._41 * view._11 + view._42 * view._12 + view._43 * view._13);
		p.y = -(view._41 * view._21 + view._42 * view._22 + view._43 * view._23);
		p.z = -(view._41 * view._31 + view._42 * view._32 + view._43 * view._33);
		p.w = 1.0f;
		return p;
	}

	float Wrap(double v)
	{
		return float(v - floor(v));
	}

	template <class T> void Release_Com(T *&p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	// --- Terrain heights. ---

	bool Upload_Heights()
	{
		D3DLOCKED_RECT rect;
		if (FAILED(HeightStaging->LockRect(0, &rect, nullptr, 0)))
			return false;
		D3DSURFACE_DESC desc;
		HeightStaging->GetLevelDesc(0, &desc);
		const bool luminance = desc.Format == D3DFMT_L8;
		for (int y = 0; y < HeightHeight; ++y)
		{
			const unsigned char *in = &HeightCopy[size_t(y) * HeightWidth];
			BYTE *row = (BYTE *)rect.pBits + size_t(y) * size_t(rect.Pitch);
			if (luminance)
				memcpy(row, in, size_t(HeightWidth));
			else
				for (int x = 0; x < HeightWidth; ++x)
				{
					row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = in[x];
					row[x * 4 + 3] = 255;
				}
		}
		HeightStaging->UnlockRect(0);
		return SUCCEEDED(Device->UpdateTexture(HeightStaging, HeightTexture));
	}

	// The heights of the loaded map, made again when another map loads and uploaded again when
	// they change (craters, scripted terrain changes).
	bool Update_Heights()
	{
		WorldHeightMap *map = (TheTerrainRenderObject != nullptr) ? TheTerrainRenderObject->getMap() : nullptr;
		if (map == nullptr || map->getDataPtr() == nullptr || map->getXExtent() <= 0 || map->getYExtent() <= 0)
			return false;
		const unsigned char *data = map->getDataPtr();
		const int width = map->getXExtent();
		const int height = map->getYExtent();
		const size_t count = size_t(width) * size_t(height);

		if (HeightTexture == nullptr || data != HeightSource || width != HeightWidth || height != HeightHeight)
		{
			Release_Com(HeightTexture);
			Release_Com(HeightStaging);
			D3DFORMAT format = D3DFMT_L8;
			HRESULT hr = Device->CreateTexture(width, height, 1, 0, format, D3DPOOL_DEFAULT, &HeightTexture, nullptr);
			if (FAILED(hr))
			{
				format = D3DFMT_A8R8G8B8;
				hr = Device->CreateTexture(width, height, 1, 0, format, D3DPOOL_DEFAULT, &HeightTexture, nullptr);
			}
			if (SUCCEEDED(hr))
				hr = Device->CreateTexture(width, height, 1, 0, format, D3DPOOL_SYSTEMMEM, &HeightStaging, nullptr);
			if (FAILED(hr))
			{
				Release_Com(HeightTexture);
				Release_Com(HeightStaging);
				HeightSource = nullptr;
				return false;
			}
			HeightSource = data;
			HeightWidth = width;
			HeightHeight = height;
			HeightCopy.assign(data, data + count);
			HeightCheckedFrame = FrameNumber;
			if (!Upload_Heights())
			{
				Release_Com(HeightTexture);
				Release_Com(HeightStaging);
				HeightSource = nullptr;
				return false;
			}
		}
		else if (FrameNumber - HeightCheckedFrame >= HEIGHT_CHECK_FRAMES)
		{
			HeightCheckedFrame = FrameNumber;
			if (memcmp(&HeightCopy[0], data, count) != 0)
			{
				HeightCopy.assign(data, data + count);
				Upload_Heights();
			}
		}
		HeightBorder = map->getBorderSizeInline();
		return true;
	}

	// --- Foam and caustic noise: cellular (Worley) noise on a wrapping grid of jittered points. ---

	struct Cells
	{
		int count;	// cells across the tile
		std::vector<float> points;	// one point per cell, in cell units within the cell

		Cells(int n, unsigned seed) : count(n), points(size_t(n) * n * 2)
		{
			for (size_t i = 0; i < points.size(); ++i)
			{
				seed = seed * 1664525u + 1013904223u;
				points[i] = 0.1f + 0.8f * float(seed >> 8) / 16777216.0f;
			}
		}

		// Distances from (x, y), in cell units, to the nearest and second nearest points.
		void Nearest(float x, float y, float &f1, float &f2) const
		{
			const int cx = int(floorf(x));
			const int cy = int(floorf(y));
			f1 = f2 = 1e9f;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int ix = ((cx + dx) % count + count) % count;
					const int iy = ((cy + dy) % count + count) % count;
					const float *p = &points[(size_t(iy) * count + ix) * 2];
					const float ox = float(cx + dx) + p[0] - x;
					const float oy = float(cy + dy) + p[1] - y;
					const float d = sqrtf(ox * ox + oy * oy);
					if (d < f1)
					{
						f2 = f1;
						f1 = d;
					}
					else if (d < f2)
						f2 = d;
				}
		}
	};

	float Saturate(float v)
	{
		return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}

	void Make_Foam(std::vector<float> &texels)
	{
		const Cells web_a(8, 777u), web_b(13, 4242u), bubbles_a(24, 99u), bubbles_b(40, 31337u);
		texels.resize(size_t(FOAM_SIZE) * FOAM_SIZE * 2);
		for (int y = 0; y < FOAM_SIZE; ++y)
		{
			for (int x = 0; x < FOAM_SIZE; ++x)
			{
				const float u = (x + 0.5f) / FOAM_SIZE;
				const float v = (y + 0.5f) / FOAM_SIZE;
				float f1, f2;
				// Lines where two cells meet, thin and bright.
				web_a.Nearest(u * web_a.count, v * web_a.count, f1, f2);
				const float line_a = 1.0f - Saturate((f2 - f1) / 0.14f);
				web_b.Nearest(u * web_b.count, v * web_b.count, f1, f2);
				const float line_b = 1.0f - Saturate((f2 - f1) / 0.18f);
				const float web = Saturate(line_a * line_a + 0.6f * line_b * line_b);
				// Round bubbles of two sizes.
				bubbles_a.Nearest(u * bubbles_a.count, v * bubbles_a.count, f1, f2);
				const float blob_a = Saturate(1.0f - f1 / 0.42f);
				bubbles_b.Nearest(u * bubbles_b.count, v * bubbles_b.count, f1, f2);
				const float blob_b = Saturate(1.0f - f1 / 0.38f);
				const float bubbles = Saturate(sqrtf(blob_a) * 0.8f + sqrtf(blob_b) * 0.5f);
				float *t = &texels[(size_t(y) * FOAM_SIZE + x) * 2];
				t[0] = web;
				t[1] = bubbles;
			}
		}
	}

	bool Create_Foam_Texture()
	{
		if (FoamTexture != nullptr)
			return true;
		IDirect3DTexture9 *staging = nullptr;
		if (FAILED(Device->CreateTexture(FOAM_SIZE, FOAM_SIZE, FOAM_LEVELS, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr)))
			return false;
		std::vector<float> level;
		Make_Foam(level);
		int size = FOAM_SIZE;
		for (int l = 0; l < FOAM_LEVELS; ++l)
		{
			D3DLOCKED_RECT rect;
			if (FAILED(staging->LockRect(l, &rect, nullptr, 0)))
			{
				staging->Release();
				return false;
			}
			for (int y = 0; y < size; ++y)
			{
				BYTE *row = (BYTE *)rect.pBits + size_t(y) * size_t(rect.Pitch);
				for (int x = 0; x < size; ++x)
				{
					const float *t = &level[(size_t(y) * size + x) * 2];
					row[x * 4 + 0] = 0;										// B
					row[x * 4 + 1] = BYTE(Saturate(t[1]) * 255.0f + 0.5f);	// G bubbles
					row[x * 4 + 2] = BYTE(Saturate(t[0]) * 255.0f + 0.5f);	// R web
					row[x * 4 + 3] = 255;
				}
			}
			staging->UnlockRect(l);
			if (size > 1)
			{
				const int half = size / 2;
				std::vector<float> next(size_t(half) * half * 2);
				for (int y = 0; y < half; ++y)
					for (int x = 0; x < half; ++x)
						for (int c = 0; c < 2; ++c)
						{
							float sum = 0.0f;
							for (int sy = 0; sy < 2; ++sy)
								for (int sx = 0; sx < 2; ++sx)
									sum += level[(size_t(y * 2 + sy) * size + (x * 2 + sx)) * 2 + c];
							next[(size_t(y) * half + x) * 2 + c] = sum * 0.25f;
						}
				level.swap(next);
				size = half;
			}
		}
		HRESULT hr = Device->CreateTexture(FOAM_SIZE, FOAM_SIZE, FOAM_LEVELS, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &FoamTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = Device->UpdateTexture(staging, FoamTexture);
		staging->Release();
		if (FAILED(hr))
		{
			Release_Com(FoamTexture);
			return false;
		}
		return true;
	}

	// --- Refraction: the render target as it is before the first water draw of the frame. ---

	bool Update_Refraction()
	{
		if (RefractionFrame == FrameNumber)
			return RefractionValid;
		RefractionFrame = FrameNumber;
		RefractionValid = false;

		IDirect3DSurface9 *target = nullptr;
		if (FAILED(Device->GetRenderTarget(0, &target)) || target == nullptr)
			return false;
		D3DSURFACE_DESC desc;
		target->GetDesc(&desc);
		if (RefractionTexture == nullptr || desc.Width != RefractionDesc.Width || desc.Height != RefractionDesc.Height
			|| desc.Format != RefractionDesc.Format)
		{
			Release_Com(RefractionSurface);
			Release_Com(RefractionTexture);
			HRESULT hr = Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &RefractionTexture, nullptr);
			if (SUCCEEDED(hr))
				hr = RefractionTexture->GetSurfaceLevel(0, &RefractionSurface);
			if (FAILED(hr))
			{
				Release_Com(RefractionSurface);
				Release_Com(RefractionTexture);
				target->Release();
				return false;
			}
			RefractionDesc = desc;
		}
		// Also resolves a multisampled target.
		RefractionValid = SUCCEEDED(Device->StretchRect(target, nullptr, RefractionSurface, nullptr, D3DTEXF_NONE));
		target->Release();
		return RefractionValid;
	}

	void Release_High_Quality()
	{
		Release_Com(HeightTexture);
		Release_Com(HeightStaging);
		HeightSource = nullptr;
		HeightCopy.clear();
		Release_Com(FoamTexture);
		Release_Com(RefractionSurface);
		Release_Com(RefractionTexture);
		RefractionFrame = ~0u;
		RefractionValid = false;
	}
}

void W3DWaterShader::Sky_Color(float rgb[3])
{
	// The ambient and first sun of the map, so the sky darkens at night.
	float light[3] = { 1.0f, 1.0f, 1.0f };
	if (TheGlobalData != nullptr)
	{
		light[0] = TheGlobalData->m_terrainAmbient[0].red + TheGlobalData->m_terrainDiffuse[0].red;
		light[1] = TheGlobalData->m_terrainAmbient[0].green + TheGlobalData->m_terrainDiffuse[0].green;
		light[2] = TheGlobalData->m_terrainAmbient[0].blue + TheGlobalData->m_terrainDiffuse[0].blue;
	}
	for (int i = 0; i < 3; ++i)
	{
		float v = SKY_COLOR[i] * light[i];
		rgb[i] = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
	}
}

bool W3DWaterShader::Wants_Reflection()
{
	if (!Get_Graphics_Options().WaterShader())
	{
		ReflectionReason = "the water shader is off";
		return false;
	}
	Device = DX9Wrapper::_Get_D3D_Device9();
	if (Device == nullptr || !Shaders_Supported())
	{
		ReflectionReason = "the water shader cannot run on this card";
		return false;
	}
	return true;
}

bool W3DWaterShader::Begin_Reflection(IDirect3DSurface9 **target, IDirect3DSurface9 **depth)
{
	Device = DX9Wrapper::_Get_D3D_Device9();
	if (Device == nullptr)
		return false;

	// WaterQuality 1: half the back buffer, sharp enough once the waves move it, at a quarter of
	// the cost. 2 and up: the full back buffer.
	IDirect3DSurface9 *back_buffer = nullptr;
	if (FAILED(Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) || back_buffer == nullptr)
	{
		ReflectionReason = "no back buffer";
		return false;
	}
	D3DSURFACE_DESC desc;
	back_buffer->GetDesc(&desc);
	back_buffer->Release();
	const unsigned divisor = (Get_Graphics_Options().WaterQuality >= 2) ? 1 : 2;
	int width = int(desc.Width / divisor);
	int height = int(desc.Height / divisor);
	if (width < 16)
		width = 16;
	if (height < 16)
		height = 16;

	if (ReflectionTexture == nullptr || width != ReflectionWidth || height != ReflectionHeight)
	{
		Release_Reflection();
		HRESULT hr = Device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ReflectionTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = ReflectionTexture->GetSurfaceLevel(0, &ReflectionSurface);
		// Its own depth buffer: the back buffer's may be multisampled, which a texture is not.
		if (SUCCEEDED(hr))
			hr = Device->CreateDepthStencilSurface(width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ReflectionDepth, nullptr);
		if (FAILED(hr))
		{
			if (ReflectionDepth == nullptr && ReflectionSurface != nullptr)
				hr = Device->CreateDepthStencilSurface(width, height, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &ReflectionDepth, nullptr);
			if (FAILED(hr))
			{
				Release_Reflection();
				ReflectionReason = "the reflection texture could not be created";
				return false;
			}
		}
		ReflectionWidth = width;
		ReflectionHeight = height;
	}

	*target = ReflectionSurface;
	*depth = ReflectionDepth;
	return true;
}

void W3DWaterShader::End_Reflection(float level)
{
	HasReflection = true;
	ReflectionLevel = level;
	ReflectionReason = nullptr;
}

void W3DWaterShader::Skip_Reflection(const char *why)
{
	ReflectionReason = why;
}

bool W3DWaterShader::Begin(Kind kind, float water_level, float river_width, float river_alpha)
{
	if (!Get_Graphics_Options().WaterShader())
		return Skip(Get_Graphics_Options().WaterQuality < 1 ? "WaterQuality is 0 in Options.ini" : "switched off by a debug key");
	if (TheGlobalData == nullptr)
		return Skip("no global data");
	Device = DX9Wrapper::_Get_D3D_Device9();
	if (Device == nullptr)
		return Skip("no device");

	// Only passes set up with the parity water pixel shader are ours.
	IDirect3DPixelShader9 *bound_ps = nullptr;
	Device->GetPixelShader(&bound_ps);
	if (bound_ps == nullptr)
		return Skip("the water pass uses no pixel shader (fixed-function water)");
	bound_ps->Release();

	IDirect3DVertexShader9 *bound_vs = nullptr;
	Device->GetVertexShader(&bound_vs);
	if (bound_vs != nullptr)
	{
		bound_vs->Release();
		return Skip("the pass has a vertex shader bound");
	}

	const DX9Caps *caps = DX9Wrapper::Get_Current_Caps();
	if (caps->Get_Vertex_Shader_Major_Version() < 3 || caps->Get_Pixel_Shader_Major_Version() < 3)
		return Fallback(REASON_NO_SM3, (caps->Get_Vertex_Shader_Major_Version() << 8) | caps->Get_Pixel_Shader_Major_Version());

	IDirect3DVertexShader9 *vertex_shader = ShaderLibrary::Peek_Vertex(ShaderLibrary::VS_WATER);
	IDirect3DPixelShader9 *pixel_shader = ShaderLibrary::Peek_Pixel(kind == RIVER ? ShaderLibrary::PS_WATER_RIVER : ShaderLibrary::PS_WATER_TRAPEZOID);
	if (vertex_shader == nullptr || pixel_shader == nullptr)
		return Fallback(REASON_NO_SHADER, unsigned(kind));

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

	if (!Create_Wave_Texture())
		return false;	// Create_Wave_Texture records the reason

	// WaterQuality 2: the water seen into, once its textures are ready; else as at 1.
	bool high_quality = false;
	bool has_heights = false;
	if (Get_Graphics_Options().WaterQuality >= 2)
	{
		IDirect3DPixelShader9 *seen_into = ShaderLibrary::Peek_Pixel(kind == RIVER ? ShaderLibrary::PS_WATER_RIVER_HQ : ShaderLibrary::PS_WATER_TRAPEZOID_HQ);
		if (seen_into == nullptr)
			HighQualityReason = "its shaders failed to load";
		else if (!Create_Foam_Texture())
			HighQualityReason = "the foam texture could not be created";
		else if (!Update_Refraction())
			HighQualityReason = "the scene could not be copied for the refraction";
		else
		{
			pixel_shader = seen_into;
			high_quality = true;
			has_heights = Update_Heights();
			HighQualityReason = has_heights ? nullptr : "no terrain heights (water drawn as deep)";
			if (has_heights)
				++FrameHighQualityPasses;
		}
	}

	D3DMATRIX world, view, projection, world_view, world_view_projection, noise, shroud;
	Device->GetTransform(D3DTS_WORLD, &world);
	Device->GetTransform(D3DTS_VIEW, &view);
	Device->GetTransform(D3DTS_PROJECTION, &projection);
	Device->GetTransform(D3DTS_TEXTURE2, &noise);
	Device->GetTransform(D3DTS_TEXTURE3, &shroud);
	Multiply(world_view, world, view);
	Multiply(world_view_projection, world_view, projection);

	const double seconds = double(timeGetTime()) * 0.001;
	Float4 waves =
	{
		Wrap(seconds * WAVE_SPEED_A[0]), Wrap(seconds * WAVE_SPEED_A[1]),
		Wrap(seconds * WAVE_SPEED_B[0]), Wrap(seconds * WAVE_SPEED_B[1]),
	};

	Float4 vs[WATER_VS_COUNT];
	memset(vs, 0, sizeof(vs));
	Columns(&vs[WATER_VS_WVP], world_view_projection, 4);
	Columns(&vs[WATER_VS_WORLD], world, 3);
	Columns(&vs[WATER_VS_WORLDVIEW], world_view, 3);
	Columns(&vs[WATER_VS_NOISE], noise, 2);
	Columns(&vs[WATER_VS_SHROUD], shroud, 2);
	vs[WATER_VS_OPTIONS].x = range_fog;
	vs[WATER_VS_WAVES] = waves;
	vs[WATER_VS_WAVE_SCALE].x = WAVE_SCALE_A;
	vs[WATER_VS_WAVE_SCALE].y = WAVE_SCALE_B;
	vs[WATER_VS_WAVE_SCALE].z = (kind == RIVER) ? 1.0f : 0.0f;
	vs[WATER_VS_WAVE_SCALE].w = river_width;

	// The sun: the first terrain light, which lights the water's vertex colours too.
	Float4 sun = { 0.0f, 0.0f, 1.0f, 0.0f };
	Float4 sun_color = { 0.0f, 0.0f, 0.0f, SPECULAR_POWER };
	if (TheGlobalData->m_numGlobalLights > 0)
	{
		const Coord3D &position = TheGlobalData->m_terrainLightPos[0];
		float d[3] = { -position.x, -position.y, -position.z };
		Normalize(d);
		sun.x = d[0];
		sun.y = d[1];
		sun.z = d[2];
		sun_color.x = TheGlobalData->m_terrainDiffuse[0].red;
		sun_color.y = TheGlobalData->m_terrainDiffuse[0].green;
		sun_color.z = TheGlobalData->m_terrainDiffuse[0].blue;
	}

	// The reflection is right only for water at the height it was mirrored about.
	const bool use_reflection = HasReflection && ReflectionTexture != nullptr && fabsf(water_level - ReflectionLevel) <= MIRROR_LEVEL_TOLERANCE;
	float sky[3];
	Sky_Color(sky);

	Float4 ps[WATER_PS_COUNT];
	memset(ps, 0, sizeof(ps));
	ps[WATER_PS_SUN] = sun;
	ps[WATER_PS_SUN_COLOR] = sun_color;
	ps[WATER_PS_CAMERA] = Camera_Position(view);
	ps[WATER_PS_FRESNEL].x = FRESNEL_BIAS;
	ps[WATER_PS_FRESNEL].y = FRESNEL_SCALE;
	ps[WATER_PS_FRESNEL].z = FRESNEL_POWER;
	ps[WATER_PS_FRESNEL].w = FRESNEL_MAX;
	ps[WATER_PS_OPTIONS].x = REFLECTION_DISTORTION;
	ps[WATER_PS_OPTIONS].y = WAVE_NORMAL_STRENGTH;
	ps[WATER_PS_OPTIONS].z = SPECULAR_STRENGTH;
	ps[WATER_PS_FOG] = fog;
	ps[WATER_PS_FOGCOLOR] = Color_Value(D3DCOLOR(Render_State(D3DRS_FOGCOLOR)));
	ps[WATER_PS_SKY].x = sky[0];
	ps[WATER_PS_SKY].y = sky[1];
	ps[WATER_PS_SKY].z = sky[2];
	ps[WATER_PS_SKY].w = use_reflection ? 1.0f : 0.0f;
	ps[WATER_PS_RIVER].x = river_alpha;
	ps[WATER_PS_WAVE_LOOK].x = WAVE_SHADING;
	ps[WATER_PS_WAVE_LOOK].y = TEXTURE_WOBBLE;
	ps[WATER_PS_WAVE_LOOK].z = REFLECTION_OPACITY;

	if (high_quality)
	{
		ps[WATER_PS_FRESNEL].x = HQ_FRESNEL_BIAS;
		ps[WATER_PS_FRESNEL].y = HQ_FRESNEL_SCALE;
		ps[WATER_PS_FRESNEL].z = HQ_FRESNEL_POWER;
		ps[WATER_PS_FRESNEL].w = HQ_FRESNEL_MAX;
		ps[WATER_PS_SUN_COLOR].w = HQ_SPECULAR_POWER;
		ps[WATER_PS_OPTIONS].z = HQ_SPECULAR_STRENGTH;

		// Clip xy / w to the refraction uv: the viewport within the render target, and D3D9's half
		// texel between pixel and texel centres.
		D3DVIEWPORT9 viewport;
		Device->GetViewport(&viewport);
		const float target_width = float(RefractionDesc.Width);
		const float target_height = float(RefractionDesc.Height);
		ps[WATER_PS_SCREEN].x = 0.5f * viewport.Width / target_width;
		ps[WATER_PS_SCREEN].y = -0.5f * viewport.Height / target_height;
		ps[WATER_PS_SCREEN].z = (viewport.X + 0.5f * viewport.Width + 0.5f) / target_width;
		ps[WATER_PS_SCREEN].w = (viewport.Y + 0.5f * viewport.Height + 0.5f) / target_height;

		// World xy to the height texel: vertex k of the height map is at (k - border) * MAP_XY_FACTOR.
		if (has_heights)
		{
			ps[WATER_PS_HEIGHT].x = 1.0f / (MAP_XY_FACTOR * HeightWidth);
			ps[WATER_PS_HEIGHT].y = 1.0f / (MAP_XY_FACTOR * HeightHeight);
			ps[WATER_PS_HEIGHT].z = (HeightBorder + 0.5f) / HeightWidth;
			ps[WATER_PS_HEIGHT].w = (HeightBorder + 0.5f) / HeightHeight;
		}
		ps[WATER_PS_BODY].x = 255.0f * MAP_HEIGHT_SCALE;
		ps[WATER_PS_BODY].y = has_heights ? 1.0f : 0.0f;
		ps[WATER_PS_BODY].z = float(fmod(seconds * SHORE_WAVE_SPEED, 6.28318530718));
		ps[WATER_PS_BODY].w = REFRACTION_DISTORTION;
		ps[WATER_PS_ABSORB].x = ABSORPTION[0];
		ps[WATER_PS_ABSORB].y = ABSORPTION[1];
		ps[WATER_PS_ABSORB].z = ABSORPTION[2];
		ps[WATER_PS_ABSORB].w = DEEP_COLOR_SCALE;
		ps[WATER_PS_FOAM].x = FOAM_DEPTH;
		ps[WATER_PS_FOAM].y = FOAM_STRENGTH;
		ps[WATER_PS_FOAM].z = CAUSTIC_STRENGTH;
		ps[WATER_PS_FOAM].w = CAUSTIC_DEPTH;
		ps[WATER_PS_AMBIENT].x = TheGlobalData->m_terrainAmbient[0].red;
		ps[WATER_PS_AMBIENT].y = TheGlobalData->m_terrainAmbient[0].green;
		ps[WATER_PS_AMBIENT].z = TheGlobalData->m_terrainAmbient[0].blue;
		ps[WATER_PS_AMBIENT].w = HQ_BROAD_GLINT;
		ps[WATER_PS_WAVE_LOOK].x = HQ_WAVE_SHADING;
		ps[WATER_PS_WAVE_MIX].x = SWELL_SIZE;
		ps[WATER_PS_WAVE_MIX].y = SWELL_STRENGTH;
		ps[WATER_PS_WAVE_MIX].z = RIPPLE_STRENGTH;
	}

	Device->SetVertexShaderConstantF(0, &vs[0].x, WATER_VS_COUNT);
	Device->SetPixelShaderConstantF(0, &ps[0].x, WATER_PS_COUNT);

	// The pass's own pixel shader goes back in End(). GetPixelShader adds the reference End()
	// releases.
	Device->GetPixelShader(&SavedPixelShader);
	Device->SetVertexShader(vertex_shader);
	Device->SetPixelShader(pixel_shader);

	SlotCount = 0;
	Bind_Sampler(WAVE_SAMPLER, WaveTexture, WaveSamplerValues);
	Bind_Sampler(REFLECTION_SAMPLER, use_reflection ? ReflectionTexture : nullptr, ReflectionSamplerValues);
	if (high_quality)
	{
		Bind_Sampler(REFRACTION_SAMPLER, RefractionTexture, ReflectionSamplerValues);
		Bind_Sampler(HEIGHT_SAMPLER, has_heights ? HeightTexture : nullptr, ReflectionSamplerValues);
		Bind_Sampler(FOAM_SAMPLER, FoamTexture, WaveSamplerValues);

		Device->GetSamplerState(0, D3DSAMP_MIPFILTER, &SavedWaterMip);
		RestoreWaterMip = SavedWaterMip == D3DTEXF_NONE;
		if (RestoreWaterMip)
			Device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);

		// The shader works out the whole colour, so plain alpha blending, whatever the classic pass
		// used (additive water, or the soft edge from destination alpha).
		RestoreBlend = true;
		const DWORD blend[3] = { TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA };
		for (int i = 0; i < 3; ++i)
		{
			SavedBlend[i] = Render_State(BlendStates[i]);
			Device->SetRenderState(BlendStates[i], blend[i]);
		}
	}

	// The pixel shader fogs itself. The wrapper's cache keeps TRUE, and End() puts it back.
	RestoreFog = fog_enabled;
	if (fog_enabled)
		Device->SetRenderState(D3DRS_FOGENABLE, FALSE);

	++FramePasses;
	return true;
}

void W3DWaterShader::End()
{
	Device->SetVertexShader(nullptr);
	Device->SetPixelShader(SavedPixelShader);
	if (SavedPixelShader != nullptr)
	{
		SavedPixelShader->Release();
		SavedPixelShader = nullptr;
	}

	while (SlotCount > 0)
	{
		SamplerSlot &slot = Slots[--SlotCount];
		Device->SetTexture(slot.stage, slot.saved_texture);
		Release_Com(slot.saved_texture);
		for (int i = 0; i < SAMPLER_STATE_COUNT; ++i)
			Device->SetSamplerState(slot.stage, SamplerStates[i], slot.saved_states[i]);
	}

	if (RestoreWaterMip)
	{
		Device->SetSamplerState(0, D3DSAMP_MIPFILTER, SavedWaterMip);
		RestoreWaterMip = false;
	}
	if (RestoreBlend)
	{
		for (int i = 0; i < 3; ++i)
			Device->SetRenderState(BlendStates[i], SavedBlend[i]);
		RestoreBlend = false;
	}

	if (RestoreFog)
	{
		Device->SetRenderState(D3DRS_FOGENABLE, TRUE);
		RestoreFog = false;
	}
}

void W3DWaterShader::Release()
{
	if (WaveTexture != nullptr)
	{
		WaveTexture->Release();
		WaveTexture = nullptr;
	}
	Release_Reflection();
	HasReflection = false;
	Release_High_Quality();
}

void W3DWaterShader::New_Frame()
{
	++FrameNumber;
	LastFramePasses = FramePasses;
	FramePasses = 0;
	LastHighQualityPasses = FrameHighQualityPasses;
	FrameHighQualityPasses = 0;
	LastHighQualityReason = HighQualityReason;
	LastHasReflection = HasReflection;
	LastReflectionLevel = ReflectionLevel;
	LastReflectionReason = ReflectionReason;
	HasReflection = false;
	ReflectionReason = "not drawn";
}

unsigned W3DWaterShader::Last_Frame_Passes()
{
	return LastFramePasses;
}

const char *W3DWaterShader::Last_Reason()
{
	return LastReason;
}

bool W3DWaterShader::Last_Reflection(float *level, int *width, int *height, const char **why_not)
{
	*level = LastReflectionLevel;
	*width = ReflectionWidth;
	*height = ReflectionHeight;
	*why_not = LastReflectionReason != nullptr ? LastReflectionReason : "";
	return LastHasReflection;
}

const char *W3DWaterShader::Last_Seen_Into()
{
	if (Get_Graphics_Options().WaterQuality < 2)
		return "WaterQuality is below 2";
	if (LastHighQualityPasses > 0)
		return nullptr;
	return LastHighQualityReason != nullptr ? LastHighQualityReason : "no water drawn";
}

static_assert(WATER_VS_WVP == 0 && WATER_VS_WAVE_SCALE + 1 == WATER_VS_COUNT, "water vertex constants are uploaded as one block from c0");
static_assert(WATER_PS_SUN == 0 && WATER_PS_WAVE_MIX + 1 == WATER_PS_COUNT, "water pixel constants are uploaded as one block from c0");
