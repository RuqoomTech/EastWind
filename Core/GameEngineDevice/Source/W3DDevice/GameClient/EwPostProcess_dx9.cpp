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
#include "WW3D2/dx9wrapper.h"
#include "WW3D2/ewshaders_dx9.h"
#include "WW3D2/ewgraphicsoptions_dx9.h"
#include "WWDebug/wwdebug.h"
#include "ew_constants.h"

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

	enum Reason
	{
		REASON_OFF,
		REASON_DEBUG_OFF,
		REASON_NO_SHADER,
		REASON_NO_TARGET,
		REASON_CREATE_FAILED,
		REASON_SET_FAILED,
		REASON_NOT_DRAWN,
		REASON_BLOOM_OFF,
		REASON_BLOOM_CREATE_FAILED,
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
	};
	static_assert(sizeof(ReasonText) / sizeof(ReasonText[0]) == REASON_COUNT, "ReasonText must match Reason");

	bool Logged[REASON_COUNT];

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

	// Set between Begin_Scene and End_Scene.
	IDirect3DSurface9 *BackBuffer = nullptr;
	IDirect3DSurface9 *DepthBuffer = nullptr;

	unsigned FramePasses = 0;
	unsigned LastFramePasses = 0;
	const char *FrameReason = ReasonText[REASON_NOT_DRAWN];
	const char *LastReason = ReasonText[REASON_NOT_DRAWN];
	unsigned BloomFramePasses = 0;
	unsigned LastBloomFramePasses = 0;
	const char *BloomFrameReason = ReasonText[REASON_NOT_DRAWN];
	const char *LastBloomReason = ReasonText[REASON_NOT_DRAWN];

	void Log_Once(Reason reason, unsigned value)
	{
		if (!Logged[reason] && reason != REASON_OFF && reason != REASON_DEBUG_OFF && reason != REASON_BLOOM_OFF)
		{
			Logged[reason] = true;
			WWDEBUG_SAY(("EwPostProcess: %s, value 0x%x (logged once)", ReasonText[reason], value));
		}
	}

	bool Fail(Reason reason, unsigned value = 0)
	{
		FrameReason = ReasonText[reason];
		BloomFrameReason = ReasonText[reason];
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
			BloomFrameReason = ReasonText[REASON_BLOOM_CREATE_FAILED];
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
			BloomFrameReason = ReasonText[REASON_BLOOM_CREATE_FAILED];
			Log_Once(REASON_BLOOM_CREATE_FAILED, hr);
			return false;
		}
		return true;
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
		++BloomFramePasses;
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

	// Bloom needs its four shaders and its textures; without them the scene is only copied.
	const EwGraphicsOptions &o = EwGraphics();
	bool bloom = false;
	if (!o.Bloom)
		BloomFrameReason = ReasonText[REASON_BLOOM_OFF];
	else if (EwShaders::Peek_Pixel(EwShaders::PS_POST_BRIGHT) == nullptr || EwShaders::Peek_Pixel(EwShaders::PS_POST_DOWN) == nullptr
		|| EwShaders::Peek_Pixel(EwShaders::PS_POST_BLUR) == nullptr || EwShaders::Peek_Pixel(EwShaders::PS_POST_BLOOM) == nullptr)
		BloomFrameReason = ReasonText[REASON_NO_SHADER];
	else
		bloom = Bloom_Ready(device);

	if (bloom)
		Draw_Bloom(device);

	// The last pass, onto the back buffer over the view rectangle. The scene is read with point
	// filtering, so each pixel gets exactly its own texel.
	Set_Texture(device, 0, SceneTexture, false);
	if (bloom)
	{
		for (int i = 0; i < BLOOM_LEVELS; ++i)
			Set_Texture(device, 1 + i, Bloom[i].Texture[0], true);
		device->SetPixelShader(EwShaders::Peek_Pixel(EwShaders::PS_POST_BLOOM));
	}
	else
	{
		device->SetPixelShader(EwShaders::Peek_Pixel(EwShaders::PS_POST_COPY));
	}
	Draw_Quad(device, x, y, width, height, (float)x / tw, (float)y / th, (float)(x + width) / tw, (float)(y + height) / th);
	++FramePasses;
	if (bloom)
		++BloomFramePasses;

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
	CreateFailed = false;
}

void EwPostProcess::New_Frame()
{
	LastFramePasses = FramePasses;
	LastReason = FrameReason;
	FramePasses = 0;
	FrameReason = ReasonText[REASON_NOT_DRAWN];
	LastBloomFramePasses = BloomFramePasses;
	LastBloomReason = BloomFrameReason;
	BloomFramePasses = 0;
	BloomFrameReason = ReasonText[REASON_NOT_DRAWN];
}

unsigned EwPostProcess::Last_Frame_Passes()
{
	return LastFramePasses;
}

const char *EwPostProcess::Last_Reason()
{
	return LastReason;
}

unsigned EwPostProcess::Last_Frame_Bloom_Passes()
{
	return LastBloomFramePasses;
}

const char *EwPostProcess::Last_Bloom_Reason()
{
	return LastBloomReason;
}
