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

// EastWind DX9: sun shadow map (M3.4). See shadowmap.h.

#include "shadowmap.h"
#include "graphicsoptions.h"
#include "shader_constants.h"
#include "dx9wrapper.h"
#include "dx9caps.h"
#include "shaderlibrary.h"
#include "ww3d.h"
#include "camera.h"
#include "scene.h"
#include "WWMath/matrix4.h"
#include "WWDebug/wwdebug.h"

#include <math.h>
#include <string.h>

namespace
{
	const D3DFORMAT FORMAT_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
	const D3DFORMAT FORMAT_NULL = (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L');

	// Light box fit, in world units.
	const float MAX_VIEW_DISTANCE = 3000.0f;	// the corner rays of the view are cut off here
	const float CASTER_HEIGHT = 400.0f;			// casters this far above the ground (toward the sun) still cast
	const float BOX_STEP = 32.0f;				// the box size changes in steps, so the texel size stays put
	const float MIN_HALF_SIZE = 128.0f;

	// Filtering and bias.
	const float SOFT_EDGE = 2.5f;				// world units over which a shadow edge softens
	const float MIN_RADIUS_TEXELS = 1.5f;
	const float MAX_RADIUS_TEXELS = 4.0f;
	const float NORMAL_OFFSET_TEXELS = 1.5f;	// receivers move along their normal by this, at grazing light
	const float DEPTH_BIAS_WORLD = 1.5f;		// constant bias toward the light
	const float SLOPE_BIAS = 2.0f;				// D3DRS_SLOPESCALEDEPTHBIAS in the caster pass
	const float STRENGTH = 1.0f;				// 1: shadows remove all of the sun's direct light

	enum DepthFormat { DEPTH_NONE, DEPTH_D24, DEPTH_INTZ };

	struct Frame
	{
		bool Drawn;
		unsigned Casters;
		float Box;
		const char *WhyNot;
	};

	IDirect3DTexture9 *DepthTexture = nullptr;
	IDirect3DSurface9 *DepthSurface = nullptr;
	IDirect3DSurface9 *ColorSurface = nullptr;
	CameraClass *LightCamera = nullptr;
	DepthFormat Format = DEPTH_NONE;
	int Size = 0;
	const char *CreateFailed = nullptr;		// why making the resources failed for FailedSize
	int FailedSize = 0;

	bool InCasterPass = false;
	bool IsActive = false;
	Vector3 ToLight(0, 0, 1);
	Frame This = { false, 0, 0.0f, "not drawn yet" };
	Frame Last = { false, 0, 0.0f, "not drawn yet" };

	// Light-space box of this frame, for Caster_Visible.
	Vector3 BoxAxisX, BoxAxisY, BoxAxisZ;
	float BoxCenterX = 0, BoxCenterY = 0, BoxHalf = 0, BoxZMin = 0, BoxZMax = 0;

	template <class T> void Release_Com(T *&p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	void Free_Resources()
	{
		Release_Com(DepthSurface);
		Release_Com(DepthTexture);
		Release_Com(ColorSurface);
		Format = DEPTH_NONE;
		Size = 0;
	}

	// Makes the depth texture and a colour target of the same size (the camera sizes its
	// viewport from the colour target). Returns why not on failure.
	const char *Create_Resources(IDirect3DDevice9Ex *device, int size)
	{
		Free_Resources();

		D3DFORMAT depth_format;
		if (ShaderLibrary::Format_Supported(device, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24X8))
		{
			Format = DEPTH_D24;
			depth_format = D3DFMT_D24X8;
		}
		else if (ShaderLibrary::Format_Supported(device, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FORMAT_INTZ))
		{
			Format = DEPTH_INTZ;
			depth_format = FORMAT_INTZ;
		}
		else
			return "the card has no depth texture format (D24X8 or INTZ)";

		if (FAILED(device->CreateTexture(size, size, 1, D3DUSAGE_DEPTHSTENCIL, depth_format, D3DPOOL_DEFAULT, &DepthTexture, nullptr))
			|| FAILED(DepthTexture->GetSurfaceLevel(0, &DepthSurface)))
		{
			Free_Resources();
			return "creating the depth texture failed (out of video memory?)";
		}

		// A NULL render target takes no memory; otherwise the smallest colour target there is.
		const D3DFORMAT colors[] = { FORMAT_NULL, D3DFMT_R5G6B5, D3DFMT_X8R8G8B8 };
		for (int i = 0; i < 3 && ColorSurface == nullptr; ++i)
		{
			if (!ShaderLibrary::Format_Supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, colors[i]))
				continue;
			if (FAILED(device->CreateRenderTarget(size, size, colors[i], D3DMULTISAMPLE_NONE, 0, FALSE, &ColorSurface, nullptr)))
				ColorSurface = nullptr;
		}
		if (ColorSurface == nullptr)
		{
			Free_Resources();
			return "creating the colour target failed";
		}

		Size = size;
		WWDEBUG_SAY(("ShadowMap: %dx%d %s depth map", size, size, Format == DEPTH_D24 ? "D24X8 (hardware PCF)" : "INTZ"));
		return nullptr;
	}

	// Constants c200..c205: on/off, matrix, filter and bias.
	void Set_Constants(IDirect3DDevice9Ex *device, const Matrix4x4 *uvz, float texel_world, float depth_range)
	{
		float c[SHADOW_PS_COUNT][4];
		memset(c, 0, sizeof(c));
		if (uvz != nullptr)
		{
			for (int r = 0; r < 3; ++r)
				for (int k = 0; k < 4; ++k)
					c[SHADOW_PS_MATRIX - SHADOW_PS_FIRST + r][k] = (*uvz)[r][k];

			float radius = SOFT_EDGE / texel_world;
			if (radius < MIN_RADIUS_TEXELS) radius = MIN_RADIUS_TEXELS;
			if (radius > MAX_RADIUS_TEXELS) radius = MAX_RADIUS_TEXELS;
			float *p = c[SHADOW_PS_PARAMS - SHADOW_PS_FIRST];
			p[0] = 1.0f;
			p[1] = 1.0f / (float)Size;
			p[2] = radius;
			p[3] = texel_world * NORMAL_OFFSET_TEXELS;
			float *p2 = c[SHADOW_PS_PARAMS2 - SHADOW_PS_FIRST];
			p2[0] = Format == DEPTH_D24 ? 1.0f : 0.0f;
			p2[1] = DEPTH_BIAS_WORLD / depth_range;
			p2[3] = STRENGTH;
		}
		device->SetPixelShaderConstantF(SHADOW_PS_FIRST, &c[0][0], SHADOW_PS_COUNT);
	}

	void Bind(IDirect3DDevice9Ex *device, bool on)
	{
		device->SetTexture(SHADOW_SAMPLER, on ? DepthTexture : nullptr);
		if (!on)
			return;
		const DWORD filter = Format == DEPTH_D24 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_MINFILTER, filter);
		device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_MAGFILTER, filter);
		device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_SRGBTEXTURE, FALSE);
	}

	void Turn_Off(const char *why_not)
	{
		IDirect3DDevice9Ex *device = DX9Wrapper::_Get_D3D_Device9();
		if (device != nullptr)
		{
			Set_Constants(device, nullptr, 1.0f, 1.0f);
			Bind(device, false);
		}
		IsActive = false;
		This.Drawn = false;
		This.WhyNot = why_not;
	}

	// The point where a ray from the camera meets height z, at most max_t along dir.
	Vector3 Ray_At_Height(const Vector3 &origin, const Vector3 &dir, float z, float max_t)
	{
		float t = max_t;
		if (dir.Z < -1.0e-4f)
		{
			t = (z - origin.Z) / dir.Z;
			if (t < 0.0f) t = 0.0f;
			if (t > max_t) t = max_t;
		}
		return origin + dir * t;
	}
}

void ShadowMap::New_Frame()
{
	Last = This;
	Turn_Off("not drawn this frame");
	This.Casters = 0;
	This.Box = 0.0f;
}

bool ShadowMap::Caster_Pass()
{
	return InCasterPass;
}

bool ShadowMap::Active()
{
	return IsActive;
}

bool ShadowMap::Is_Sun(float x, float y, float z)
{
	return IsActive && x * ToLight.X + y * ToLight.Y + z * ToLight.Z > 0.99f;
}

bool ShadowMap::Caster_Visible(const Vector3 &center, float radius)
{
	const float x = Vector3::Dot_Product(center, BoxAxisX) - BoxCenterX;
	const float y = Vector3::Dot_Product(center, BoxAxisY) - BoxCenterY;
	const float z = Vector3::Dot_Product(center, BoxAxisZ);
	const float reach = BoxHalf + radius;
	return x > -reach && x < reach && y > -reach && y < reach && z + radius > BoxZMin && z - radius < BoxZMax;
}

void ShadowMap::Count_Caster()
{
	++This.Casters;
}

bool ShadowMap::Last_Frame(int *size, const char **format, unsigned *casters, float *box, const char **why_not)
{
	*size = Size;
	*format = Format == DEPTH_D24 ? "D24X8, hardware PCF" : Format == DEPTH_INTZ ? "INTZ" : "none";
	*casters = Last.Casters;
	*box = Last.Box;
	*why_not = Last.WhyNot;
	return Last.Drawn;
}

void ShadowMap::Release()
{
	Free_Resources();
	CreateFailed = nullptr;
	FailedSize = 0;
	IsActive = false;
	if (LightCamera != nullptr)
	{
		LightCamera->Release_Ref();
		LightCamera = nullptr;
	}
}

void ShadowMap::Render(SceneClass *scene, CameraClass *view, const Vector3 &sun_travel, float ground_min, float ground_max)
{
	const GraphicsOptions &options = Get_Graphics_Options();
	if (!options.ShadowMaps())
	{
		if (Size != 0)
			Free_Resources();
		Turn_Off(options.ShadowMapSize <= 0 ? "off in Options.ini" : "off (debug key or the lit shaders are off)");
		return;
	}
	IDirect3DDevice9Ex *device = DX9Wrapper::_Get_D3D_Device9();
	if (device == nullptr || scene == nullptr || view == nullptr)
	{
		Turn_Off("no scene");
		return;
	}
	const D3DCAPS9 &caps = DX9Wrapper::Get_Current_Caps()->Get_DX9_Caps();
	if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
	{
		Turn_Off("the card has no ps_3_0");
		return;
	}

	// Resources, made once for the size in Options.ini; a failure is not retried until Release().
	if (Size != options.ShadowMapSize)
	{
		if (FailedSize != options.ShadowMapSize)
		{
			CreateFailed = Create_Resources(device, options.ShadowMapSize);
			if (CreateFailed != nullptr)
				FailedSize = options.ShadowMapSize;
		}
		if (Size != options.ShadowMapSize)
		{
			Turn_Off(CreateFailed);
			return;
		}
	}

	// The sun. Light from below the horizon casts nothing sensible.
	Vector3 to_light = -sun_travel;
	if (to_light.Length2() < 1.0e-6f)
	{
		Turn_Off("the map has no sun");
		return;
	}
	to_light.Normalize();
	if (to_light.Z < 0.05f)
	{
		Turn_Off("the sun is below the horizon");
		return;
	}
	ToLight = to_light;

	// Light basis: z toward the sun, x level with the ground.
	Vector3 axis_z = to_light;
	Vector3 axis_x;
	Vector3::Cross_Product(Vector3(0, 0, 1), axis_z, &axis_x);
	if (axis_x.Length2() < 1.0e-4f)
		axis_x.Set(1, 0, 0);
	axis_x.Normalize();
	Vector3 axis_y;
	Vector3::Cross_Product(axis_z, axis_x, &axis_y);

	// The ground the view sees: its four corner rays between the lowest and highest terrain.
	Vector2 view_min, view_max;
	view->Get_View_Plane(view_min, view_max);
	float view_near, view_far;
	view->Get_Clip_Planes(view_near, view_far);
	const float max_t = view_far < MAX_VIEW_DISTANCE ? view_far : MAX_VIEW_DISTANCE;
	const Matrix3D &view_tm = view->Get_Transform();
	const Vector3 origin = view_tm.Get_Translation();
	const Vector2 corners[4] = { view_min, Vector2(view_max.X, view_min.Y), view_max, Vector2(view_min.X, view_max.Y) };

	float min_x = 1.0e30f, max_x = -1.0e30f, min_y = 1.0e30f, max_y = -1.0e30f, min_z = 1.0e30f, max_z = -1.0e30f;
	for (int i = 0; i < 4; ++i)
	{
		const Vector3 dir = view_tm.Rotate_Vector(Vector3(corners[i].X, corners[i].Y, -1.0f));
		for (int h = 0; h < 2; ++h)
		{
			const Vector3 p = Ray_At_Height(origin, dir, h == 0 ? ground_min : ground_max, max_t);
			const float x = Vector3::Dot_Product(p, axis_x);
			const float y = Vector3::Dot_Product(p, axis_y);
			const float z = Vector3::Dot_Product(p, axis_z);
			if (x < min_x) min_x = x;
			if (x > max_x) max_x = x;
			if (y < min_y) min_y = y;
			if (y > max_y) max_y = y;
			if (z < min_z) min_z = z;
			if (z > max_z) max_z = z;
		}
	}

	// Square box in steps, centre on whole texels: the shadows do not crawl as the view scrolls.
	float half = 0.5f * ((max_x - min_x) > (max_y - min_y) ? (max_x - min_x) : (max_y - min_y));
	if (half < MIN_HALF_SIZE) half = MIN_HALF_SIZE;
	half = ceilf(half / BOX_STEP) * BOX_STEP;
	const float texel = 2.0f * half / (float)Size;
	const float center_x = floorf(0.5f * (min_x + max_x) / texel + 0.5f) * texel;
	const float center_y = floorf(0.5f * (min_y + max_y) / texel + 0.5f) * texel;
	const float camera_z = max_z + CASTER_HEIGHT;
	const float near_plane = 1.0f;
	const float far_plane = camera_z - min_z + 20.0f;

	BoxAxisX = axis_x;
	BoxAxisY = axis_y;
	BoxAxisZ = axis_z;
	BoxCenterX = center_x;
	BoxCenterY = center_y;
	BoxHalf = half;
	BoxZMin = min_z - 20.0f;
	BoxZMax = camera_z;

	if (LightCamera == nullptr)
		LightCamera = NEW_REF(CameraClass, ());
	const Vector3 position = axis_x * center_x + axis_y * center_y + axis_z * camera_z;
	LightCamera->Set_Transform(Matrix3D(axis_x, axis_y, axis_z, position));
	LightCamera->Set_Projection_Type(CameraClass::ORTHO);
	LightCamera->Set_Viewport(Vector2(0, 0), Vector2(1, 1));
	LightCamera->Set_View_Plane(Vector2(-half, -half), Vector2(half, half));
	LightCamera->Set_Clip_Planes(near_plane, far_plane);
	LightCamera->Set_Depth_Range(0.0f, 1.0f);

	// The caster pass: depth only, into our depth texture.
	Bind(device, false);
	DX9Wrapper::Set_Render_Target(ColorSurface, DepthSurface);
	if (WW3D::Begin_Render(false, true, Vector3(0, 0, 0)) != WW3D_ERROR_OK)
	{
		DX9Wrapper::Set_Render_Target((IDirect3DSurface9 *)nullptr);
		Turn_Off("WW3D::Begin_Render failed");
		return;
	}
	const bool slope_bias = (caps.RasterCaps & D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS) != 0;
	DX9Wrapper::Set_DX9_Render_State(D3DRS_COLORWRITEENABLE, 0);
	if (slope_bias)
	{
		const float bias = SLOPE_BIAS;
		DWORD bits;
		memcpy(&bits, &bias, sizeof(bits));
		DX9Wrapper::Set_DX9_Render_State(D3DRS_SLOPESCALEDEPTHBIAS, bits);
	}

	InCasterPass = true;
	WW3D::Render(scene, LightCamera);
	InCasterPass = false;

	DX9Wrapper::Set_DX9_Render_State(D3DRS_COLORWRITEENABLE, 0x0000000f);
	if (slope_bias)
		DX9Wrapper::Set_DX9_Render_State(D3DRS_SLOPESCALEDEPTHBIAS, 0);
	WW3D::End_Render(false);
	DX9Wrapper::Set_Render_Target((IDirect3DSurface9 *)nullptr);
	view->Apply();

	// World to map u, v and depth. D3D9 puts texel centres half a texel in.
	Matrix4x4 projection;
	LightCamera->Get_D3D_Projection_Matrix(&projection);
	Matrix3D view_matrix;
	LightCamera->Get_View_Matrix(&view_matrix);
	const Matrix4x4 clip = projection * Matrix4x4(view_matrix);
	const float half_texel = 0.5f / (float)Size;
	Matrix4x4 uvz;
	uvz[0] = clip[0] * 0.5f + Vector4(0, 0, 0, 0.5f + half_texel);
	uvz[1] = clip[1] * -0.5f + Vector4(0, 0, 0, 0.5f + half_texel);
	uvz[2] = clip[2];
	uvz[3] = Vector4(0, 0, 0, 1);

	Set_Constants(device, &uvz, texel, far_plane - near_plane);
	Bind(device, true);
	IsActive = true;
	This.Drawn = true;
	This.Box = 2.0f * half;
	This.WhyNot = nullptr;
}
