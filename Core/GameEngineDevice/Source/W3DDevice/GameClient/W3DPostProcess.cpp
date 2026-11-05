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

#include "W3DDevice/GameClient/W3DPostProcess.h"
#include "Common/GlobalData.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "Utility/stringex.h"
#include "WW3D2/dx9wrapper.h"
#include "WW3D2/camera.h"
#include "WW3D2/shaderlibrary.h"
#include "WW3D2/graphicsoptions.h"
#include "WW3D2/gputimer.h"
#include "WW3D2/meshshader.h"
#include "WW3D2/shadowmap.h"
#include "WWDebug/wwdebug.h"
#include "shader_constants.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <mmsystem.h>

// The render target is set on the device directly, not with DX9Wrapper::Set_Render_Target: the
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
	// With LinearLighting the bloom is added to linear light instead of screen blended, which
	// lights the area around a glow more; this strength gives about the same halo.
	const float BLOOM_LINEAR_STRENGTH = 0.45f;
	const float BLOOM_WEIGHTS[3] = { 0.45f, 0.35f, 0.30f };	// the 1/4, 1/8 and 1/16 size levels

	// Tone mapping strength, until it moves to INI data. Near 0 the curve changes nothing; larger
	// values give deeper shadows, brighter mid-tones and softer highlights.
	const float TONE_EXPOSURE = 0.6f;

	// With Hdr: the 16-bit float scene and bloom format, and the tone curve for it, which shows
	// scene (gamma) value TONE_HDR_WHITE as white so the light above 1 rolls off instead of clipping.
	const D3DFORMAT FORMAT_HDR = D3DFMT_A16B16G16R16F;
	const float TONE_HDR_EXPOSURE = 0.8f;
	const float TONE_HDR_WHITE = 2.0f;

	// Colour grading: a 256x16 TGA strip (see post.hlsl). The map's own LUT wins over the default.
	const char *const LUT_DEFAULT_FILE = "Art\\Textures\\ColorGrading.tga";
	const char *const LUT_MAP_FILE = "ColorGrading.tga";	// in the map's folder, beside map.str
	const int LUT_WIDTH = 256;
	const int LUT_HEIGHT = 16;

	// Ambient occlusion, until it moves to INI data. Distances are world units (a terrain cell is 10).
	const float AO_RADIUS = 25.0f;			// how far around a pixel occluders count
	const float AO_INTENSITY = 1.6f;
	const float AO_BIAS = 0.15f;			// rises flatter than this (cosine to the normal) do not occlude
	const float AO_MAX_RADIUS_UV = 0.06f;	// largest radius on screen, as a part of the view width
	const float AO_MAX_DEPTH = 4000.0f;		// fades out towards this depth
	const float AO_DEPTH_SHARPNESS = 12.0f;	// the blur stops at depth steps of about 1/12 of the depth
	const float AO_POWER = 1.3f;			// contrast of the final occlusion
	const D3DFORMAT FORMAT_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');

	// Heat haze, until it moves to INI data.
	const unsigned HEAT_MAX_SOURCES = 512;	// a frame's sources past this are dropped
	const float HEAT_BLOB_STRENGTH = 0.35f;	// mask value one source adds at its centre; a fire's many particles add up
	const float HEAT_OFFSET = 0.004f;		// largest uv offset at HeatHaze 1, as a part of the view height
	const float HEAT_WAVES = 30.0f;			// ripples over the view height

	// TAA, until it moves to INI data.
	const float TAA_FRAME_WEIGHT = 0.1f;	// how much this frame counts against the history
	// A camera cut: the camera moved further than this in a frame (world units), or turned more
	// than this (cosine of the angle). The history then starts again.
	const float TAA_CUT_DISTANCE = 150.0f;
	const float TAA_CUT_COS = 0.985f;

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
		REASON_AO_DEBUG_OFF,
		REASON_AO_NO_INTZ,
		REASON_AO_DEPTH_FAILED,
		REASON_AO_DEPTH_PASS_FAILED,
		REASON_AO_NO_G16R16F,
		REASON_AO_CREATE_FAILED,
		REASON_AO_NO_CAMERA,
		REASON_HDR_DEBUG_OFF,
		REASON_HDR_NO_FORMAT,
		REASON_HDR_CREATE_FAILED,
		REASON_SOFT_DEBUG_OFF,
		REASON_SOFT_MSAA,
		REASON_SOFT_NO_R32F,
		REASON_SOFT_CREATE_FAILED,
		REASON_HEAT_NO_SOURCES,
		REASON_HEAT_CREATE_FAILED,
		REASON_FXAA_TAA,
		REASON_TAA_CREATE_FAILED,
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
		"switched off with Ctrl+Shift+F1",
		"the card cannot draw into a readable depth texture (INTZ)",
		"the scene depth texture could not be created or set (HRESULT)",
		"the depth pass for AntiAliasing could not begin (device lost?)",
		"the card cannot draw into G16R16F textures",
		"the AO textures could not be created (HRESULT)",
		"no 3D camera was given",
		"switched off with Ctrl+Shift+H",
		"the card cannot draw, blend and filter 16-bit float textures",
		"the 16-bit float scene texture could not be created, perhaps with this anti-aliasing (HRESULT)",
		"switched off with Ctrl+Shift+P",
		"needs AntiAliasing = 0 in Options.ini (a multisampled depth buffer cannot be read)",
		"the card cannot draw into R32F textures",
		"the soft particle depth texture could not be created (HRESULT)",
		"no fire, explosion or exhaust particle in view",
		"the heat haze mask texture could not be created (HRESULT)",
		"skipped, TAA is on and does the anti-aliasing",
		"the TAA history textures could not be created (HRESULT)",
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
	EffectStats Stats[W3DPostProcess::EFFECT_COUNT];

	IDirect3DTexture9 *SceneTexture = nullptr;		// what the post passes read
	IDirect3DSurface9 *SceneSurface = nullptr;		// level 0 of SceneTexture
	IDirect3DSurface9 *MsaaSurface = nullptr;		// with multisampling the scene draws here first
	IDirect3DStateBlock9 *SavedStates = nullptr;	// all device states, around the post passes
	D3DSURFACE_DESC TargetDesc;						// the back buffer the textures were made for
	bool CreateFailed = false;						// do not try again until Release()
	bool SceneHdr = false;							// SceneTexture is FORMAT_HDR
	Reason HdrFailed = REASON_COUNT;				// why FORMAT_HDR cannot be used; not tried again until Release()

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

	// Ambient occlusion. The scene's own depth buffer, a depth texture the shaders can read; made
	// for the scene texture and used only while AO is on. With multisampling the scene keeps the
	// back buffer's depth and Draw_Depth_Pass() fills DepthTexture instead.
	IDirect3DTexture9 *DepthTexture = nullptr;
	IDirect3DSurface9 *DepthSurface = nullptr;
	bool DepthFailed = false;				// do not try again until Release()
	Reason DepthReason = REASON_NOT_DRAWN;	// why the scene did not draw with DepthTexture
	bool SceneDepthBound = false;			// this scene draws with DepthTexture
	const CameraClass *DepthPassCamera = nullptr;	// Draw_Depth_Pass() filled DepthTexture from this camera
	bool AoDepthPass = false;				// this frame's AO read the depth pass
	bool LastAoDepthPass = false;
	// The occlusion is drawn into [0], blurred across into [1] and down back into [0].
	IDirect3DTexture9 *AoTexture[2] = { nullptr, nullptr };
	IDirect3DSurface9 *AoSurface[2] = { nullptr, nullptr };
	int AoWidth = 0;
	int AoHeight = 0;
	int AoDivisor = 0;
	bool AoFailed = false;

	// Soft particles: the scene's view depth, copied from DepthTexture before the particles draw,
	// the size of the scene texture. Bound on PARTICLE_DEPTH_SAMPLER until End_Particles().
	IDirect3DTexture9 *ParticleDepthTexture = nullptr;
	IDirect3DSurface9 *ParticleDepthSurface = nullptr;
	Reason ParticleDepthFailed = REASON_COUNT;	// why it cannot be made; not tried again until Release()
	bool ParticleDepthBound = false;
	const CameraClass *SceneCamera = nullptr;	// the 3D camera between Begin_Scene and End_Scene

	// Heat haze: this frame's sources in world space, and the 1/4 size mask End_Scene() draws them into.
	struct HeatSource
	{
		float X, Y, Z;
		float Radius;
		float Strength;
	};
	HeatSource HeatSources[HEAT_MAX_SOURCES];
	unsigned HeatCount = 0;
	unsigned LastHeatCount = 0;
	IDirect3DTexture9 *HeatTexture = nullptr;
	IDirect3DSurface9 *HeatSurface = nullptr;
	int HeatWidth = 0;
	int HeatHeight = 0;
	bool HeatFailed = false;

	// TAA: two history textures the size of the scene texture, which take turns: the TAA pass reads
	// the last frame's result from one and writes this frame's into the other. TaaDepthTexture
	// holds the last frame's view depth (R32F).
	IDirect3DTexture9 *History[2] = { nullptr, nullptr };
	IDirect3DSurface9 *HistorySurface[2] = { nullptr, nullptr };
	int HistoryIndex = 0;						// the one this frame writes
	IDirect3DTexture9 *TaaDepthTexture = nullptr;
	IDirect3DSurface9 *TaaDepthSurface = nullptr;
	Reason TaaFailed = REASON_COUNT;			// why the textures cannot be made; not tried again until Release()
	bool HistoryValid = false;					// History[1 - HistoryIndex] holds a frame of this view
	unsigned FrameCount = 0;					// New_Frame() counts
	unsigned HistoryFrame = 0;					// FrameCount when the history was written
	float TaaJitter[2] = { 0.0f, 0.0f };		// this frame's offset in pixels
	CameraClass *TaaCamera = nullptr;			// jittered between Begin_Scene and End_Scene
	// The last frame's camera: its inverse transform, position, forward direction and uv -> view
	// plane mapping (as AoRay, without the jitter).
	Matrix3D PrevInverse(true);
	Vector3 PrevPosition(0.0f, 0.0f, 0.0f);
	Vector3 PrevForward(0.0f, 0.0f, 1.0f);
	float PrevRay[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
	unsigned TaaResets = 0;

	// Set between Begin_Scene and End_Scene.
	IDirect3DSurface9 *BackBuffer = nullptr;
	IDirect3DSurface9 *DepthBuffer = nullptr;

	void Log_Once(Reason reason, unsigned value)
	{
		if (!Logged[reason] && reason != REASON_OFF && reason != REASON_DEBUG_OFF && reason != REASON_EFFECT_OFF
			&& reason != REASON_TONE_DEBUG_OFF && reason != REASON_GRADE_DEBUG_OFF && reason != REASON_FXAA_DEBUG_OFF
			&& reason != REASON_AO_DEBUG_OFF && reason != REASON_HDR_DEBUG_OFF
			&& reason != REASON_SOFT_DEBUG_OFF && reason != REASON_SOFT_MSAA && reason != REASON_HEAT_NO_SOURCES
			&& reason != REASON_FXAA_TAA)
		{
			Logged[reason] = true;
			WWDEBUG_SAY(("W3DPostProcess: %s, value 0x%x (logged once)", ReasonText[reason], value));
		}
	}

	void Set_Reason(W3DPostProcess::Effect effect, Reason reason)
	{
		Stats[effect].Reason = ReasonText[reason];
	}

	// The scene texture is not used, so no effect runs.
	bool Fail(Reason reason, unsigned value = 0)
	{
		for (int i = 0; i < W3DPostProcess::EFFECT_COUNT; ++i)
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

	void Release_Ao()
	{
		for (int i = 0; i < 2; ++i)
		{
			Release_Com(AoSurface[i]);
			Release_Com(AoTexture[i]);
		}
		AoWidth = AoHeight = AoDivisor = 0;
		AoFailed = false;
	}

	void Release_Targets()
	{
		DepthPassCamera = nullptr;
		Release_Com(DepthSurface);
		Release_Com(DepthTexture);
		Release_Com(ParticleDepthSurface);
		Release_Com(ParticleDepthTexture);
		ParticleDepthFailed = REASON_COUNT;
		Release_Ao();
		Release_Com(MsaaSurface);
		Release_Com(SceneSurface);
		Release_Com(SceneTexture);
		Release_Com(FinalSurface);
		Release_Com(FinalTexture);
		FinalFailed = false;
		Release_Com(HeatSurface);
		Release_Com(HeatTexture);
		HeatFailed = false;
		for (int i = 0; i < 2; ++i)
		{
			Release_Com(HistorySurface[i]);
			Release_Com(History[i]);
		}
		Release_Com(TaaDepthSurface);
		Release_Com(TaaDepthTexture);
		TaaFailed = REASON_COUNT;
		HistoryValid = false;
		Release_Bloom();
	}

	void Release_Saved_Targets()
	{
		Release_Com(BackBuffer);
		Release_Com(DepthBuffer);
	}

	// Makes the scene texture (and multisampled surface) match the back buffer, in its format or FORMAT_HDR.
	HRESULT Create_Targets(IDirect3DDevice9 *device, const D3DSURFACE_DESC &desc, bool hdr)
	{
		Release_Targets();
		const D3DFORMAT format = hdr ? FORMAT_HDR : desc.Format;
		HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, format,
			D3DPOOL_DEFAULT, &SceneTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = SceneTexture->GetSurfaceLevel(0, &SceneSurface);
		if (SUCCEEDED(hr) && desc.MultiSampleType != D3DMULTISAMPLE_NONE)
		{
			hr = device->CreateRenderTarget(desc.Width, desc.Height, format, desc.MultiSampleType,
				desc.MultiSampleQuality, FALSE, &MsaaSurface, nullptr);
		}
		if (FAILED(hr))
			Release_Targets();
		else
		{
			TargetDesc = desc;
			SceneHdr = hdr;
		}
		return hr;
	}

	// Creates the scene's depth texture for the current scene texture, once. If it cannot be used,
	// DepthReason says why.
	bool Depth_Ready(IDirect3DDevice9 *device)
	{
		if (DepthTexture != nullptr)
			return true;
		if (DepthFailed)
			return false;	// DepthReason still says why

		HRESULT hr = E_FAIL;
		if (!ShaderLibrary::Format_Supported(device, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FORMAT_INTZ))
			DepthReason = REASON_AO_NO_INTZ;
		else
		{
			hr = device->CreateTexture(TargetDesc.Width, TargetDesc.Height, 1, D3DUSAGE_DEPTHSTENCIL, FORMAT_INTZ,
				D3DPOOL_DEFAULT, &DepthTexture, nullptr);
			if (SUCCEEDED(hr))
				hr = DepthTexture->GetSurfaceLevel(0, &DepthSurface);
			if (FAILED(hr))
				DepthReason = REASON_AO_DEPTH_FAILED;
		}
		if (FAILED(hr))
		{
			Release_Com(DepthSurface);
			Release_Com(DepthTexture);
			DepthFailed = true;
			Log_Once(DepthReason, hr);
			return false;
		}
		WWDEBUG_SAY(("W3DPostProcess: %ux%u INTZ scene depth texture for ambient occlusion", TargetDesc.Width, TargetDesc.Height));
		return true;
	}

	// Creates the two AO textures (1/divisor of the scene texture size), once.
	bool Ao_Ready(IDirect3DDevice9 *device, int divisor)
	{
		if (AoTexture[0] != nullptr && AoDivisor == divisor)
			return true;
		if (AoFailed)
		{
			Set_Reason(W3DPostProcess::AMBIENT_OCCLUSION, REASON_AO_CREATE_FAILED);
			return false;
		}
		Release_Ao();
		if (!ShaderLibrary::Format_Supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_G16R16F))
		{
			AoFailed = true;
			Set_Reason(W3DPostProcess::AMBIENT_OCCLUSION, REASON_AO_NO_G16R16F);
			Log_Once(REASON_AO_NO_G16R16F, 0);
			return false;
		}
		const int w = ((int)TargetDesc.Width + divisor - 1) / divisor;
		const int h = ((int)TargetDesc.Height + divisor - 1) / divisor;
		HRESULT hr = S_OK;
		for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
		{
			hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &AoTexture[i], nullptr);
			if (SUCCEEDED(hr))
				hr = AoTexture[i]->GetSurfaceLevel(0, &AoSurface[i]);
		}
		if (FAILED(hr))
		{
			Release_Ao();
			AoFailed = true;
			Set_Reason(W3DPostProcess::AMBIENT_OCCLUSION, REASON_AO_CREATE_FAILED);
			Log_Once(REASON_AO_CREATE_FAILED, hr);
			return false;
		}
		AoWidth = w;
		AoHeight = h;
		AoDivisor = divisor;
		WWDEBUG_SAY(("W3DPostProcess: %dx%d ambient occlusion textures", w, h));
		return true;
	}

	// Creates the bloom textures for the current scene texture size, once.
	bool Bloom_Ready(IDirect3DDevice9 *device)
	{
		if (Bloom[0].Texture[0] != nullptr)
			return true;
		if (BloomFailed)
		{
			Set_Reason(W3DPostProcess::BLOOM, REASON_BLOOM_CREATE_FAILED);
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
				hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, SceneHdr ? FORMAT_HDR : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
					&Bloom[i].Texture[j], nullptr);
				if (SUCCEEDED(hr))
					hr = Bloom[i].Texture[j]->GetSurfaceLevel(0, &Bloom[i].Surface[j]);
			}
		}
		if (FAILED(hr))
		{
			Release_Bloom();
			BloomFailed = true;
			Set_Reason(W3DPostProcess::BLOOM, REASON_BLOOM_CREATE_FAILED);
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
			return false;
		}
		return true;
	}

	// Creates the heat haze mask, 1/4 of the scene texture size, once.
	bool Heat_Ready(IDirect3DDevice9 *device)
	{
		if (HeatTexture != nullptr)
			return true;
		const int w = ((int)TargetDesc.Width + 3) / 4;
		const int h = ((int)TargetDesc.Height + 3) / 4;
		HRESULT hr = HeatFailed ? E_FAIL : device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
			D3DPOOL_DEFAULT, &HeatTexture, nullptr);
		if (SUCCEEDED(hr))
			hr = HeatTexture->GetSurfaceLevel(0, &HeatSurface);
		if (FAILED(hr))
		{
			Release_Com(HeatSurface);
			Release_Com(HeatTexture);
			if (!HeatFailed)
				Log_Once(REASON_HEAT_CREATE_FAILED, hr);
			HeatFailed = true;
			Set_Reason(W3DPostProcess::HEAT_HAZE, REASON_HEAT_CREATE_FAILED);
			return false;
		}
		HeatWidth = w;
		HeatHeight = h;
		return true;
	}

	// Creates the TAA history and last-depth textures for the current scene texture, once, and the
	// final pass's texture, which TAA reads when the final pass has effects.
	bool Taa_Ready(IDirect3DDevice9 *device)
	{
		if (History[0] == nullptr && TaaFailed == REASON_COUNT)
		{
			HRESULT hr = E_FAIL;
			if (!ShaderLibrary::Format_Supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R32F))
				TaaFailed = REASON_SOFT_NO_R32F;
			else
			{
				// 16-bit float keeps the small steps of the blend, which 8 bits would round away.
				const D3DFORMAT format = ShaderLibrary::Format_Supported(device, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER,
					D3DRTYPE_TEXTURE, FORMAT_HDR) ? FORMAT_HDR : TargetDesc.Format;
				hr = S_OK;
				for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
				{
					hr = device->CreateTexture(TargetDesc.Width, TargetDesc.Height, 1, D3DUSAGE_RENDERTARGET, format,
						D3DPOOL_DEFAULT, &History[i], nullptr);
					if (SUCCEEDED(hr))
						hr = History[i]->GetSurfaceLevel(0, &HistorySurface[i]);
				}
				if (SUCCEEDED(hr))
					hr = device->CreateTexture(TargetDesc.Width, TargetDesc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
						D3DPOOL_DEFAULT, &TaaDepthTexture, nullptr);
				if (SUCCEEDED(hr))
					hr = TaaDepthTexture->GetSurfaceLevel(0, &TaaDepthSurface);
				if (FAILED(hr))
					TaaFailed = REASON_TAA_CREATE_FAILED;
			}
			if (TaaFailed != REASON_COUNT)
			{
				for (int i = 0; i < 2; ++i)
				{
					Release_Com(HistorySurface[i]);
					Release_Com(History[i]);
				}
				Release_Com(TaaDepthSurface);
				Release_Com(TaaDepthTexture);
				Log_Once(TaaFailed, hr);
			}
			HistoryValid = false;
		}
		if (TaaFailed != REASON_COUNT)
		{
			Set_Reason(W3DPostProcess::TAA, TaaFailed);
			return false;
		}
		if (!Final_Ready(device))
		{
			Set_Reason(W3DPostProcess::TAA, REASON_FXAA_CREATE_FAILED);
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
			WWDEBUG_SAY(("W3DPostProcess: colour LUT %s could not be read", path));
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
			WWDEBUG_SAY(("W3DPostProcess: colour LUT %s must be a %dx%d true-colour TGA (24 or 32 bits), it is %dx%d type %d, %d bits",
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
			WWDEBUG_SAY(("W3DPostProcess: colour LUT %s is cut short", path));
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
			Set_Reason(W3DPostProcess::COLOR_GRADING, REASON_LUT_CREATE_FAILED);
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
			Set_Reason(W3DPostProcess::COLOR_GRADING, REASON_LUT_CREATE_FAILED);
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

	// The heat blobs, two triangles each. PostVS hands z (the strength) to the blob shader.
	PostVertex HeatVertices[HEAT_MAX_SOURCES * 6];

	// Projects this frame's heat sources with camera into HeatVertices, in the camera's clip
	// space (the view rectangle). Returns how many are in view.
	unsigned Build_Heat_Blobs(const CameraClass *camera)
	{
		unsigned blobs = 0;
		for (unsigned i = 0; i < HeatCount; ++i)
		{
			const HeatSource &s = HeatSources[i];
			Vector3 view;
			Vector3 centre;
			Vector3 edge;
			camera->Transform_To_View_Space(view, Vector3(s.X, s.Y, s.Z));
			if (camera->Project_Camera_Space_Point(centre, view) == CameraClass::OUTSIDE_NEAR_CLIP)
				continue;
			camera->Project_Camera_Space_Point(edge, view + Vector3(s.Radius, s.Radius, 0.0f));
			const float rx = fabsf(edge.X - centre.X);
			const float ry = fabsf(edge.Y - centre.Y);
			if (centre.Z > 1.0f || centre.X - rx > 1.0f || centre.X + rx < -1.0f || centre.Y - ry > 1.0f || centre.Y + ry < -1.0f)
				continue;
			const float z = s.Strength * HEAT_BLOB_STRENGTH;
			const PostVertex a = { centre.X - rx, centre.Y + ry, z, -1.0f, -1.0f };
			const PostVertex b = { centre.X + rx, centre.Y + ry, z,  1.0f, -1.0f };
			const PostVertex c = { centre.X - rx, centre.Y - ry, z, -1.0f,  1.0f };
			const PostVertex d = { centre.X + rx, centre.Y - ry, z,  1.0f,  1.0f };
			PostVertex *v = &HeatVertices[blobs * 6];
			v[0] = a; v[1] = b; v[2] = c;
			v[3] = c; v[4] = b; v[5] = d;
			++blobs;
		}
		return blobs;
	}

	// Draws the blobs into the view rectangle (u0..v1) of the heat mask, added up. Ends with the
	// back buffer and its depth buffer bound again.
	void Draw_Heat_Mask(IDirect3DDevice9 *device, unsigned blobs, float u0, float v0, float u1, float v1)
	{
		// The mask has no multisampling, so no (possibly multisampled) depth buffer may be bound.
		device->SetDepthStencilSurface(nullptr);
		device->SetRenderTarget(0, HeatSurface);
		device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
		const int x0 = (int)(u0 * HeatWidth + 0.5f);
		const int y0 = (int)(v0 * HeatHeight + 0.5f);
		const int x1 = (int)(u1 * HeatWidth + 0.5f);
		const int y1 = (int)(v1 * HeatHeight + 0.5f);
		if (x1 > x0 && y1 > y0)
		{
			D3DVIEWPORT9 viewport = { (DWORD)x0, (DWORD)y0, (DWORD)(x1 - x0), (DWORD)(y1 - y0), 0.0f, 1.0f };
			device->SetViewport(&viewport);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
			device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
			device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
			device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
			device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_HEAT_BLOB));
			device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, blobs * 2, HeatVertices, sizeof(PostVertex));
			DX9Wrapper::Increment_DX9_CallCount();
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		}
		++Stats[W3DPostProcess::HEAT_HAZE].Passes;
		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
	}

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
		DX9Wrapper::Increment_DX9_CallCount();
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
		device->SetPixelShaderConstantF(POST_PS_TEXEL, texel, 1);
	}

	// One pass into a whole bloom texture, reading source with linear filtering.
	void Bloom_Pass(IDirect3DDevice9 *device, ShaderLibrary::PixelShaderId shader, IDirect3DTexture9 *source,
		IDirect3DSurface9 *dest, int width, int height)
	{
		device->SetRenderTarget(0, dest);
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(shader));
		Set_Texture(device, 0, source, true);
		Draw_Quad(device, 0, 0, width, height, 0.0f, 0.0f, 1.0f, 1.0f);
		++Stats[W3DPostProcess::BLOOM].Passes;
	}

	// Builds the three blurred bloom levels from the scene texture. Ends with the back buffer and
	// its depth buffer bound again.
	void Draw_Bloom(IDirect3DDevice9 *device)
	{
		const float strength = Get_Graphics_Options().Linear() ? BLOOM_LINEAR_STRENGTH : BLOOM_STRENGTH;
		const float params[4] = { BLOOM_THRESHOLD, 1.0f / (1.0f - BLOOM_THRESHOLD), strength, 0.0f };
		const float weights[4] = { BLOOM_WEIGHTS[0], BLOOM_WEIGHTS[1], BLOOM_WEIGHTS[2], 0.0f };
		device->SetPixelShaderConstantF(POST_PS_BLOOM, params, 1);
		device->SetPixelShaderConstantF(POST_PS_BLOOM_WEIGHTS, weights, 1);

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
			Bloom_Pass(device, i == 0 ? ShaderLibrary::PS_POST_BRIGHT : ShaderLibrary::PS_POST_DOWN, source,
				level.Surface[0], level.Width, level.Height);

			Set_Texel(device, 0.0f, 0.0f, 1.0f / level.Width, 0.0f);
			Bloom_Pass(device, ShaderLibrary::PS_POST_BLUR, level.Texture[0], level.Surface[1], level.Width, level.Height);
			Set_Texel(device, 0.0f, 0.0f, 0.0f, 1.0f / level.Height);
			Bloom_Pass(device, ShaderLibrary::PS_POST_BLUR, level.Texture[1], level.Surface[0], level.Width, level.Height);

			source = level.Texture[0];
			source_w = level.Width;
			source_h = level.Height;
		}

		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
	}

	// One AO pass over a whole AO texture.
	void Ao_Pass(IDirect3DDevice9 *device, ShaderLibrary::PixelShaderId shader, IDirect3DSurface9 *dest)
	{
		device->SetRenderTarget(0, dest);
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(shader));
		Draw_Quad(device, 0, 0, AoWidth, AoHeight, 0.0f, 0.0f, 1.0f, 1.0f);
		++Stats[W3DPostProcess::AMBIENT_OCCLUSION].Passes;
	}

	// The POST_PS_DEPTH constant: stored depth -> view depth. The projection maps view depth d
	// to f/(f-n) * (1 - n/d), and the viewport puts that into zmin..zmax.
	void Depth_Params(const CameraClass *camera, float depth[4])
	{
		float znear = 1.0f;
		float zfar = 1000.0f;
		float zmin = 0.0f;
		float zmax = 1.0f;
		camera->Get_Clip_Planes(znear, zfar);
		camera->Get_Depth_Range(&zmin, &zmax);
		const float zrange = zmax > zmin ? zmax - zmin : 1.0f;
		depth[0] = znear * zfar;
		depth[1] = zfar + (zfar - znear) * zmin / zrange;
		depth[2] = (zfar - znear) / zrange;
		depth[3] = zfar < AO_MAX_DEPTH ? zfar : AO_MAX_DEPTH;
	}

	// The states of the full-screen post passes, with the post vertex shader.
	void Set_Post_States(IDirect3DDevice9 *device)
	{
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
		device->SetVertexShader(ShaderLibrary::Peek_Vertex(ShaderLibrary::VS_POST));
	}

	// Creates the soft particle depth texture for the current scene texture, once.
	bool Particle_Depth_Ready(IDirect3DDevice9 *device)
	{
		if (ParticleDepthTexture == nullptr && ParticleDepthFailed == REASON_COUNT)
		{
			HRESULT hr = E_FAIL;
			if (!ShaderLibrary::Format_Supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R32F))
				ParticleDepthFailed = REASON_SOFT_NO_R32F;
			else
			{
				hr = device->CreateTexture(TargetDesc.Width, TargetDesc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
					D3DPOOL_DEFAULT, &ParticleDepthTexture, nullptr);
				if (SUCCEEDED(hr))
					hr = ParticleDepthTexture->GetSurfaceLevel(0, &ParticleDepthSurface);
				if (FAILED(hr))
				{
					Release_Com(ParticleDepthSurface);
					Release_Com(ParticleDepthTexture);
					ParticleDepthFailed = REASON_SOFT_CREATE_FAILED;
				}
			}
			if (ParticleDepthFailed != REASON_COUNT)
				Log_Once(ParticleDepthFailed, hr);
		}
		return ParticleDepthFailed == REASON_COUNT;	// else ParticleDepthFailed says why
	}

	// Captures all device states into SavedStates, creating it the first time. False without it.
	bool Save_States(IDirect3DDevice9 *device)
	{
		if (SavedStates == nullptr)
		{
			if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &SavedStates)))
				SavedStates = nullptr;
		}
		else
		{
			SavedStates->Capture();
		}
		return SavedStates != nullptr;
	}

	// Copies the view depth of DepthTexture into ParticleDepthTexture, between draws of the scene.
	void Draw_Depth_Copy(IDirect3DDevice9 *device, const CameraClass *camera, IDirect3DSurface9 *target, IDirect3DSurface9 *depth)
	{
		// The state block does not hold the render target and depth buffer, so they are put back
		// first; it does hold the viewport.
		device->SetDepthStencilSurface(nullptr);
		device->SetRenderTarget(0, ParticleDepthSurface);
		Set_Post_States(device);
		float depth_params[4];
		Depth_Params(camera, depth_params);
		device->SetPixelShaderConstantF(POST_PS_DEPTH, depth_params, 1);
		Set_Texture(device, POST_DEPTH_SAMPLER, DepthTexture, false);
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_DEPTH_COPY));
		Draw_Quad(device, 0, 0, (int)TargetDesc.Width, (int)TargetDesc.Height, 0.0f, 0.0f, 1.0f, 1.0f);
		device->SetRenderTarget(0, target);
		device->SetDepthStencilSurface(depth);
		SavedStates->Apply();
	}

	// Draws the blurred occlusion into AoTexture[0] and sets the constants the final pass reads
	// (c6..c10). u0..v1 is the view rectangle in the scene texture. Ends with the back buffer and
	// its depth buffer bound again.
	void Draw_Ao(IDirect3DDevice9 *device, const CameraClass *camera, float u0, float v0, float u1, float v1)
	{
		Vector2 vmin;
		Vector2 vmax;
		camera->Get_View_Plane(vmin, vmax);
		const float du = u1 > u0 ? u1 - u0 : 1.0f;
		const float dv = v1 > v0 ? v1 - v0 : 1.0f;
		const float view_w = vmax.X > vmin.X ? vmax.X - vmin.X : 1.0f;
		const float view_h = vmax.Y > vmin.Y ? vmax.Y - vmin.Y : 1.0f;

		float depth[4];
		Depth_Params(camera, depth);
		// uv -> the view plane at depth 1 (v grows down, y up).
		const float ray_x = view_w / du;
		const float ray_y = -view_h / dv;
		const float ray[4] = { ray_x, ray_y, vmin.X - u0 * ray_x, vmax.Y - v0 * ray_y };
		const float params[4] = { AO_RADIUS, AO_INTENSITY, AO_BIAS, AO_MAX_RADIUS_UV * du };
		const float params2[4] = { du / view_w, dv / view_h, 1.0f / AoWidth, 1.0f / AoHeight };
		const float final_params[4] = { Get_Graphics_Options().PostAoOnly() ? 1.0f : 0.0f, AO_DEPTH_SHARPNESS, AO_POWER, 0.0f };
		device->SetPixelShaderConstantF(POST_PS_DEPTH, depth, 1);
		device->SetPixelShaderConstantF(POST_PS_AO_RAY, ray, 1);
		device->SetPixelShaderConstantF(POST_PS_AO, params, 1);
		device->SetPixelShaderConstantF(POST_PS_AO2, params2, 1);
		device->SetPixelShaderConstantF(POST_PS_AO_FINAL, final_params, 1);

		// The AO textures are smaller than the depth buffer, and the passes need none.
		device->SetDepthStencilSurface(nullptr);
		Set_Texture(device, POST_DEPTH_SAMPLER, DepthTexture, false);

		// Occlusion. The taps stay half a texel inside the view rectangle; outside it the depth is
		// the clear value, which the shader treats as too far to occlude.
		const float tw = (float)TargetDesc.Width;
		const float th = (float)TargetDesc.Height;
		const float view_clamp[4] = { u0 + 0.5f / tw, v0 + 0.5f / th, u1 - 0.5f / tw, v1 - 0.5f / th };
		device->SetPixelShaderConstantF(POST_PS_CLAMP, view_clamp, 1);
		Set_Texel(device, 1.0f / tw, 1.0f / th, 0.0f, 0.0f);
		Ao_Pass(device, ShaderLibrary::PS_POST_AO, AoSurface[0]);

		// Blur across, then down.
		const float whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		device->SetPixelShaderConstantF(POST_PS_CLAMP, whole, 1);
		Set_Texture(device, POST_AO_SAMPLER, AoTexture[0], false);
		Set_Texel(device, 0.0f, 0.0f, 1.0f / AoWidth, 0.0f);
		Ao_Pass(device, ShaderLibrary::PS_POST_AO_BLUR, AoSurface[1]);
		Set_Texture(device, POST_AO_SAMPLER, AoTexture[1], false);
		Set_Texel(device, 0.0f, 0.0f, 0.0f, 1.0f / AoHeight);
		Ao_Pass(device, ShaderLibrary::PS_POST_AO_BLUR, AoSurface[0]);

		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
	}

	// TAA: blends source (this frame's finished colours) with the last frame's result, reprojected
	// with camera's movement, into HistorySurface[HistoryIndex] over the view rectangle (x, y,
	// width, height; u0..v1 in uv), and keeps this frame's view depth for the next frame. Returns
	// the result for the FXAA pass. Ends with the back buffer and its depth buffer bound again.
	IDirect3DTexture9 *Draw_Taa(IDirect3DDevice9 *device, const CameraClass *camera, IDirect3DTexture9 *source,
		int x, int y, int width, int height, float u0, float v0, float u1, float v1)
	{
		const float tw = (float)TargetDesc.Width;
		const float th = (float)TargetDesc.Height;

		// uv -> the view plane at depth 1 (v grows down, y up), as in Draw_Ao().
		Vector2 vmin;
		Vector2 vmax;
		camera->Get_View_Plane(vmin, vmax);
		const float du = u1 > u0 ? u1 - u0 : 1.0f;
		const float dv = v1 > v0 ? v1 - v0 : 1.0f;
		const float rx = (vmax.X > vmin.X ? vmax.X - vmin.X : 1.0f) / du;
		const float ry = -(vmax.Y > vmin.Y ? vmax.Y - vmin.Y : 1.0f) / dv;
		const float ray[4] = { rx, ry, vmin.X - u0 * rx, vmax.Y - v0 * ry };
		// Both ends without the jitter: the history holds each pixel's centre, so with a still camera
		// it is read at this very pixel. Cancelling the jitter here would resample it a part of a
		// pixel off every frame and blur it.

		const Matrix3D &transform = camera->Get_Transform();
		const Vector3 position = transform.Get_Translation();
		const Vector3 forward = transform.Get_Z_Vector();
		const bool cut = (position - PrevPosition).Length() > TAA_CUT_DISTANCE
			|| Vector3::Dot_Product(forward, PrevForward) < TAA_CUT_COS;
		// Only a history of the frame just before: a frame without TAA leaves it out of date.
		const bool history = HistoryValid && HistoryFrame + 1 == FrameCount && !cut;
		if (!history)
			++TaaResets;

		// A view space point of this frame, (uv * d, d, 1) for view depth d, is
		// (d * (u * rx + ray[2]), d * (v * ry + ray[3]), -d) (the camera looks down -z); m takes it into the
		// last frame's view space, and the last frame's ray turns that into its uv.
		Matrix3D m;
		Matrix3D::Multiply(PrevInverse, transform, &m);
		float rows[3][4];
		for (int i = 0; i < 3; ++i)
		{
			rows[i][0] = m[i][0] * rx;
			rows[i][1] = m[i][1] * ry;
			rows[i][2] = m[i][0] * ray[2] + m[i][1] * ray[3] - m[i][2];
			rows[i][3] = m[i][3];
		}
		float reproject[3][4];
		for (int k = 0; k < 4; ++k)
		{
			reproject[0][k] = (rows[0][k] + PrevRay[2] * rows[2][k]) / PrevRay[0];
			reproject[1][k] = (rows[1][k] + PrevRay[3] * rows[2][k]) / PrevRay[1];
			reproject[2][k] = -rows[2][k];
		}

		float depth[4];
		Depth_Params(camera, depth);
		const float taa[4] = { history ? 1.0f : 0.0f, TAA_FRAME_WEIGHT, 0.0f, 0.0f };
		const float clamp_uv[4] = { u0 + 0.5f / tw, v0 + 0.5f / th, u1 - 0.5f / tw, v1 - 0.5f / th };
		device->SetPixelShaderConstantF(POST_PS_DEPTH, depth, 1);
		device->SetPixelShaderConstantF(POST_PS_TAA_REPROJECT, &reproject[0][0], 3);
		device->SetPixelShaderConstantF(POST_PS_TAA, taa, 1);
		device->SetPixelShaderConstantF(POST_PS_CLAMP, clamp_uv, 1);
		Set_Texel(device, 1.0f / tw, 1.0f / th, 0.0f, 0.0f);

		// The history textures have no multisampling, so no (possibly multisampled) depth buffer may be bound.
		device->SetDepthStencilSurface(nullptr);
		device->SetRenderTarget(0, HistorySurface[HistoryIndex]);
		Set_Texture(device, 0, source, false);
		Set_Texture(device, POST_HISTORY_SAMPLER, History[1 - HistoryIndex], true);
		Set_Texture(device, POST_DEPTH_SAMPLER, DepthTexture, false);
		Set_Texture(device, POST_PREV_DEPTH_SAMPLER, TaaDepthTexture, false);
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_TAA));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);

		// The depth copy draws into the texture the TAA pass read.
		device->SetTexture(POST_PREV_DEPTH_SAMPLER, nullptr);
		device->SetRenderTarget(0, TaaDepthSurface);
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_DEPTH_COPY));
		Draw_Quad(device, 0, 0, (int)TargetDesc.Width, (int)TargetDesc.Height, 0.0f, 0.0f, 1.0f, 1.0f);

		transform.Get_Inverse(PrevInverse);
		PrevPosition = position;
		PrevForward = forward;
		for (int k = 0; k < 4; ++k)
			PrevRay[k] = ray[k];
		HistoryValid = true;
		HistoryFrame = FrameCount;
		IDirect3DTexture9 *result = History[HistoryIndex];
		HistoryIndex = 1 - HistoryIndex;
		++Stats[W3DPostProcess::TAA].Passes;

		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
		return result;
	}

	// Whether a post effect runs this frame; sets the reason if not.
	bool Effect_On(W3DPostProcess::Effect effect, bool option, bool debug_off, Reason debug_reason,
		ShaderLibrary::PixelShaderId shader)
	{
		if (!option)
			Set_Reason(effect, REASON_EFFECT_OFF);
		else if (debug_off)
			Set_Reason(effect, debug_reason);
		else if (ShaderLibrary::Peek_Pixel(shader) == nullptr)
			Set_Reason(effect, REASON_NO_SHADER);
		else
			return true;
		return false;
	}
}

bool W3DPostProcess::Begin_Scene(float clear_alpha, CameraClass *camera)
{
	if (BackBuffer != nullptr)
		return false;	// already rendering into the scene texture

	const GraphicsOptions &o = Get_Graphics_Options();
	if (!o.PostProcess())
		return Fail(o.PostProcessWanted() ? REASON_DEBUG_OFF : REASON_OFF);

	if (ShaderLibrary::Peek_Vertex(ShaderLibrary::VS_POST) == nullptr || ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_COPY) == nullptr)
		return Fail(REASON_NO_SHADER);

	IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
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
	const bool hdr = o.PostHdr() && HdrFailed == REASON_COUNT;
	if (SceneTexture == nullptr || !Same_Target(desc, TargetDesc) || hdr != SceneHdr)
	{
		HRESULT hr = E_FAIL;
		if (hdr && !CreateFailed)
		{
			// Particles and water blend into the scene, and bloom and the water refraction filter it.
			if (!ShaderLibrary::Format_Supported(device, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING
				| D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, FORMAT_HDR))
				HdrFailed = REASON_HDR_NO_FORMAT;
			else if (FAILED(hr = Create_Targets(device, desc, true)))
			{
				HdrFailed = REASON_HDR_CREATE_FAILED;
				Log_Once(REASON_HDR_CREATE_FAILED, hr);
			}
		}
		if (FAILED(hr) && !CreateFailed)
			hr = Create_Targets(device, desc, false);
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
	if (!SceneHdr)
		Set_Reason(HDR, !o.Hdr ? REASON_EFFECT_OFF : o.DebugHdrOff ? REASON_HDR_DEBUG_OFF : HdrFailed);

	// SunIntensity lifts the sun of the lit shaders (shader_linear.h) only while they draw into
	// the float scene; an 8-bit target would clip it. End_Scene() puts it back to 1.
	{
		const float sun[4] = { SceneHdr ? o.SunIntensity : 1.0f, 0.0f, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(FRAME_PS_SUN, sun, 1);
	}

	// Ambient occlusion, soft particles, TAA and water SSR read the scene's depth, so the scene draws with its own depth
	// texture, cleared here as the frame clear cleared the back buffer's.
	DWORD clear = D3DCLEAR_TARGET;
	SceneDepthBound = false;
	// A multisampled depth buffer cannot be read; AO then reads the depth pass.
	if (TargetDesc.MultiSampleType == D3DMULTISAMPLE_NONE && (o.PostAo() || o.PostSoftParticles() || o.TAA || o.WaterSsr()) && Depth_Ready(device))
	{
		hr = device->SetDepthStencilSurface(DepthSurface);
		if (SUCCEEDED(hr))
		{
			SceneDepthBound = true;
			clear |= D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
		}
		else
		{
			device->SetDepthStencilSurface(DepthBuffer);
			DepthReason = REASON_AO_DEPTH_FAILED;
			Log_Once(REASON_AO_DEPTH_FAILED, hr);
		}
	}

	SceneCamera = camera;

	// TAA: the scene draws moved by this frame's jitter, until End_Scene().
	TaaCamera = nullptr;
	if (Effect_On(TAA, o.TAA, false, REASON_EFFECT_OFF, ShaderLibrary::PS_POST_TAA))
	{
		if (ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_DEPTH_COPY) == nullptr || ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_FXAA) == nullptr)
			Set_Reason(TAA, REASON_NO_SHADER);
		else if (camera == nullptr)
			Set_Reason(TAA, REASON_AO_NO_CAMERA);
		else if (!SceneDepthBound && DepthPassCamera != camera)
			Set_Reason(TAA, DepthReason);
		else if (Taa_Ready(device))
		{
			camera->Set_Pixel_Jitter(TaaJitter[0], TaaJitter[1]);
			TaaCamera = camera;
		}
	}

	const DWORD alpha = (DWORD)(clear_alpha * 255.0f + 0.5f) & 0xff;
	device->Clear(0, nullptr, clear, D3DCOLOR_ARGB(alpha, 0, 0, 0), 1.0f, 0);
	return true;
}

void W3DPostProcess::End_Scene(int x, int y, int width, int height, const CameraClass *camera)
{
	if (BackBuffer == nullptr)
		return;
	GpuTimer::Mark(GpuTimer::SCENE);
	// TAA runs for the camera Begin_Scene() jittered. Applying it again unjittered keeps anything
	// drawn later with its projection, without its own Apply(), in place.
	const bool taa = TaaCamera != nullptr && TaaCamera == camera;
	if (TaaCamera != nullptr)
	{
		TaaCamera->Set_Pixel_Jitter(0.0f, 0.0f);
		TaaCamera->Apply();
		TaaCamera = nullptr;
	}
	const bool scene_depth = SceneDepthBound;
	SceneDepthBound = false;
	SceneCamera = nullptr;
	// Only for the camera the pass drew with; another view has other depth.
	const bool depth_pass = DepthPassCamera != nullptr && DepthPassCamera == camera;
	if (depth_pass)
		DepthPassCamera = nullptr;

	IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
	const float sun[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	device->SetPixelShaderConstantF(FRAME_PS_SUN, sun, 1);
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

	// Without a state block the states cannot be put back, so the back buffer stays as the frame
	// clear left it.
	if (!Save_States(device) || width <= 0 || height <= 0)
	{
		Release_Saved_Targets();
		return;
	}

	Set_Post_States(device);

	// Which effects of the final pass run. Bloom also needs its own shaders and textures, colour
	// grading its LUT texture; without any effect the scene is only copied.
	const GraphicsOptions &o = Get_Graphics_Options();
	bool bloom = Effect_On(BLOOM, o.Bloom, false, REASON_EFFECT_OFF, ShaderLibrary::PS_POST_FINAL);
	if (bloom && (ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_BRIGHT) == nullptr || ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_DOWN) == nullptr
		|| ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_BLUR) == nullptr))
	{
		Set_Reason(BLOOM, REASON_NO_SHADER);
		bloom = false;
	}
	if (bloom)
		bloom = Bloom_Ready(device);
	const bool tone = Effect_On(TONE_MAPPING, o.ToneMapping, o.DebugToneMappingOff, REASON_TONE_DEBUG_OFF,
		ShaderLibrary::PS_POST_FINAL);
	bool grade = Effect_On(COLOR_GRADING, o.ColorGrading, o.DebugColorGradingOff, REASON_GRADE_DEBUG_OFF,
		ShaderLibrary::PS_POST_FINAL);
	if (grade)
		grade = Lut_Ready(device);
	bool ao = Effect_On(AMBIENT_OCCLUSION, o.AmbientOcclusion > 0, o.DebugAoMode == 2, REASON_AO_DEBUG_OFF,
		ShaderLibrary::PS_POST_FINAL);
	if (ao && (ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_AO) == nullptr || ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_AO_BLUR) == nullptr))
	{
		Set_Reason(AMBIENT_OCCLUSION, REASON_NO_SHADER);
		ao = false;
	}
	if (ao && !scene_depth && !depth_pass)
	{
		Set_Reason(AMBIENT_OCCLUSION, DepthReason);
		ao = false;
	}
	if (ao && camera == nullptr)
	{
		Set_Reason(AMBIENT_OCCLUSION, REASON_AO_NO_CAMERA);
		ao = false;
	}
	if (ao)
		ao = Ao_Ready(device, o.AmbientOcclusion >= 2 ? 1 : 2);
	bool haze = Effect_On(HEAT_HAZE, o.HeatHaze > 0.0f, false, REASON_EFFECT_OFF, ShaderLibrary::PS_POST_FINAL);
	if (haze && ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_HEAT_BLOB) == nullptr)
	{
		Set_Reason(HEAT_HAZE, REASON_NO_SHADER);
		haze = false;
	}
	if (haze && camera == nullptr)
	{
		Set_Reason(HEAT_HAZE, REASON_AO_NO_CAMERA);
		haze = false;
	}
	const unsigned heat_blobs = haze ? Build_Heat_Blobs(camera) : 0;
	if (haze && heat_blobs == 0)
	{
		Set_Reason(HEAT_HAZE, REASON_HEAT_NO_SOURCES);
		haze = false;
	}
	if (haze)
		haze = Heat_Ready(device);
	const bool final_effects = bloom || tone || grade || ao || haze;
	bool fxaa = Effect_On(FXAA, o.FXAA, o.DebugFxaaOff, REASON_FXAA_DEBUG_OFF, ShaderLibrary::PS_POST_FXAA);
	if (fxaa && taa)
	{
		Set_Reason(FXAA, REASON_FXAA_TAA);
		fxaa = false;
	}
	bool sharpen = Effect_On(SHARPEN, o.Sharpen > 0.0f, false, REASON_EFFECT_OFF, ShaderLibrary::PS_POST_FXAA);
	if ((fxaa || sharpen) && final_effects && !Final_Ready(device))
	{
		if (fxaa)
			Set_Reason(FXAA, REASON_FXAA_CREATE_FAILED);
		if (sharpen)
			Set_Reason(SHARPEN, REASON_FXAA_CREATE_FAILED);
		fxaa = sharpen = false;
	}
	// Sharpening runs in the FXAA pass, so the pass also runs for it alone. With TAA it puts the
	// TAA result on the back buffer (Taa_Ready() made FinalTexture).
	const bool fxaa_pass = fxaa || sharpen || taa;

	const float u0 = (float)x / tw;
	const float v0 = (float)y / th;
	const float u1 = (float)(x + width) / tw;
	const float v1 = (float)(y + height) / th;

	if (ao)
	{
		Draw_Ao(device, camera, u0, v0, u1, v1);
		GpuTimer::Mark(GpuTimer::AO);
	}
	if (bloom)
	{
		Draw_Bloom(device);
		GpuTimer::Mark(GpuTimer::BLOOM);
	}
	if (haze)
		Draw_Heat_Mask(device, heat_blobs, u0, v0, u1, v1);

	// The final pass, onto the back buffer over the view rectangle, or into the same rectangle of
	// FinalTexture when FXAA follows. The scene is read with point filtering, so each pixel gets
	// exactly its own texel. FXAA alone skips it and reads the scene texture.
	IDirect3DTexture9 *fxaa_source = SceneTexture;
	if (fxaa_pass && final_effects)
	{
		// FinalTexture has no multisampling, so no (possibly multisampled) depth buffer may be bound.
		device->SetRenderTarget(0, FinalSurface);
		device->SetDepthStencilSurface(nullptr);
		fxaa_source = FinalTexture;
	}
	// The haze moves the uv by parts of a texel, which point filtering would show as steps.
	Set_Texture(device, 0, SceneTexture, haze);
	if (final_effects)
	{
		if (bloom)
		{
			for (int i = 0; i < BLOOM_LEVELS; ++i)
				Set_Texture(device, 1 + i, Bloom[i].Texture[0], true);
		}
		if (grade)
			Set_Texture(device, 4, LutTexture, true);
		if (ao)
		{
			Set_Texture(device, POST_AO_SAMPLER, AoTexture[0], false);
			Set_Texture(device, POST_DEPTH_SAMPLER, DepthTexture, false);
		}
		const float final_flags[4] = { bloom ? 1.0f : 0.0f, tone ? 1.0f : 0.0f, grade ? 1.0f : 0.0f, ao ? 1.0f : 0.0f };
		// The curve works on linear light; 1 / (the curve at white) keeps white at white.
		const float exposure = SceneHdr ? TONE_HDR_EXPOSURE : TONE_EXPOSURE;
		const float white = SceneHdr ? powf(TONE_HDR_WHITE, 2.2f) : 1.0f;
		const float tone_params[4] = { exposure, 1.0f / Filmic_Curve(exposure * white), exposure * white, 0.0f };
		device->SetPixelShaderConstantF(POST_PS_FINAL, final_flags, 1);
		device->SetPixelShaderConstantF(POST_PS_TONE, tone_params, 1);
		// Heat haze: offsets and waves are sized by the view height, so they look the same at every
		// resolution. The moved uv stays half a texel inside the view rectangle.
		const float view_h = (float)height;
		// The waves turn a whole number of radians a second, so wrapping at 200 pi seconds does not jump.
		const float seconds = (float)fmod(timeGetTime() * 0.001, 200.0 * 3.14159265358979);
		const float heat[4] = { haze ? o.HeatHaze * HEAT_OFFSET * view_h / tw : 0.0f,
			haze ? o.HeatHaze * HEAT_OFFSET * view_h / th : 0.0f, seconds, 0.0f };
		const float heat_wave[4] = { 6.2831853f * HEAT_WAVES * tw / view_h, 6.2831853f * HEAT_WAVES * th / view_h, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(POST_PS_HEAT, heat, 1);
		device->SetPixelShaderConstantF(POST_PS_HEAT_WAVE, heat_wave, 1);
		if (haze)
		{
			const float view_clamp[4] = { u0 + 0.5f / tw, v0 + 0.5f / th, u1 - 0.5f / tw, v1 - 0.5f / th };
			device->SetPixelShaderConstantF(POST_PS_CLAMP, view_clamp, 1);
			Set_Texture(device, POST_HEAT_SAMPLER, HeatTexture, true);
		}
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_FINAL));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);
	}
	else if (!fxaa_pass)
	{
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_COPY));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);
	}
	if (final_effects || !fxaa_pass)
		GpuTimer::Mark(GpuTimer::FINAL);
	++Stats[SCENE].Passes;
	if (SceneHdr)
		++Stats[HDR].Passes;
	if (bloom)
		++Stats[BLOOM].Passes;
	if (tone)
		++Stats[TONE_MAPPING].Passes;
	if (grade)
		++Stats[COLOR_GRADING].Passes;
	if (haze)
		++Stats[HEAT_HAZE].Passes;
	if (ao)
	{
		++Stats[AMBIENT_OCCLUSION].Passes;
		AoDepthPass = !scene_depth;
	}

	if (taa)
		fxaa_source = Draw_Taa(device, camera, fxaa_source, x, y, width, height, u0, v0, u1, v1);

	// FXAA, onto the back buffer. Its reads stay half a texel inside the view rectangle, so the
	// edge of the view never blends with what lies outside it.
	if (fxaa_pass)
	{
		device->SetRenderTarget(0, BackBuffer);
		device->SetDepthStencilSurface(DepthBuffer);
		Set_Texture(device, 0, fxaa_source, true);
		Set_Texel(device, 1.0f / tw, 1.0f / th, 0.0f, 0.0f);
		const float clamp_uv[4] = { u0 + 0.5f / tw, v0 + 0.5f / th, u1 - 0.5f / tw, v1 - 0.5f / th };
		device->SetPixelShaderConstantF(POST_PS_CLAMP, clamp_uv, 1);
		const float sharpen_params[4] = { sharpen ? o.Sharpen : 0.0f, fxaa ? 1.0f : 0.0f, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(POST_PS_SHARPEN, sharpen_params, 1);
		device->SetPixelShader(ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_FXAA));
		Draw_Quad(device, x, y, width, height, u0, v0, u1, v1);
		GpuTimer::Mark(GpuTimer::FXAA);
		if (fxaa)
			++Stats[FXAA].Passes;
		if (sharpen)
			++Stats[SHARPEN].Passes;
	}

	// Puts back every state, including the textures and the stream source that DrawPrimitiveUP
	// cleared.
	SavedStates->Apply();
	Release_Saved_Targets();
}

void W3DPostProcess::Draw_Depth_Pass(SceneClass *scene, CameraClass *camera)
{
	DepthPassCamera = nullptr;
	const GraphicsOptions &o = Get_Graphics_Options();
	// Begin_Scene() makes the scene texture; until then (the first frame) the size is not known.
	if (!o.PostProcess() || !(o.PostAo() || o.TAA || o.WaterSsr()) || SceneTexture == nullptr || TargetDesc.MultiSampleType == D3DMULTISAMPLE_NONE
		|| scene == nullptr || camera == nullptr)
		return;
	IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
	if (device == nullptr || !Depth_Ready(device))
		return;	// DepthReason says why
	// The scene texture is the colour target: it has the size, no multisampling, and the pass
	// writes no colour (the resolve overwrites it anyway). With TAA the depth moves with the scene.
	const bool jitter = o.TAA && TaaFailed == REASON_COUNT;
	if (jitter)
		camera->Set_Pixel_Jitter(TaaJitter[0], TaaJitter[1]);
	const bool drawn = ShadowMap::Render_Depth(scene, camera, SceneSurface, DepthSurface);
	if (jitter)
	{
		// Render_Depth() ends with Apply(); this puts the unjittered projection back for what draws next.
		camera->Set_Pixel_Jitter(0.0f, 0.0f);
		camera->Apply();
	}
	if (!drawn)
	{
		DepthReason = REASON_AO_DEPTH_PASS_FAILED;
		Log_Once(REASON_AO_DEPTH_PASS_FAILED, 0);
		return;
	}
	DepthPassCamera = camera;
}

void W3DPostProcess::Begin_Particles(const CameraClass *camera)
{
	const GraphicsOptions &o = Get_Graphics_Options();
	if (BackBuffer == nullptr)
		return;	// no scene texture; Begin_Scene() gave the reason
	if (!o.SoftParticles)
		return Set_Reason(SOFT_PARTICLES, REASON_EFFECT_OFF);
	if (o.DebugSoftParticlesOff)
		return Set_Reason(SOFT_PARTICLES, REASON_SOFT_DEBUG_OFF);
	if (TargetDesc.MultiSampleType != D3DMULTISAMPLE_NONE)
		return Set_Reason(SOFT_PARTICLES, REASON_SOFT_MSAA);
	if (!SceneDepthBound)
		return Set_Reason(SOFT_PARTICLES, DepthReason);
	if (camera == nullptr)
		return Set_Reason(SOFT_PARTICLES, REASON_AO_NO_CAMERA);
	if (ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_DEPTH_COPY) == nullptr || ShaderLibrary::Peek_Vertex(ShaderLibrary::VS_PARTICLE) == nullptr
		|| ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_PARTICLE) == nullptr)
		return Set_Reason(SOFT_PARTICLES, REASON_NO_SHADER);

	IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
	if (ParticleDepthBound)
		return;
	if (!Particle_Depth_Ready(device))
		return Set_Reason(SOFT_PARTICLES, ParticleDepthFailed);

	// Only the main scene: a pass into another target (the water reflection) has its own depth.
	IDirect3DSurface9 *depth = nullptr;
	device->GetDepthStencilSurface(&depth);
	const bool scene_pass = depth == DepthSurface;
	Release_Com(depth);
	IDirect3DSurface9 *target = nullptr;
	device->GetRenderTarget(0, &target);
	if (!scene_pass || target == nullptr || !Save_States(device))
	{
		Release_Com(target);
		return;
	}

	Draw_Depth_Copy(device, camera, target, DepthSurface);
	target->Release();

	Set_Texture(device, PARTICLE_DEPTH_SAMPLER, ParticleDepthTexture, false);
	ParticleDepthBound = true;
	MeshShader::Set_Scene_Depth(1.0f / (float)TargetDesc.Width, 1.0f / (float)TargetDesc.Height);
	++Stats[SOFT_PARTICLES].Passes;
}

void W3DPostProcess::End_Particles()
{
	if (!ParticleDepthBound)
		return;
	ParticleDepthBound = false;
	MeshShader::Set_Scene_Depth(0.0f, 0.0f);
	IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
	if (device)
		device->SetTexture(PARTICLE_DEPTH_SAMPLER, nullptr);
}

IDirect3DTexture9 *W3DPostProcess::Copy_Scene_Depth(const char **why_not)
{
	if (BackBuffer == nullptr)
	{
		*why_not = "the scene does not draw into the post-processing scene texture";
		return nullptr;
	}
	// With multisampling only the depth pass of the 3D camera can be read.
	const bool msaa = TargetDesc.MultiSampleType != D3DMULTISAMPLE_NONE;
	if (msaa ? DepthPassCamera == nullptr || DepthPassCamera != SceneCamera : !SceneDepthBound)
	{
		*why_not = ReasonText[DepthReason];
		return nullptr;
	}
	if (SceneCamera == nullptr)
	{
		*why_not = ReasonText[REASON_AO_NO_CAMERA];
		return nullptr;
	}
	if (ShaderLibrary::Peek_Pixel(ShaderLibrary::PS_POST_DEPTH_COPY) == nullptr)
	{
		*why_not = ReasonText[REASON_NO_SHADER];
		return nullptr;
	}
	IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
	if (ParticleDepthBound)
	{
		*why_not = "the particles hold the depth copy";
		return nullptr;
	}
	if (!Particle_Depth_Ready(device))
	{
		*why_not = ReasonText[ParticleDepthFailed];
		return nullptr;
	}

	// Only the main scene: a pass into another target (the water reflection) has its own depth.
	IDirect3DSurface9 *target = nullptr;
	IDirect3DSurface9 *depth = nullptr;
	device->GetRenderTarget(0, &target);
	device->GetDepthStencilSurface(&depth);
	const bool scene_pass = msaa ? target == MsaaSurface : depth == DepthSurface;
	const bool copied = target != nullptr && scene_pass && Save_States(device);
	if (copied)
		Draw_Depth_Copy(device, SceneCamera, target, depth);
	Release_Com(target);
	Release_Com(depth);
	if (!copied)
	{
		*why_not = "the water is not drawn in the main scene";
		return nullptr;
	}
	return ParticleDepthTexture;
}

bool W3DPostProcess::Heat_Haze_Wanted()
{
	// Only while the scene draws into the scene texture, so the sources have a pass to end in.
	const GraphicsOptions &o = Get_Graphics_Options();
	return BackBuffer != nullptr && o.HeatHaze > 0.0f && o.PostProcess();
}

void W3DPostProcess::Add_Heat_Source(float x, float y, float z, float radius, float strength)
{
	if (HeatCount >= HEAT_MAX_SOURCES || !(radius > 0.0f) || !(strength > 0.0f))
		return;
	HeatSource &s = HeatSources[HeatCount++];
	s.X = x;
	s.Y = y;
	s.Z = z;
	s.Radius = radius;
	s.Strength = strength < 1.0f ? strength : 1.0f;
}

void W3DPostProcess::Release()
{
	if (ParticleDepthBound)
		End_Particles();
	if (BackBuffer != nullptr)
	{
		IDirect3DDevice9 *device = DX9Wrapper::_Get_D3D_Device9();
		if (device)
		{
			device->SetRenderTarget(0, BackBuffer);
			device->SetDepthStencilSurface(DepthBuffer);
		}
		Release_Saved_Targets();
	}
	if (TaaCamera != nullptr)
	{
		TaaCamera->Set_Pixel_Jitter(0.0f, 0.0f);
		TaaCamera = nullptr;
	}
	Release_Targets();
	Release_Com(SavedStates);
	Release_Com(LutTexture);
	LutUploaded = false;
	LutFailed = false;
	CreateFailed = false;
	HdrFailed = REASON_COUNT;
	DepthFailed = false;
	DepthReason = REASON_NOT_DRAWN;
	SceneDepthBound = false;
	SceneCamera = nullptr;
}

void W3DPostProcess::New_Frame()
{
	for (int i = 0; i < EFFECT_COUNT; ++i)
	{
		EffectStats &s = Stats[i];
		s.LastPasses = s.Passes;
		s.LastReason = s.Reason ? s.Reason : ReasonText[REASON_NOT_DRAWN];
		s.Passes = 0;
		s.Reason = ReasonText[REASON_NOT_DRAWN];
	}
	LastAoDepthPass = AoDepthPass;
	AoDepthPass = false;
	LastHeatCount = HeatCount;
	HeatCount = 0;
	++FrameCount;
	Jitter(FrameCount, TaaJitter[0], TaaJitter[1]);
}

unsigned W3DPostProcess::Last_Frame_Passes(Effect effect)
{
	return Stats[effect].LastPasses;
}

const char *W3DPostProcess::Last_Reason(Effect effect)
{
	return Stats[effect].LastReason ? Stats[effect].LastReason : ReasonText[REASON_NOT_DRAWN];
}

bool W3DPostProcess::Last_Ao_From_Depth_Pass()
{
	return LastAoDepthPass;
}

unsigned W3DPostProcess::Last_Heat_Sources()
{
	return LastHeatCount;
}

unsigned W3DPostProcess::Taa_History_Resets()
{
	return TaaResets;
}

const char *W3DPostProcess::Color_Lut_File()
{
	return LutChosen && LutFile[0] != '\0' ? LutFile : nullptr;
}
