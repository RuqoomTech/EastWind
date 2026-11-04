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
	// The corner rays of the view are cut off at this view depth. The default camera (310 high, 37.5
	// degrees down) meets flat ground at about 950 in the top corners; past the far box the shader fades
	// to lit.
	const float MAX_VIEW_DISTANCE = 1500.0f;
	const float CASTER_HEIGHT = 400.0f;			// casters this far above the ground (toward the sun) still cast
	const float BOX_STEP = 32.0f;				// the box size changes in steps, so the texel size stays put
	const float MIN_HALF_SIZE = 128.0f;
	const float SHRINK_BELOW = 0.85f;			// a held box side shrinks only when the view needs less than this of it
	const int ROLL_STEPS = 72;					// roll angles tried over a quarter turn
	const float ROLL_KEEP = 0.95f;				// the held roll stays unless another gives a box this much smaller

	// Filtering and bias.
	const float SOFT_EDGE = 1.5f;				// world units over which a shadow edge softens (INTZ: at most)
	const float MIN_RADIUS_TEXELS = 1.5f;
	const float MAX_RADIUS_TEXELS = 16.0f;
	const float SOFT_AT_CONTACT = 0.15f;		// INTZ: edge width where the caster touches the receiver
	const float SOFT_PER_UNIT = 0.015f;			// INTZ: edge width added per world unit from caster to receiver
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
	CameraClass *DepthCamera = nullptr;		// set while Render_Depth() draws; Caster_Visible culls to its frustum
	bool IsActive = false;
	Vector3 ToLight(0, 0, 1);
	Frame This = { false, 0, 0.0f, "not drawn yet" };
	Frame Last = { false, 0, 0.0f, "not drawn yet" };

	// Light-space box of this frame, for Caster_Visible.
	Vector3 BoxAxisX, BoxAxisY, BoxAxisZ;
	float BoxCenterX = 0, BoxCenterY = 0, BoxHalfX = 0, BoxHalfY = 0, BoxZMin = 0, BoxZMax = 0;

	// The light box fit held across frames, so the map does not re-register as the view moves a little.
	// A zero FitToLight (no sun yet, or the resources were made again) makes Render() fit afresh.
	Vector3 FitToLight(0, 0, 0);
	int FitRoll = 0;
	float FitHalfX[2] = { 0, 0 }, FitHalfY[2] = { 0, 0 };	// [0] near, [1] far

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
		FitToLight.Set(0, 0, 0);
	}

	// Makes the depth texture and a colour target of the same size (the camera sizes its
	// viewport from the colour target). Returns why not on failure.
	const char *Create_Resources(IDirect3DDevice9Ex *device, int size)
	{
		Free_Resources();

		// INTZ first: the shader reads the casters' depth, so edges are sharp where a caster meets
		// the ground and soften with distance. D24X8 (hardware PCF) keeps one fixed softness.
		D3DFORMAT depth_format;
		if (ShaderLibrary::Format_Supported(device, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FORMAT_INTZ))
		{
			Format = DEPTH_INTZ;
			depth_format = FORMAT_INTZ;
		}
		else if (ShaderLibrary::Format_Supported(device, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24X8))
		{
			Format = DEPTH_D24;
			depth_format = D3DFMT_D24X8;
		}
		else
			return "the card has no depth texture format (D24X8 or INTZ)";

		// The atlas holds the two cascades side by side.
		if (DX9Wrapper::Get_Current_Caps()->Get_DX9_Caps().MaxTextureWidth < (DWORD)(2 * size))
			return "the card's largest texture is narrower than two cascades (lower ShadowMapSize)";
		if (FAILED(device->CreateTexture(2 * size, size, 1, D3DUSAGE_DEPTHSTENCIL, depth_format, D3DPOOL_DEFAULT, &DepthTexture, nullptr))
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
			if (FAILED(device->CreateRenderTarget(2 * size, size, colors[i], D3DMULTISAMPLE_NONE, 0, FALSE, &ColorSurface, nullptr)))
				ColorSurface = nullptr;
		}
		if (ColorSurface == nullptr)
		{
			Free_Resources();
			return "creating the colour target failed";
		}

		Size = size;
		WWDEBUG_SAY(("ShadowMap: two %dx%d cascades, %s depth map", size, size, Format == DEPTH_D24 ? "D24X8 (hardware PCF)" : "INTZ"));
		return nullptr;
	}

	// Filter radius in texels of a cascade whose texels are texel_world wide.
	float Radius_Texels(float texel_world)
	{
		float radius = SOFT_EDGE / texel_world;
		if (radius < MIN_RADIUS_TEXELS) radius = MIN_RADIUS_TEXELS;
		if (radius > MAX_RADIUS_TEXELS) radius = MAX_RADIUS_TEXELS;
		return radius;
	}

	// Constants c200..c205 (on/off, far cascade matrix, filter, softness and bias) and c207..c210
	// (near cascade matrix and filter).
	void Set_Constants(IDirect3DDevice9Ex *device, const Matrix4x4 *uvz, float texel_world, const Matrix4x4 *near_uvz,
		float near_texel_world, float depth_range)
	{
		float n[SHADOW_PS_NEAR_COUNT][4];
		memset(n, 0, sizeof(n));
		float c[SHADOW_PS_COUNT][4];
		memset(c, 0, sizeof(c));
		if (uvz != nullptr)
		{
			for (int r = 0; r < 3; ++r)
				for (int k = 0; k < 4; ++k)
				{
					c[SHADOW_PS_MATRIX - SHADOW_PS_FIRST + r][k] = (*uvz)[r][k];
					n[SHADOW_PS_NEAR_MATRIX - SHADOW_PS_NEAR_FIRST + r][k] = (*near_uvz)[r][k];
				}
			const float near_radius = Radius_Texels(near_texel_world);
			float *pn = n[SHADOW_PS_NEAR - SHADOW_PS_NEAR_FIRST];
			pn[0] = near_texel_world * (float)Size;
			pn[1] = near_radius;
			pn[2] = near_texel_world * NORMAL_OFFSET_TEXELS;
			pn[3] = near_radius * near_texel_world / depth_range;

			const float radius = Radius_Texels(texel_world);
			float *p = c[SHADOW_PS_PARAMS - SHADOW_PS_FIRST];
			p[0] = 1.0f;
			p[1] = 1.0f / (float)Size;
			p[2] = radius;
			p[3] = texel_world * NORMAL_OFFSET_TEXELS;
			float *p2 = c[SHADOW_PS_PARAMS2 - SHADOW_PS_FIRST];
			p2[0] = Format == DEPTH_D24 ? 1.0f : 0.0f;
			p2[1] = DEPTH_BIAS_WORLD / depth_range;
			p2[2] = radius * texel_world / depth_range;	// filter radius in depth units, for the slope bias
			float *p3 = c[SHADOW_PS_PARAMS3 - SHADOW_PS_FIRST];
			p3[0] = texel_world * (float)Size;
			p3[1] = depth_range;
			p3[2] = SOFT_AT_CONTACT;
			p3[3] = SOFT_PER_UNIT;
			p2[3] = STRENGTH;
		}
		device->SetPixelShaderConstantF(SHADOW_PS_FIRST, &c[0][0], SHADOW_PS_COUNT);
		device->SetPixelShaderConstantF(SHADOW_PS_NEAR_FIRST, &n[0][0], SHADOW_PS_NEAR_COUNT);
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
			Set_Constants(device, nullptr, 1.0f, nullptr, 1.0f, 1.0f);
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

	// A held box half size: grows at once to what the view needs, shrinks only when it needs clearly
	// less, so the texel size stays put while the view scrolls and zooms a little.
	void Hold_Half(float &held, float needed)
	{
		if (needed < MIN_HALF_SIZE) needed = MIN_HALF_SIZE;
		needed = ceilf(needed / BOX_STEP) * BOX_STEP;
		if (needed > held || needed < held * SHRINK_BELOW)
			held = needed;
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
	if (DepthCamera != nullptr)
		return !DepthCamera->Cull_Sphere(SphereClass(center, radius));
	const float x = Vector3::Dot_Product(center, BoxAxisX) - BoxCenterX;
	const float y = Vector3::Dot_Product(center, BoxAxisY) - BoxCenterY;
	const float z = Vector3::Dot_Product(center, BoxAxisZ);
	const float reach_x = BoxHalfX + radius;
	const float reach_y = BoxHalfY + radius;
	return x > -reach_x && x < reach_x && y > -reach_y && y < reach_y && z + radius > BoxZMin && z - radius < BoxZMax;
}

void ShadowMap::Count_Caster()
{
	if (DepthCamera == nullptr)
		++This.Casters;
}

bool ShadowMap::Render_Depth(SceneClass *scene, CameraClass *camera, IDirect3DSurface9 *color, IDirect3DSurface9 *depth)
{
	DX9Wrapper::Set_Render_Target(color, depth);
	if (WW3D::Begin_Render(false, true, Vector3(0, 0, 0)) != WW3D_ERROR_OK)
	{
		DX9Wrapper::Set_Render_Target((IDirect3DSurface9 *)nullptr);
		return false;
	}
	DX9Wrapper::Set_DX9_Render_State(D3DRS_COLORWRITEENABLE, 0);
	InCasterPass = true;
	DepthCamera = camera;
	WW3D::Render(scene, camera);
	DepthCamera = nullptr;
	InCasterPass = false;
	DX9Wrapper::Set_DX9_Render_State(D3DRS_COLORWRITEENABLE, 0x0000000f);
	WW3D::End_Render(false);
	DX9Wrapper::Set_Render_Target((IDirect3DSurface9 *)nullptr);
	camera->Apply();
	return true;
}

bool ShadowMap::Last_Frame(int *size, const char **format, unsigned *casters, float *box, const char **why_not)
{
	*size = Size;
	*format = Format == DEPTH_D24 ? "D24X8, hardware PCF" : Format == DEPTH_INTZ ? "INTZ, contact-hardening" : "none";
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
	if (Vector3::Dot_Product(to_light, FitToLight) < 0.9999f)
	{
		FitToLight = to_light;
		FitRoll = -1;
	}

	// Light basis: z toward the sun, x level with the ground (rolled below).
	Vector3 axis_z = to_light;
	Vector3 axis_x;
	Vector3::Cross_Product(Vector3(0, 0, 1), axis_z, &axis_x);
	if (axis_x.Length2() < 1.0e-4f)
		axis_x.Set(1, 0, 0);
	axis_x.Normalize();
	Vector3 axis_y;
	Vector3::Cross_Product(axis_z, axis_x, &axis_y);

	// The ground the view sees: its four corner rays between the lowest and highest terrain. The far
	// cascade takes the rays out to the far plane, the near cascade only to the view depth where the
	// centre of the view meets the ground, so it covers the lower half of the screen in finer texels.
	Vector2 view_min, view_max;
	view->Get_View_Plane(view_min, view_max);
	float view_near, view_far;
	view->Get_Clip_Planes(view_near, view_far);
	const float max_t = view_far < MAX_VIEW_DISTANCE ? view_far : MAX_VIEW_DISTANCE;
	const Matrix3D &view_tm = view->Get_Transform();
	const Vector3 origin = view_tm.Get_Translation();
	const Vector2 corners[4] = { view_min, Vector2(view_max.X, view_min.Y), view_max, Vector2(view_min.X, view_max.Y) };
	const Vector3 centre_dir = view_tm.Rotate_Vector(Vector3(0, 0, -1.0f));
	float near_t = (Ray_At_Height(origin, centre_dir, 0.5f * (ground_min + ground_max), max_t) - origin).Length();
	if (near_t > max_t) near_t = max_t;

	Vector3 points[2][8];	// per cascade: the corner rays at the lowest and highest ground
	for (int i = 0; i < 4; ++i)
	{
		const Vector3 dir = view_tm.Rotate_Vector(Vector3(corners[i].X, corners[i].Y, -1.0f));
		for (int h = 0; h < 2; ++h)
			for (int c = 0; c < 2; ++c)
				points[c][2 * i + h] = Ray_At_Height(origin, dir, h == 0 ? ground_min : ground_max, c == 0 ? near_t : max_t);
	}

	// Roll the basis around z to the angle whose far box has the shortest longer side, which sets the
	// texel size the shader sees: the view's ground is a trapezoid sheared by the sun, so no fixed angle
	// fits it well. A rectangle repeats every quarter turn. Both
	// cascades take this one roll, the far one's, since it holds most of the texels' waste. The held
	// roll stays unless another is clearly better, as a new roll re-registers the whole map.
	{
		float px[8], py[8];
		for (int i = 0; i < 8; ++i)
		{
			px[i] = Vector3::Dot_Product(points[1][i], axis_x);
			py[i] = Vector3::Dot_Product(points[1][i], axis_y);
		}
		float best_side = 1.0e30f, held_side = 1.0e30f;
		int best_roll = 0;
		for (int a = 0; a < ROLL_STEPS; ++a)
		{
			const float angle = (float)a * (0.5f * WWMATH_PI / (float)ROLL_STEPS);
			const float ca = cosf(angle), sa = sinf(angle);
			float min_a = 1.0e30f, max_a = -1.0e30f, min_b = 1.0e30f, max_b = -1.0e30f;
			for (int i = 0; i < 8; ++i)
			{
				const float va = ca * px[i] + sa * py[i];
				const float vb = ca * py[i] - sa * px[i];
				if (va < min_a) min_a = va;
				if (va > max_a) max_a = va;
				if (vb < min_b) min_b = vb;
				if (vb > max_b) max_b = vb;
			}
			const float side = (max_a - min_a) > (max_b - min_b) ? (max_a - min_a) : (max_b - min_b);
			if (side < best_side)
			{
				best_side = side;
				best_roll = a;
			}
			if (a == FitRoll)
				held_side = side;
		}
		if (FitRoll < 0 || best_side < held_side * ROLL_KEEP)
		{
			FitRoll = best_roll;
			// The map re-registers anyway: fit its size afresh too.
			FitHalfX[0] = FitHalfX[1] = FitHalfY[0] = FitHalfY[1] = 0.0f;
		}
		const float angle = (float)FitRoll * (0.5f * WWMATH_PI / (float)ROLL_STEPS);
		const float ca = cosf(angle), sa = sinf(angle);
		const Vector3 rolled_x = axis_x * ca + axis_y * sa;
		axis_y = axis_y * ca - axis_x * sa;
		axis_x = rolled_x;
	}

	struct Cascade
	{
		float MinX, MaxX, MinY, MaxY, MinZ, MaxZ;
		float HalfX, HalfY, CenterX, CenterY;
		float Texel;	// the coarser axis' texel, for the shader's one texel size
		Matrix4x4 Uvz;
	};
	Cascade cascades[2];	// [0] near, [1] far
	for (int c = 0; c < 2; ++c)
	{
		Cascade &k = cascades[c];
		k.MinX = k.MinY = k.MinZ = 1.0e30f;
		k.MaxX = k.MaxY = k.MaxZ = -1.0e30f;
		for (int i = 0; i < 8; ++i)
		{
			const Vector3 &p = points[c][i];
			const float x = Vector3::Dot_Product(p, axis_x);
			const float y = Vector3::Dot_Product(p, axis_y);
			const float z = Vector3::Dot_Product(p, axis_z);
			if (x < k.MinX) k.MinX = x;
			if (x > k.MaxX) k.MaxX = x;
			if (y < k.MinY) k.MinY = y;
			if (y > k.MaxY) k.MaxY = y;
			if (z < k.MinZ) k.MinZ = z;
			if (z > k.MaxZ) k.MaxZ = z;
		}

		// Box sides held in steps, centre on whole texels per axis: the shadows do not crawl as the
		// view scrolls.
		Hold_Half(FitHalfX[c], 0.5f * (k.MaxX - k.MinX));
		Hold_Half(FitHalfY[c], 0.5f * (k.MaxY - k.MinY));
		k.HalfX = FitHalfX[c];
		k.HalfY = FitHalfY[c];
		const float texel_x = 2.0f * k.HalfX / (float)Size;
		const float texel_y = 2.0f * k.HalfY / (float)Size;
		k.Texel = texel_x > texel_y ? texel_x : texel_y;
		k.CenterX = floorf(0.5f * (k.MinX + k.MaxX) / texel_x + 0.5f) * texel_x;
		k.CenterY = floorf(0.5f * (k.MinY + k.MaxY) / texel_y + 0.5f) * texel_y;
	}

	// Both cascades share the far one's depth range, so depth bias and softness match across them.
	const float camera_z = cascades[1].MaxZ + CASTER_HEIGHT;
	const float near_plane = 1.0f;
	const float far_plane = camera_z - cascades[1].MinZ + 20.0f;

	BoxAxisX = axis_x;
	BoxAxisY = axis_y;
	BoxAxisZ = axis_z;
	BoxZMin = cascades[1].MinZ - 20.0f;
	BoxZMax = camera_z;

	if (LightCamera == nullptr)
		LightCamera = NEW_REF(CameraClass, ());

	// The caster pass: depth only, into our depth texture, one cascade into each half.
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
	for (int c = 0; c < 2; ++c)
	{
		Cascade &k = cascades[c];
		BoxCenterX = k.CenterX;
		BoxCenterY = k.CenterY;
		BoxHalfX = k.HalfX;
		BoxHalfY = k.HalfY;

		const Vector3 position = axis_x * k.CenterX + axis_y * k.CenterY + axis_z * camera_z;
		LightCamera->Set_Transform(Matrix3D(axis_x, axis_y, axis_z, position));
		LightCamera->Set_Projection_Type(CameraClass::ORTHO);
		LightCamera->Set_Viewport(Vector2(0.5f * c, 0), Vector2(0.5f * c + 0.5f, 1));
		LightCamera->Set_View_Plane(Vector2(-k.HalfX, -k.HalfY), Vector2(k.HalfX, k.HalfY));
		LightCamera->Set_Clip_Planes(near_plane, far_plane);
		LightCamera->Set_Depth_Range(0.0f, 1.0f);
		WW3D::Render(scene, LightCamera);

		// World to the cascade's own u, v (0..1) and depth; the shader moves u into its half of the
		// atlas. D3D9 puts texel centres half a texel in.
		Matrix4x4 projection;
		LightCamera->Get_D3D_Projection_Matrix(&projection);
		Matrix3D view_matrix;
		LightCamera->Get_View_Matrix(&view_matrix);
		const Matrix4x4 clip = projection * Matrix4x4(view_matrix);
		const float half_texel = 0.5f / (float)Size;
		k.Uvz[0] = clip[0] * 0.5f + Vector4(0, 0, 0, 0.5f + half_texel);
		k.Uvz[1] = clip[1] * -0.5f + Vector4(0, 0, 0, 0.5f + half_texel);
		k.Uvz[2] = clip[2];
		k.Uvz[3] = Vector4(0, 0, 0, 1);
	}
	InCasterPass = false;

	DX9Wrapper::Set_DX9_Render_State(D3DRS_COLORWRITEENABLE, 0x0000000f);
	if (slope_bias)
		DX9Wrapper::Set_DX9_Render_State(D3DRS_SLOPESCALEDEPTHBIAS, 0);
	WW3D::End_Render(false);
	DX9Wrapper::Set_Render_Target((IDirect3DSurface9 *)nullptr);
	view->Apply();

	Set_Constants(device, &cascades[1].Uvz, cascades[1].Texel, &cascades[0].Uvz, cascades[0].Texel, far_plane - near_plane);
	Bind(device, true);
	IsActive = true;
	This.Drawn = true;
	This.Box = 2.0f * (cascades[1].HalfX > cascades[1].HalfY ? cascades[1].HalfX : cascades[1].HalfY);
	This.WhyNot = nullptr;
}
