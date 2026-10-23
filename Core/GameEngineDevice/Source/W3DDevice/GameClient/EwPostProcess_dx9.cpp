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

#include "W3DDevice/GameClient/EwPostProcess_dx9.h"
#include "Common/GlobalData.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "Utility/stringex.h"
#include "WW3D2/dx9wrapper.h"
#include "WW3D2/ewshaders_dx9.h"
#include "WW3D2/ewgraphicsoptions_dx9.h"
#include "WWDebug/wwdebug.h"
#include "ew_constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// The render target is set on the device directly, not with DX8Wrapper::Set_Render_Target: the
// water reflection and the projected shadows switch targets with the wrapper, which saves the
// device's current target (the scene texture) and restores it afterwards.
//
// The post passes set their states on the device too and put every state back with a state
// block, so the wrapper's state cache stays true.

namespace
{
	// Bloom strength, until it moves to INI data.
	const float BLOOM_THRESHOLD = 0.7f;		// scene brightness (luma) where the glow starts
	const float BLOOM_STRENGTH = 0.8f;
	const float BLOOM_WEIGHTS[3] = { 0.45f, 0.35f, 0.30f };	// the 1/4, 1/8 and 1/16 size levels

	// Tone mapping strength, until it moves to INI data. Near 0 the curve changes nothing; larger
	// values give deeper shadows, brighter mid-tones and softer highlights.
	const float TONE_EXPOSURE = 0.6f;

	// Colour grading: a 256x16 TGA strip (see post.hlsl). The map's own LUT wins over the default.
	const char *const LUT_DEFAULT_FILE = "Art\\Textures\\EwColorGrading.tga";
	const char *const LUT_MAP_FILE = "ColorGrading.tga";	// in the map's folder, beside map.str
	const int LUT_WIDTH = 256;
	const int LUT_HEIGHT = 16;

	enum Reason
	{
		REASON_OFF,
		REASON_DEBUG_OFF,
		REASON_NO_SHADER,
		REASON_NO_TARGET,
		REASON_CREATE_FAILED,
		REASON_SET_FAILED,
		REASON_NOT_DRAWN,
		REASON_EFFECT_OFF,
		REASON_BLOOM_CREATE_FAILED,
		REASON_TONE_DEBUG_OFF,
		REASON_GRADE_DEBUG_OFF,
		REASON_LUT_CREATE_FAILED,
		REASON_FXAA_DEBUG_OFF,
		REASON_FXAA_CREATE_FAILED,
		REASON_COUNT
	};

	const char *const ReasonText[] =
	{
		"no post effect is on in Options.ini",
		"switched off with Ctrl+Shift+F7",
		"the card has no vs_3_0/ps_3_0, or the post shaders failed to load",
		"the device has no render target or depth buffer",
		"the scene texture could not be created (HRESULT)",
		"the scene texture could not be set as the render target (HRESULT)",
		"no 3D view was drawn",
		"off in Options.ini",
		"the bloom textures could not be created (HRESULT)",
		"switched off with Ctrl+Shift+F5",
		"switched off with Ctrl+Shift+F6",
		"the colour LUT texture could not be created (HRESULT)",
		"switched off with Ctrl+Shift+F4",
		"the FXAA texture could not be created (HRESULT)",
	};
	static_assert(sizeof(ReasonText) / sizeof(ReasonText[0]) == REASON_COUNT, "ReasonText must match Reason");

	bool Logged[REASON_COUNT];

	// Per effect: passes drawn this frame and the last, and why the effect did not run.
	struct EffectStats
	{
		unsigned Passes;
		unsigned LastPasses;
		const char *Reason;
		const char *LastReason;
	};
	EffectStats Stats[EwPostProcess::EFFECT_COUNT];

	IDirect3DTexture9 *SceneTexture = nullptr;		// what the post passes read
	IDirect3DSurface9 *SceneSurface = nullptr;		// level 0 of SceneTexture
	IDirect3DSurface9 *MsaaSurface = nullptr;		// with multisampling the scene draws here first
	IDirect3DStateBlock9 *SavedStates = nullptr;	// all device states, around the post passes
	D3DSURFACE_DESC TargetDesc;						// the back buffer the textures were made for
	bool CreateFailed = false;						// do not try again until Release()

	// Bloom: two textures at 1/4, 1/8 and 1/16 of the back buffer size. The blur runs across from
	// [0] into [1] and down from [1] back into [0], so [0] holds each level's result.
	const int BLOOM_LEVELS = 3;
	struct BloomLevel
	{
		IDirect3DTexture9 *Texture[2];
		IDirect3DSurface9 *Surface[2];
		int Width;
		int Height;
	};
	BloomLevel Bloom[BLOOM_LEVELS];
	bool BloomFailed = false;

	// Colour grading. The pixels stay in memory, so a device reset only uploads them again.
	unsigned LutPixels[LUT_WIDTH * LUT_HEIGHT];	// A8R8G8B8, rows from the top
	bool LutChosen = false;					// LutPixels hold the LUT for LutMap
	bool LutUploaded = false;				// LutTexture holds LutPixels
	// Plain arrays, not AsciiString: a static AsciiString would free its text after the game's
	// memory manager has shut down.
	char LutMap[_MAX_PATH];					// the map the LUT was chosen for
	char LutFile[_MAX_PATH];				// the file in LutPixels; empty for the built-in neutral LUT
	IDirect3DTexture9 *LutTexture = nullptr;
	bool LutFailed = false;

	// FXAA after bloom, tone mapping or colour grading: the final pass draws here, the size and
	// format of the scene texture, and FXAA reads it. FXAA alone reads the scene texture.
	IDirect3DTexture9 *FinalTexture = nullptr;
	IDirect3DSurface9 *FinalSurface = nullptr;
	bool FinalFailed = false;

	// Set between Begin_Scene and End_Scene.
	IDirect3DSurface9 *BackBuffer = nullptr;
	IDirect3DSurface9 *DepthBuffer = nullptr;

	void Log_Once(Reason reason, unsigned value)
	{
		if (!Logged[reason] && reason != REASON_OFF && reason != REASON_DEBUG_OFF && reason != REASON_EFFECT_OFF
			&& reason != REASON_TONE_DEBUG_OFF && reason != REASON_GRADE_DEBUG_OFF && reason != REASON_FXAA_DEBUG_OFF)
		{
			Logged[reason] = true;
			WWDEBUG_SAY(("EwPostProcess: %s, value 0x%x (logged once)", ReasonText[reason], value));
		}
	}

	void Set_Reason(EwPostProcess::Effect effect, Reason reason)
	{
		Stats[effect].Reason = ReasonText[reason];
	}

	// The scene texture is not used, so no effect runs.
	bool Fail(Reason reason, unsigned value = 0)
	{
		for (int i = 0; i < EwPostProcess::EFFECT_COUNT; ++i)
			Stats[i].Reason = ReasonText[reason];
		Log_Once(reason, value);
		return false;
	}

	bool Same_Target(const D3DSURFACE_DESC &a, const D3DSURFACE_DESC &b)
	{
		return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format
			&& a.MultiSampleType == b.MultiSampleType && a.MultiSampleQuality == b.MultiSampleQuality;
	}

	template <class T> void Release_Com(T *&p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	void Release_Bloom()
	{
		for (int i = 0; i < BLOOM_LEVELS; ++i)
		{
			for (int j = 0; j < 2; ++j)
			{
				Release_Com(Bloom[i].Surface[j]);
				Release_Com(Bloom[i].Texture[j]);
			}
		}
		BloomFailed = false;
	}

	void Release_Targets()
	{
		Release_Com(MsaaSurface);
		Release_Com(SceneSurface);
		Release_Com(SceneTexture);
		Release_Com(FinalSurface);
		Release_Com(FinalTexture);
		FinalFailed = false;
		Release_Bloom();
	}

	void Release_Saved_Targets()
	{
		Release_Com(BackBuffer);
		Release_Com(DepthBuffer);
	}

	// Makes the scene texture (and multisampled surface) match the back buffer.
	HRESULT Create_Targets(IDirect3DDevice9 *device, const D3DSURFACE_DESC &desc)
	{
		Release_Targets();
		HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
			D3DPOOL_DEFAULT, &SceneTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = SceneTexture->GetSurfaceLevel(0, &SceneSurface);
		if (SUCCEEDED(hr) && desc.MultiSampleType != D3DMULTISAMPLE_NONE)
		{
			hr = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, desc.MultiSampleType,
				desc.MultiSampleQuality, FALSE, &MsaaSurface, nullptr);
		}
		if (FAILED(hr))
			Release_Targets();
		else
			TargetDesc = desc;
		return hr;
	}

	// Creates the bloom textures for the current scene texture size, once.
	bool Bloom_Ready(IDirect3DDevice9 *device)
	{
		if (Bloom[0].Texture[0] != nullptr)
			return true;
		if (BloomFailed)
		{
			Set_Reason(EwPostProcess::BLOOM, REASON_BLOOM_CREATE_FAILED);
			return false;
		}

		int w = (int)TargetDesc.Width;
		int h = (int)TargetDesc.Height;
		HRESULT hr = S_OK;
		for (int i = 0; i < BLOOM_LEVELS && SUCCEEDED(hr); ++i)
		{
			const int div = i == 0 ? 4 : 2;
			w = (w + div - 1) / div;
			h = (h + div - 1) / div;
			Bloom[i].Width = w;
			Bloom[i].Height = h;
			for (int j = 0; j < 2 && SUCCEEDED(hr); ++j)
			{
				hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
					&Bloom[i].Texture[j], nullptr);
				if (SUCCEEDED(hr))
					hr = Bloom[i].Texture[j]->GetSurfaceLevel(0, &Bloom[i].Surface[j]);
			}
		}
		if (FAILED(hr))
		{
			Release_Bloom();
			BloomFailed = true;
			Set_Reason(EwPostProcess::BLOOM, REASON_BLOOM_CREATE_FAILED);
			Log_Once(REASON_BLOOM_CREATE_FAILED, hr);
			return false;
		}
		return true;
	}

	// Creates the texture the final pass draws into before FXAA, once.
	bool Final_Ready(IDirect3DDevice9 *device)
	{
		if (FinalTexture != nullptr)
			return true;
		HRESULT hr = FinalFailed ? E_FAIL : device->CreateTexture(TargetDesc.Width, TargetDesc.Height, 1,
			D3DUSAGE_RENDERTARGET, TargetDesc.Format, D3DPOOL_DEFAULT, &FinalTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = FinalTexture->GetSurfaceLevel(0, &FinalSurface);
		if (FAILED(hr))
		{
			Release_Com(FinalSurface);
			Release_Com(FinalTexture);
			if (!FinalFailed)
				Log_Once(REASON_FXAA_CREATE_FAILED, hr);
			FinalFailed = true;
			Set_Reason(EwPostProcess::FXAA, REASON_FXAA_CREATE_FAILED);
			return false;
		}
		return true;
	}

	// The identity LUT: grading with it changes nothing.
	void Neutral_Lut(unsigned *pixels)
	{
		for (int y = 0; y < LUT_HEIGHT; ++y)
		{
			for (int x = 0; x < LUT_WIDTH; ++x)
			{
				const unsigned r = (unsigned)(x % 16) * 17;
				const unsigned g = (unsigned)y * 17;
				const unsigned b = (unsigned)(x / 16) * 17;
				pixels[y * LUT_WIDTH + x] = 0xff000000u | (r << 16) | (g << 8) | b;
			}
		}
	}

	// Reads a LUT_WIDTH x LUT_HEIGHT TGA, 24 or 32 bits, uncompressed or RLE, through the game's
	// file system (so it can be in a .big file). Alpha is ignored.
	bool Read_Lut_File(const char *path, unsigned *pixels)
	{
		File *file = TheFileSystem ? TheFileSystem->openFile(path, File::READ | File::BINARY) : nullptr;
		if (file == nullptr)
			return false;
		const int size = file->size();
		std::vector<unsigned char> data(size > 0 ? size : 1);
		const int read = size > 0 ? file->read(&data[0], size) : 0;
		file->close();

		const unsigned char *h = &data[0];
		if (size < 18 || read != size)
		{
			WWDEBUG_SAY(("EwPostProcess: colour LUT %s could not be read", path));
			return false;
		}
		const int id_length = h[0];
		const int colormap_type = h[1];
		const int type = h[2];
		const int width = h[12] | (h[13] << 8);
		const int height = h[14] | (h[15] << 8);
		const int depth = h[16];
		const int descriptor = h[17];
		if (colormap_type != 0 || (type != 2 && type != 10) || (depth != 24 && depth != 32)
			|| width != LUT_WIDTH || height != LUT_HEIGHT)
		{
			WWDEBUG_SAY(("EwPostProcess: colour LUT %s must be a %dx%d true-colour TGA (24 or 32 bits), it is %dx%d type %d, %d bits",
				path, LUT_WIDTH, LUT_HEIGHT, width, height, type, depth));
			return false;
		}

		const size_t bytes = (size_t)depth / 8;
		const int count = width * height;
		size_t pos = 18 + (size_t)id_length;
		int i = 0;
		bool short_data = false;
		while (i < count && !short_data)
		{
			int run = 1;
			bool repeat = false;
			if (type == 10)
			{
				if (pos >= data.size())
				{
					short_data = true;
					break;
				}
				const unsigned char packet = data[pos++];
				run = (packet & 0x7f) + 1;
				repeat = (packet & 0x80) != 0;
			}
			unsigned pixel = 0;
			for (int k = 0; k < run && i < count; ++k, ++i)
			{
				if (k == 0 || !repeat)
				{
					if (pos + bytes > data.size())
					{
						short_data = true;
						break;
					}
					pixel = 0xff000000u | ((unsigned)data[pos + 2] << 16) | ((unsigned)data[pos + 1] << 8) | data[pos];
					pos += bytes;
				}
				// TGA rows go from the bottom unless descriptor bit 5 is set; bit 4 mirrors x.
				const int fx = i % width;
				const int fy = i / width;
				const int x = (descriptor & 0x10) ? width - 1 - fx : fx;
				const int y = (descriptor & 0x20) ? fy : height - 1 - fy;
				pixels[y * width + x] = pixel;
			}
		}
		if (short_data || i < count)
		{
			WWDEBUG_SAY(("EwPostProcess: colour LUT %s is cut short", path));
			return false;
		}
		return true;
	}

	// Picks the LUT for the map: the map's own, else the default file, else the neutral one.
	void Choose_Lut(const char *map)
	{
		strlcpy(LutMap, map, sizeof(LutMap));
		LutFile[0] = '\0';
		LutChosen = true;
		LutUploaded = false;

		const char *slash = strrchr(map, '\\');
		const char *forward = strrchr(map, '/');
		if (forward != nullptr && (slash == nullptr || forward > slash))
			slash = forward;
		if (slash != nullptr)
		{
			char path[_MAX_PATH];
			snprintf(path, sizeof(path), "%.*s%s", (int)(slash - map + 1), map, LUT_MAP_FILE);
			if (Read_Lut_File(path, LutPixels))
			{
				strlcpy(LutFile, path, sizeof(LutFile));
				return;
			}
		}
		if (Read_Lut_File(LUT_DEFAULT_FILE, LutPixels))
		{
			strlcpy(LutFile, LUT_DEFAULT_FILE, sizeof(LutFile));
			return;
		}
		Neutral_Lut(LutPixels);
	}

	// Makes the LUT texture hold the current map's LUT.
	bool Lut_Ready(IDirect3DDevice9 *device)
	{
		const char *map = TheGlobalData ? TheGlobalData->m_mapName.str() : "";
		if (!LutChosen || strcmp(map, LutMap) != 0)
			Choose_Lut(map);
		if (LutFailed)
		{
			Set_Reason(EwPostProcess::COLOR_GRADING, REASON_LUT_CREATE_FAILED);
			return false;
		}

		HRESULT hr = S_OK;
		if (LutTexture == nullptr)
		{
			hr = device->CreateTexture(LUT_WIDTH, LUT_HEIGHT, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
				&LutTexture, nullptr);
			LutUploaded = false;
		}
		if (SUCCEEDED(hr) && !LutUploaded)
		{
			D3DLOCKED_RECT rect;
			hr = LutTexture->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD);
			if (SUCCEEDED(hr))
			{
				for (int y = 0; y < LUT_HEIGHT; ++y)
					memcpy((unsigned char *)rect.pBits + y * rect.Pitch, LutPixels + y * LUT_WIDTH, LUT_WIDTH * 4);
				LutTexture->UnlockRect(0);
				LutUploaded = true;
			}
		}
		if (FAILED(hr))
		{
			Release_Com(LutTexture);
			LutFailed = true;
			Set_Reason(EwPostProcess::COLOR_GRADING, REASON_LUT_CREATE_FAILED);
			Log_Once(REASON_LUT_CREATE_FAILED, hr);
			return false;
		}
		return true;
	}

	float Filmic_Curve(float x)
	{
		return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
	}

	struct PostVertex
	{
		float x, y, z;
		float u, v;
	};
	const DWORD POST_FVF = D3DFVF_XYZ | D3DFVF_TEX1;

	// Draws a quad over the viewport (x, y, width, height) of the current target with the uv
	// rectangle u0,v0 - u1,v1. D3D9 puts pixel centres on whole coordinates, half a pixel off the
	// texel centres, so the quad moves half a pixel up and left.
	void Draw_Quad(IDirect3DDevice9 *device, int x, int y, int width, int height, float u0, float v0, float u1, float v1)
	{
		D3DVIEWPORT9 viewport = { (DWORD)x, (DWORD)y, (DWORD)width, (DWORD)height, 0.0f, 1.0f };
		device->SetViewport(&viewport);

		const float dx = 1.0f / (float)width;
		const float dy = 1.0f / (float)height;
		const PostVertex quad[4] =
		{
			{ -1.0f - dx,  1.0f + dy, 0.0f, u0, v0 },
			{  1.0f - dx,  1.0f + dy, 0.0f, u1, v0 },
			{ -1.0f - dx, -1.0f + dy, 0.0f, u0, v1 },
			{  1.0f - dx, -1.0f + dy, 0.0f, u1, v1 },
		};
		device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PostVertex));
		DX8Wrapper::Increment_DX8_CallCount();
	}

	void Set_Texture(IDirect3DDevice9 *device, int stage, IDirect3DTexture9 *texture, bool linear)
	{
		const DWORD filter = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		device->SetTexture(stage, texture);
		device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
		device->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
		device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
	}

	void Set_Texel(IDirect3DDevice9 *device, float x, float y, float step_x, float step_y)
	{
		const float texel[4] = { x, y, step_x, step_y };
		device->SetPixelShaderConstantF(EW_POST_PS_TEXEL, texel, 1);
	}

	// One pass into a whole bloom texture, reading source with linear filtering.
	void Bloom_Pass(IDirect3DDevice9 *device, EwShaders::PixelShaderId shader, IDirect3DTexture9 *source,
		IDirect3DSurface9 *dest, int width, int height)
	{
		device->SetRenderTarget(0, dest);
		device->SetPixelShader(EwShaders::Peek_Pixel(shader));
		Set_Texture(device, 0, source, true);
		Draw_Quad(device, 0, 0, width, height, 0.0f, 0.0f, 1.0f, 1.0f);
		++Stats[EwPostProcess::BLOOM].Passes;
	}

	// Builds the three blurred bloom levels from the scene texture. Ends with the back buffer and
	// its depth buffer bound again.
	void Draw_Bloom(IDirect3DDevice9 *device)
	{
		const float params[4] = { BLOOM_THRESHOLD, 1.0f / (1.0f - BLOOM_THRESHOLD), BLOOM_STRENGTH, 0.0f };
		const float weights[4] = { BLOOM_WEIGHTS[0], BLOOM_WEIGHTS[1], BLOOM_WEIGHTS[2], 0.0f };
		device->SetPixelShaderConstantF(EW_POST_PS_BLOOM, params, 1);
		device->SetPixelShaderConstantF(EW_POST_PS_BLOOM_WEIGHTS, weights, 1);

		// The bloom textures have no multisampling, so no (possibly multisampled) depth buffer may
		// be bound with them.
		device->SetDepthStencilSurface(nullptr);

		IDirect3DTexture9 *source = SceneTexture;
		int source_w = (int)TargetDesc.Width;
		int source_h = (int)TargetDesc.Height;
		for (int i = 0; i < BLOOM_LEVELS; ++i)
		{
			BloomLevel &level = Bloom[i];
			Set_Texel(device, 1.0f / source_w, 1.0f / source_h, 0.0f, 0.0f);
			Bloom_Pass(device, i == 0 ? EwShaders::PS_POST_BRIGHT : EwShaders::PS_POST_DOWN, source,
				level.Surface[0], level.Width, level.Height);

			Set_Texel(device, 0.0f, 0.0f, 1.0f / level.Width, 0.0f);
			Bloom_Pass(device, EwShaders::PS_POST_BLUR, level.Texture[0], level.Surface[1], level.Width, level.Height);
			Set_Texel(device, 0.0f, 0.0f, 0.0f, 1.0f / level.Height);
			Bloom_Pass(device, EwShaders::PS_POST_BLUR, level.Texture[1], level.Surface[0], level.Width, level.Height);

			source = level.Texture[0];
			source_w = level.Width;
			source_h = level.Height;
		}

		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
	}

	// Whether a post effect runs this frame; sets the reason if not.
	bool Effect_On(EwPostProcess::Effect effect, bool option, bool debug_off, Reason debug_reason,
		EwShaders::PixelShaderId shader)
	{
		if (!option)
			Set_Reason(effect, REASON_EFFECT_OFF);
		else if (debug_off)
			Set_Reason(effect, debug_reason);
		else if (EwShaders::Peek_Pixel(shader) == nullptr)
			Set_Reason(effect, REASON_NO_SHADER);
		else
			return true;
		return false;
	}
}

bool EwPostProcess::Begin_Scene(float clear_alpha)
{
	if (BackBuffer != nullptr)
		return false;	// already rendering into the scene texture

	const EwGraphicsOptions &o = EwGraphics();
	if (!o.PostProcess())
		return Fail(o.PostProcessWanted() ? REASON_DEBUG_OFF : REASON_OFF);

	if (EwShaders::Peek_Vertex(EwShaders::VS_POST) == nullptr || EwShaders::Peek_Pixel(EwShaders::PS_POST_COPY) == nullptr)
		return Fail(REASON_NO_SHADER);

	IDirect3DDevice9 *device = DX8Wrapper::_Get_D3D_Device9();
	if (device == nullptr)
		return Fail(REASON_NO_TARGET);

	IDirect3DSurface9 *target = nullptr;
	IDirect3DSurface9 *depth = nullptr;
	device->GetRenderTarget(0, &target);
	device->GetDepthStencilSurface(&depth);
	if (target == nullptr || depth == nullptr)
	{
		Release_Com(target);
		Release_Com(depth);
		return Fail(REASON_NO_TARGET);
	}

	D3DSURFACE_DESC desc;
	target->GetDesc(&desc);
	if (SceneTexture == nullptr || !Same_Target(desc, TargetDesc))
	{
		HRESULT hr = CreateFailed ? E_FAIL : Create_Targets(device, desc);
		if (FAILED(hr))
		{
			CreateFailed = true;
			target->Release();
			depth->Release();
			return Fail(REASON_CREATE_FAILED, hr);
		}
	}

	// Setting a render target also sets the viewport to the whole target; the camera sets the
	// view's viewport again when the scene draws.
	HRESULT hr = device->SetRenderTarget(0, MsaaSurface ? MsaaSurface : SceneSurface);
	if (FAILED(hr))
	{
		device->SetRenderTarget(0, target);
		target->Release();
		depth->Release();
		return Fail(REASON_SET_FAILED, hr);
	}
	BackBuffer = target;
	DepthBuffer = depth;

	const DWORD alpha = (DWORD)(clear_alpha * 255.0f + 0.5f) & 0xff;
	device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(alpha, 0, 0, 0), 1.0f, 0);
	return true;
}

void EwPostProcess::End_Scene(int x, int y, int width, int height)
{
	if (BackBuffer == nullptr)
		return;

	IDirect3DDevice9 *device = DX8Wrapper::_Get_D3D_Device9();
	if (MsaaSurface)
		device->StretchRect(MsaaSurface, nullptr, SceneSurface, nullptr, D3DTEXF_NONE);
	device->SetRenderTarget(0, BackBuffer);
	device->SetDepthStencilSurface(DepthBuffer);

	// The view rectangle, clipped to the back buffer.
	const int tw = (int)TargetDesc.Width;
	const int th = (int)TargetDesc.Height;
	if (x < 0) { width += x; x = 0; }
	if (y < 0) { height += y; y = 0; }
	if (x + width > tw) width = tw - x;
	if (y + height > th) height = th - y;

	if (SavedStates == nullptr)
	{
		if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &SavedStates)))
			SavedStates = nullptr;
	}
	else
	{
		SavedStates->Capture();
	}
	// Without a state block the states cannot be put back, so the back buffer stays as the frame
	// clear left it.
	if (width <= 0 || height <= 0 || SavedStates == nullptr)
	{
		Release_Saved_Targets();
		return;
	}

	device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
		| D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	device->SetFVF(POST_FVF);
	device->SetVertexShader(EwShaders::Peek_Vertex(EwShaders::VS_POST));

	// Which effects of the final pass run. Bloom also needs its own shaders and textures, colour
	// grading its LUT texture; without any effect the scene is only copied.
	const EwGraphicsOptions &o = EwGraphics();
	bool bloom = Effect_On(BLOOM, o.Bloom, false, REASON_EFFECT_OFF, EwShaders::PS_POST_FINAL);
	if (bloom && (EwShaders::Peek_Pixel(EwShaders::PS_POST_BRIGHT) == nullptr || EwShaders::Peek_Pixel(EwShaders::PS_POST_DOWN) == nullptr
		|| EwShaders::Peek_Pixel(EwShaders::PS_POST_BLUR) == nullptr))
	{
		Set_Reason(BLOOM, REASON_NO_SHADER);
		bloom = false;
	}
	if (bloom)
		bloom = Bloom_Ready(device);
	const bool tone = Effect_On(TONE_MAPPING, o.ToneMapping, o.DebugToneMappingOff, REASON_TONE_DEBUG_OFF,
		EwShaders::PS_POST_FINAL);
	bool grade = Effect_On(COLOR_GRADING, o.ColorGrading, o.DebugColorGradingOff, REASON_GRADE_DEBUG_OFF,
		EwShaders::PS_POST_FINAL);
	if (grade)
		grade = Lut_Ready(device);
	const bool final_effects = bloom || tone || grade;
	bool fxaa = Effect_On(FXAA, o.FXAA, o.DebugFxaaOff, REASON_FXAA_DEBUG_OFF, EwShaders::PS_POST_FXAA);
	if (fxaa && final_effects)
		fxaa = Final_Ready(device);

	if (bloom)
		Draw_Bloom(device);

	const float u0 = (float)x / tw;
	const float v0 = (float)y / th;
	const float u1 = (float)(x + width) / tw;
	const float v1 = (float)(y + height) / th;

	// The final pass, onto the back buffer over the view rectangle, or into the same rectangle of
	// FinalTexture when FXAA follows. The scene is read with point filtering, so each pixel gets
	// exactly its own texel. FXAA alone skips it and reads the scene texture.
	IDirect3DTexture9 *fxaa_source = SceneTexture;
	if (fxaa && final_effects)
	{
		// FinalTexture has no multisampling, so no (possibly multisampled) depth buffer may be bound.
		device->SetRenderTarget(0, FinalSurface);
		device->SetDepthStencilSurface(nullptr);
		fxaa_source = FinalTexture;
	}
	Set_Texture(device, 0, SceneTexture, false);
	if (final_effects)
	{
		if (bloom)
		{
			for (int i = 0; i < BLOOM_LEVELS; ++i)
				Set_Texture(device, 1 + i, Bloom[i].Texture[0], true);
		}
		if (grade)
			Set_Texture(device, 4, LutTexture, true);
		const float final_flags[4] = { bloom ? 1.0f : 0.0f, tone ? 1.0f : 0.0f, grade ? 1.0f : 0.0f, 0.0f };
		const float tone_params[4] = { TONE_EXPOSURE, 1.0f / Filmic_Curve(TONE_EXPOSURE), 0.0f, 0.0f };
		device->SetPixelShaderConstantF(EW_POST_PS_FINAL, final_flags, 1);
		device->SetPixelShaderConstantF(EW_POST_PS_TONE, tone_params, 1);
		device->SetPixelShader(EwShaders::Peek_Pixel(EwShaders::PS_POST_FINAL));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);
	}
	else if (!fxaa)
	{
		device->SetPixelShader(EwShaders::Peek_Pixel(EwShaders::PS_POST_COPY));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);
	}
	++Stats[SCENE].Passes;
	if (bloom)
		++Stats[BLOOM].Passes;
	if (tone)
		++Stats[TONE_MAPPING].Passes;
	if (grade)
		++Stats[COLOR_GRADING].Passes;

	// FXAA, onto the back buffer. Its reads stay half a texel inside the view rectangle, so the
	// edge of the view never blends with what lies outside it.
	if (fxaa)
	{
		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
		Set_Texture(device, 0, fxaa_source, true);
		Set_Texel(device, 1.0f / tw, 1.0f / th, 0.0f, 0.0f);
		const float clamp_uv[4] = { u0 + 0.5f / tw, v0 + 0.5f / th, u1 - 0.5f / tw, v1 - 0.5f / th };
		device->SetPixelShaderConstantF(EW_POST_PS_CLAMP, clamp_uv, 1);
		device->SetPixelShader(EwShaders::Peek_Pixel(EwShaders::PS_POST_FXAA));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);
		++Stats[FXAA].Passes;
	}

	// Puts back every state, including the textures and the stream source that DrawPrimitiveUP
	// cleared.
	SavedStates->Apply();
	Release_Saved_Targets();
}

void EwPostProcess::Release()
{
	if (BackBuffer != nullptr)
	{
		IDirect3DDevice9 *device = DX8Wrapper::_Get_D3D_Device9();
		if (device)
		{
			device->SetRenderTarget(0, BackBuffer);
			device->SetDepthStencilSurface(DepthBuffer);
		}
		Release_Saved_Targets();
	}
	Release_Targets();
	Release_Com(SavedStates);
	Release_Com(LutTexture);
	LutUploaded = false;
	LutFailed = false;
	CreateFailed = false;
}

void EwPostProcess::New_Frame()
{
	for (int i = 0; i < EFFECT_COUNT; ++i)
	{
		EffectStats &s = Stats[i];
		s.LastPasses = s.Passes;
		s.LastReason = s.Reason ? s.Reason : ReasonText[REASON_NOT_DRAWN];
		s.Passes = 0;
		s.Reason = ReasonText[REASON_NOT_DRAWN];
	}
}

unsigned EwPostProcess::Last_Frame_Passes(Effect effect)
{
	return Stats[effect].LastPasses;
}

const char *EwPostProcess::Last_Reason(Effect effect)
{
	return Stats[effect].LastReason ? Stats[effect].LastReason : ReasonText[REASON_NOT_DRAWN];
}

const char *EwPostProcess::Color_Lut_File()
{
	return LutChosen && LutFile[0] != '\0' ? LutFile : nullptr;
}
