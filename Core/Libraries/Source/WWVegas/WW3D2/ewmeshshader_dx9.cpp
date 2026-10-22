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

#include "ewmeshshader_dx9.h"
#include "ewshaders_dx9.h"
#include "ewgraphicsoptions_dx9.h"
#include "ew_constants.h"
#include "dx9wrapper.h"
#include "dx9caps.h"
#include "WWDebug/wwdebug.h"

#include <math.h>
#include <string.h>

// All state is read from the device, not from the wrapper's caches: the game sets some
// transforms and texture stage states on the device directly, and the device is never created
// pure (dx9wrapper.cpp), so the Get calls return the state the draw will use.

namespace
{
	enum Reason
	{
		REASON_NO_SM3,
		REASON_NO_SHADER,
		REASON_FVF,
		REASON_VERTEX_BLEND,
		REASON_CLIP_PLANES,
		REASON_FOG_MODE,
		REASON_STAGE_OP,
		REASON_STAGE_ARG,
		REASON_RESULT_ARG,
		REASON_STAGE2,
		REASON_TEXCOORD,
		REASON_TEXTURE_TRANSFORM,
		REASON_TEXTURE_TYPE,
		REASON_SPOT_LIGHT,
		REASON_COUNT
	};

	const char *const ReasonText[] =
	{
		"the card has no vs_3_0/ps_3_0",
		"the mesh shaders failed to load",
		"vertex format (FVF)",
		"vertex blending",
		"user clip planes",
		"fog mode other than linear",
		"texture stage operation (stage << 16 | op)",
		"texture stage argument (stage << 16 | arg)",
		"texture stage result argument (stage << 16 | arg)",
		"texture stage 2 in use",
		"texture coordinate index (stage << 16 | index)",
		"texture transform flags (stage << 16 | flags)",
		"texture type (stage << 16 | type)",
		"spot light (light index)",
	};
	static_assert(sizeof(ReasonText) / sizeof(ReasonText[0]) == REASON_COUNT, "ReasonText must match Reason");

	bool Logged[REASON_COUNT];

	bool Fallback(Reason reason, unsigned value)
	{
		if (!Logged[reason])
		{
			Logged[reason] = true;
			WWDEBUG_SAY(("EwMeshShader: fixed-function fallback for %s, value 0x%x (logged once)", ReasonText[reason], value));
		}
		return false;
	}

	struct Float4
	{
		float x, y, z, w;
	};

	IDirect3DDevice9Ex *Device;
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

	DWORD Stage_State(DWORD stage, D3DTEXTURESTAGESTATETYPE state)
	{
		DWORD value = 0;
		Device->GetTextureStageState(stage, state, &value);
		return value;
	}

	Float4 Color(D3DCOLOR c)
	{
		Float4 f = { ((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f };
		return f;
	}

	Float4 Color(const D3DCOLORVALUE &c)
	{
		Float4 f = { c.r, c.g, c.b, c.a };
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

	// D3D multiplies row vectors (v * M), so output k is dot(v, column k).
	// Uploading the columns lets the shader use one dot per output.
	void Columns(Float4 *out, const D3DMATRIX &m, int count)
	{
		for (int k = 0; k < count; ++k)
		{
			Float4 c = { m.m[0][k], m.m[1][k], m.m[2][k], m.m[3][k] };
			out[k] = c;
		}
	}

	// Inverse of the upper-left 3x3. False if it is singular.
	bool Inverse3x3(float inv[3][3], const D3DMATRIX &m)
	{
		const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
		const float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
		const float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
		const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
		if (fabsf(det) < 1e-20f)
			return false;
		const float s = 1.0f / det;
		inv[0][0] = (e * i - f * h) * s; inv[0][1] = (c * h - b * i) * s; inv[0][2] = (b * f - c * e) * s;
		inv[1][0] = (f * g - d * i) * s; inv[1][1] = (a * i - c * g) * s; inv[1][2] = (c * d - a * f) * s;
		inv[2][0] = (d * h - e * g) * s; inv[2][1] = (b * g - a * h) * s; inv[2][2] = (a * e - b * d) * s;
		return true;
	}

	// Normals use the inverse transpose. With row vectors, output k of n * transpose(inverse(A))
	// is dot(n, row k of inverse(A)), so the rows of the inverse are uploaded.
	void Normal_Rows(Float4 *out, const float inv[3][3])
	{
		for (int k = 0; k < 3; ++k)
		{
			Float4 r = { inv[k][0], inv[k][1], inv[k][2], 0.0f };
			out[k] = r;
		}
	}

	bool Supported_Op(DWORD op)
	{
		return (op >= D3DTOP_DISABLE && op <= D3DTOP_BLENDCURRENTALPHA)
			|| (op >= D3DTOP_MODULATEALPHA_ADDCOLOR && op <= D3DTOP_MODULATEINVCOLOR_ADDALPHA)
			|| (op >= D3DTOP_DOTPRODUCT3 && op <= D3DTOP_LERP);
	}

	bool Uses_Arg1(DWORD op) { return op != D3DTOP_DISABLE && op != D3DTOP_SELECTARG2; }
	bool Uses_Arg2(DWORD op) { return op != D3DTOP_DISABLE && op != D3DTOP_SELECTARG1; }
	bool Uses_Arg0(DWORD op) { return op == D3DTOP_MULTIPLYADD || op == D3DTOP_LERP; }

	// DIFFUSE, CURRENT, TEXTURE, TFACTOR and SPECULAR, with COMPLEMENT and ALPHAREPLICATE.
	bool Supported_Arg(DWORD arg)
	{
		return (arg & D3DTA_SELECTMASK) <= D3DTA_SPECULAR
			&& (arg & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE)) == 0;
	}

	float Arg_Select(DWORD arg) { return float(arg & D3DTA_SELECTMASK); }
	float Arg_Modifier(DWORD arg)
	{
		return ((arg & D3DTA_COMPLEMENT) ? 1.0f : 0.0f) + ((arg & D3DTA_ALPHAREPLICATE) ? 2.0f : 0.0f);
	}

	struct StageOp
	{
		DWORD op, arg1, arg2, arg0;

		bool Reads_Texture() const
		{
			return (Uses_Arg1(op) && (arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
				|| (Uses_Arg2(op) && (arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
				|| (Uses_Arg0(op) && (arg0 & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
				|| op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM;
		}
	};

	bool Check_Op(DWORD stage, const StageOp &s)
	{
		if (!Supported_Op(s.op))
			return Fallback(REASON_STAGE_OP, (stage << 16) | s.op);
		if (Uses_Arg1(s.op) && !Supported_Arg(s.arg1))
			return Fallback(REASON_STAGE_ARG, (stage << 16) | s.arg1);
		if (Uses_Arg2(s.op) && !Supported_Arg(s.arg2))
			return Fallback(REASON_STAGE_ARG, (stage << 16) | s.arg2);
		if (Uses_Arg0(s.op) && !Supported_Arg(s.arg0))
			return Fallback(REASON_STAGE_ARG, (stage << 16) | s.arg0);
		return true;
	}

	// Reads one enabled texture stage. ps gets the stage's 4 pixel shader registers, texgen and
	// texmatrix the vertex shader's texture coordinate setup.
	bool Gather_Stage(DWORD stage, int texcoord_sets, Float4 *ps, Float4 &texgen, Float4 *texmatrix, float &has_texture)
	{
		StageOp colour = { Stage_State(stage, D3DTSS_COLOROP), Stage_State(stage, D3DTSS_COLORARG1),
			Stage_State(stage, D3DTSS_COLORARG2), Stage_State(stage, D3DTSS_COLORARG0) };
		StageOp alpha = { Stage_State(stage, D3DTSS_ALPHAOP), Stage_State(stage, D3DTSS_ALPHAARG1),
			Stage_State(stage, D3DTSS_ALPHAARG2), Stage_State(stage, D3DTSS_ALPHAARG0) };

		if (!Check_Op(stage, colour) || !Check_Op(stage, alpha))
			return false;

		DWORD result = Stage_State(stage, D3DTSS_RESULTARG);
		if (result != D3DTA_CURRENT)
			return Fallback(REASON_RESULT_ARG, (stage << 16) | result);

		Float4 c0 = { float(colour.op), Arg_Select(colour.arg1), Arg_Select(colour.arg2), Arg_Select(colour.arg0) };
		Float4 c1 = { Arg_Modifier(colour.arg1), Arg_Modifier(colour.arg2), Arg_Modifier(colour.arg0), 0.0f };
		Float4 a0 = { float(alpha.op), Arg_Select(alpha.arg1), Arg_Select(alpha.arg2), Arg_Select(alpha.arg0) };
		Float4 a1 = { Arg_Modifier(alpha.arg1), Arg_Modifier(alpha.arg2), Arg_Modifier(alpha.arg0), 0.0f };
		ps[0] = c0;
		ps[1] = c1;
		ps[2] = a0;
		ps[3] = a1;

		if (!colour.Reads_Texture() && !alpha.Reads_Texture())
			return true;

		// Fixed-function reads a stage without a texture as opaque white, as the shader does.
		IDirect3DBaseTexture9 *texture = nullptr;
		Device->GetTexture(stage, &texture);
		if (texture == nullptr)
			return true;
		D3DRESOURCETYPE type = texture->GetType();
		texture->Release();
		if (type != D3DRTYPE_TEXTURE)
			return Fallback(REASON_TEXTURE_TYPE, (stage << 16) | type);
		has_texture = 1.0f;

		DWORD tci = Stage_State(stage, D3DTSS_TEXCOORDINDEX);
		DWORD index = tci & 0xffff;
		float source;
		switch (tci & 0xffff0000)
		{
		case D3DTSS_TCI_PASSTHRU:
			if (int(index) >= texcoord_sets || index > 1)
				return Fallback(REASON_TEXCOORD, (stage << 16) | index);
			source = 0.0f;
			break;
		case D3DTSS_TCI_CAMERASPACENORMAL:			source = 1.0f; index = 0; break;
		case D3DTSS_TCI_CAMERASPACEPOSITION:		source = 2.0f; index = 0; break;
		case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:	source = 3.0f; index = 0; break;
		default:
			return Fallback(REASON_TEXCOORD, (stage << 16) | (tci >> 16));
		}

		DWORD flags = Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS);
		DWORD count = flags & ~DWORD(D3DTTFF_PROJECTED);
		bool projected = (flags & D3DTTFF_PROJECTED) != 0;
		if (count == D3DTTFF_COUNT1 || count > D3DTTFF_COUNT4 || (projected && count == D3DTTFF_DISABLE))
			return Fallback(REASON_TEXTURE_TRANSFORM, (stage << 16) | flags);

		Float4 tg = { source, float(index), float(count), projected ? 1.0f : 0.0f };
		texgen = tg;
		if (count != D3DTTFF_DISABLE)
		{
			D3DMATRIX m;
			Device->GetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + stage), &m);
			Columns(texmatrix, m, 4);
		}
		return true;
	}

	// Material colour source: 0 material, 1 vertex diffuse, 2 vertex specular.
	// Fixed-function uses the material colour when the vertex lacks the chosen colour.
	float Material_Source(D3DRENDERSTATETYPE state, bool color_vertex, DWORD fvf)
	{
		if (!color_vertex)
			return 0.0f;
		DWORD source = Render_State(state);
		if (source == D3DMCS_COLOR1 && (fvf & D3DFVF_DIFFUSE))
			return 1.0f;
		if (source == D3DMCS_COLOR2 && (fvf & D3DFVF_SPECULAR))
			return 2.0f;
		return 0.0f;
	}
}

bool EwMeshShader::Begin()
{
	if (!EwGraphics().PerPixelLighting)
		return false;

	Device = DX8Wrapper::_Get_D3D_Device9();
	if (Device == nullptr)
		return false;

	// Unlit draws and draws with their own shaders are not ours.
	if (Render_State(D3DRS_LIGHTING) == FALSE)
		return false;
	IDirect3DVertexShader9 *caller_vs = nullptr;
	Device->GetVertexShader(&caller_vs);
	if (caller_vs)
	{
		caller_vs->Release();
		return false;
	}
	IDirect3DPixelShader9 *caller_ps = nullptr;
	Device->GetPixelShader(&caller_ps);
	if (caller_ps)
	{
		caller_ps->Release();
		return false;
	}

	const DX8Caps *caps = DX8Wrapper::Get_Current_Caps();
	if (caps->Get_Vertex_Shader_Major_Version() < 3 || caps->Get_Pixel_Shader_Major_Version() < 3)
		return Fallback(REASON_NO_SM3, (caps->Get_Vertex_Shader_Major_Version() << 8) | caps->Get_Pixel_Shader_Major_Version());

	DWORD fvf = 0;
	Device->GetFVF(&fvf);
	const DWORD allowed = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEXCOUNT_MASK;
	const int texcoord_sets = int((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
	if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !(fvf & D3DFVF_NORMAL) || (fvf & ~allowed) || texcoord_sets > 2)
		return Fallback(REASON_FVF, fvf);

	IDirect3DVertexShader9 *vertex_shader = EwShaders::Peek_Vertex(
		EwShaders::Mesh_Vertex_Id((fvf & D3DFVF_DIFFUSE) != 0, (fvf & D3DFVF_SPECULAR) != 0, texcoord_sets));
	IDirect3DPixelShader9 *pixel_shader = EwShaders::Peek_Pixel(EwShaders::PS_MESH);
	if (vertex_shader == nullptr || pixel_shader == nullptr)
		return Fallback(REASON_NO_SHADER, 0);

	DWORD blend = Render_State(D3DRS_VERTEXBLEND);
	if (blend != D3DVBF_DISABLE || Render_State(D3DRS_INDEXEDVERTEXBLENDENABLE))
		return Fallback(REASON_VERTEX_BLEND, blend);

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

	// Texture stages. A disabled stage 0 leaves the lit colour, and stage 1 only runs after stage 0.
	Float4 stage_constants[8];
	Float4 texgen[2];
	Float4 texmatrix[8];
	float has_texture[2] = { 0.0f, 0.0f };
	memset(stage_constants, 0, sizeof(stage_constants));
	memset(texgen, 0, sizeof(texgen));
	memset(texmatrix, 0, sizeof(texmatrix));
	stage_constants[0].x = float(D3DTOP_DISABLE);
	stage_constants[4].x = float(D3DTOP_DISABLE);
	if (Stage_State(0, D3DTSS_COLOROP) != D3DTOP_DISABLE)
	{
		if (!Gather_Stage(0, texcoord_sets, &stage_constants[0], texgen[0], &texmatrix[0], has_texture[0]))
			return false;
		if (Stage_State(1, D3DTSS_COLOROP) != D3DTOP_DISABLE)
		{
			if (!Gather_Stage(1, texcoord_sets, &stage_constants[4], texgen[1], &texmatrix[4], has_texture[1]))
				return false;
			DWORD op2 = Stage_State(2, D3DTSS_COLOROP);
			if (op2 != D3DTOP_DISABLE)
				return Fallback(REASON_STAGE2, op2);
		}
	}

	// Lights: the wrapper uses indices 0..3, the same count the shader has.
	Float4 lights[EW_MESH_MAX_LIGHTS * 5];
	memset(lights, 0, sizeof(lights));
	for (DWORD i = 0; i < EW_MESH_MAX_LIGHTS; ++i)
	{
		BOOL enabled = FALSE;
		if (FAILED(Device->GetLightEnable(i, &enabled)) || !enabled)
			continue;
		D3DLIGHT9 light;
		if (FAILED(Device->GetLight(i, &light)))
			continue;
		if (light.Type != D3DLIGHT_POINT && light.Type != D3DLIGHT_DIRECTIONAL)
			return Fallback(REASON_SPOT_LIGHT, i);

		Float4 *l = &lights[i * 5];
		l[0] = Color(light.Diffuse);
		l[0].w = (light.Type == D3DLIGHT_POINT) ? 1.0f : 3.0f;
		l[1] = Color(light.Specular);
		l[2] = Color(light.Ambient);
		if (light.Type == D3DLIGHT_DIRECTIONAL)
		{
			// The shader wants the direction towards the light.
			float x = light.Direction.x, y = light.Direction.y, z = light.Direction.z;
			float length = sqrtf(x * x + y * y + z * z);
			float s = (length > 1e-12f) ? -1.0f / length : 0.0f;
			Float4 v = { x * s, y * s, z * s, 0.0f };
			l[3] = v;
		}
		else
		{
			Float4 v = { light.Position.x, light.Position.y, light.Position.z, 1.0f };
			Float4 a = { light.Attenuation0, light.Attenuation1, light.Attenuation2, light.Range };
			l[3] = v;
			l[4] = a;
		}
	}

	D3DMATRIX world, view, projection, world_view, world_view_projection;
	Device->GetTransform(D3DTS_WORLD, &world);
	Device->GetTransform(D3DTS_VIEW, &view);
	Device->GetTransform(D3DTS_PROJECTION, &projection);
	Multiply(world_view, world, view);
	Multiply(world_view_projection, world_view, projection);

	// A singular matrix scales the mesh to nothing; leave it to fixed-function.
	float world_inverse[3][3], world_view_inverse[3][3], view_inverse[3][3];
	if (!Inverse3x3(world_inverse, world) || !Inverse3x3(world_view_inverse, world_view) || !Inverse3x3(view_inverse, view))
		return false;

	// The camera sits where camera * view = 0: camera = -translation * inverse(rotation).
	Float4 camera;
	camera.x = -(view._41 * view_inverse[0][0] + view._42 * view_inverse[1][0] + view._43 * view_inverse[2][0]);
	camera.y = -(view._41 * view_inverse[0][1] + view._42 * view_inverse[1][1] + view._43 * view_inverse[2][1]);
	camera.z = -(view._41 * view_inverse[0][2] + view._42 * view_inverse[1][2] + view._43 * view_inverse[2][2]);
	camera.w = (Render_State(D3DRS_LOCALVIEWER) != FALSE) ? 1.0f : 0.0f;

	// Without a local viewer, D3D uses the camera-space direction (0, 0, 1).
	Float4 view_dir = { view_inverse[2][0], view_inverse[2][1], view_inverse[2][2], 0.0f };
	float view_dir_length = sqrtf(view_dir.x * view_dir.x + view_dir.y * view_dir.y + view_dir.z * view_dir.z);
	if (view_dir_length > 1e-12f)
	{
		view_dir.x /= view_dir_length;
		view_dir.y /= view_dir_length;
		view_dir.z /= view_dir_length;
	}

	D3DMATERIAL9 material;
	Device->GetMaterial(&material);
	const bool color_vertex = Render_State(D3DRS_COLORVERTEX) != FALSE;

	// Vertex shader constants.
	Float4 vs[EW_MESH_VS_COUNT];
	memset(vs, 0, sizeof(vs));
	Columns(&vs[EW_MESH_VS_WVP], world_view_projection, 4);
	Columns(&vs[EW_MESH_VS_WORLD], world, 3);
	Normal_Rows(&vs[EW_MESH_VS_WORLD_NORMAL], world_inverse);
	Columns(&vs[EW_MESH_VS_WORLDVIEW], world_view, 3);
	Normal_Rows(&vs[EW_MESH_VS_WORLDVIEW_NORMAL], world_view_inverse);
	vs[EW_MESH_VS_TEXGEN + 0] = texgen[0];
	vs[EW_MESH_VS_TEXGEN + 1] = texgen[1];
	memcpy(&vs[EW_MESH_VS_TEXMATRIX], texmatrix, sizeof(texmatrix));
	vs[EW_MESH_VS_OPTIONS].x = range_fog;

	// Pixel shader constants.
	Float4 ps[EW_MESH_PS_COUNT];
	memset(ps, 0, sizeof(ps));
	memcpy(&ps[EW_MESH_PS_STAGE], stage_constants, sizeof(stage_constants));
	ps[EW_MESH_PS_TFACTOR] = Color(D3DCOLOR(Render_State(D3DRS_TEXTUREFACTOR)));
	ps[EW_MESH_PS_AMBIENT] = Color(D3DCOLOR(Render_State(D3DRS_AMBIENT)));
	ps[EW_MESH_PS_MATERIAL + 0] = Color(material.Diffuse);
	ps[EW_MESH_PS_MATERIAL + 1] = Color(material.Ambient);
	ps[EW_MESH_PS_MATERIAL + 2] = Color(material.Specular);
	ps[EW_MESH_PS_MATERIAL + 3] = Color(material.Emissive);
	ps[EW_MESH_PS_MATSOURCE].x = Material_Source(D3DRS_DIFFUSEMATERIALSOURCE, color_vertex, fvf);
	ps[EW_MESH_PS_MATSOURCE].y = Material_Source(D3DRS_AMBIENTMATERIALSOURCE, color_vertex, fvf);
	ps[EW_MESH_PS_MATSOURCE].z = Material_Source(D3DRS_SPECULARMATERIALSOURCE, color_vertex, fvf);
	ps[EW_MESH_PS_MATSOURCE].w = Material_Source(D3DRS_EMISSIVEMATERIALSOURCE, color_vertex, fvf);
	ps[EW_MESH_PS_OPTIONS].x = material.Power;
	ps[EW_MESH_PS_OPTIONS].y = (Render_State(D3DRS_SPECULARENABLE) != FALSE) ? 1.0f : 0.0f;
	ps[EW_MESH_PS_OPTIONS].z = has_texture[0];
	ps[EW_MESH_PS_OPTIONS].w = has_texture[1];
	ps[EW_MESH_PS_FOG] = fog;
	ps[EW_MESH_PS_FOGCOLOR] = Color(D3DCOLOR(Render_State(D3DRS_FOGCOLOR)));
	ps[EW_MESH_PS_CAMERA] = camera;
	ps[EW_MESH_PS_VIEWDIR] = view_dir;
	memcpy(&ps[EW_MESH_PS_LIGHTS], lights, sizeof(lights));

	Device->SetVertexShaderConstantF(0, &vs[0].x, EW_MESH_VS_COUNT);
	Device->SetPixelShaderConstantF(0, &ps[0].x, EW_MESH_PS_COUNT);
	Device->SetVertexShader(vertex_shader);
	Device->SetPixelShader(pixel_shader);

	// The pixel shader fogs itself. The wrapper's cache keeps TRUE, and End() puts it back.
	RestoreFog = fog_enabled;
	if (fog_enabled)
		Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	return true;
}

void EwMeshShader::End()
{
	Device->SetVertexShader(nullptr);
	Device->SetPixelShader(nullptr);
	if (RestoreFog)
	{
		Device->SetRenderState(D3DRS_FOGENABLE, TRUE);
		RestoreFog = false;
	}
}

static_assert(EW_MESH_VS_WVP == 0 && EW_MESH_VS_OPTIONS + 1 == EW_MESH_VS_COUNT, "mesh vertex constants are uploaded as one block from c0");
static_assert(EW_MESH_PS_STAGE == 0 && EW_MESH_PS_LIGHTS + EW_MESH_MAX_LIGHTS * 5 == EW_MESH_PS_COUNT, "mesh pixel constants are uploaded as one block from c0");
