// EastWind DX9: lit-mesh shader (M3.1a, EwPerPixelLighting).
//
// Draws W3D meshes that fixed-function would light per vertex, with the same result except that
// the D3D9 lighting equation runs per pixel. EwMeshShader (ewmeshshader_dx9.cpp) binds it at draw
// time and fills the constants from the device and wrapper state:
//   - the D3D9 lights, material, material colour sources and global ambient;
//   - texture coordinate generation and texture matrices, expanded as D3D does:
//     passthru uv -> (u, v, 1, 0), generated -> (x, y, z, 1);
//   - the texture stage cascade of stages 0 and 1 (ops and arguments as numbers);
//   - specular add and linear fog. With ps_3_0 bound, the hardware no longer applies fog.
// Alpha test and blending stay hardware states.
//
// One vertex shader per vertex layout, so a shader never reads an element the FVF lacks:
// MeshVS_<N|ND|NS|NDS>_T<0|1|2> (D = diffuse colour, S = specular colour, T = texcoord sets).

#include "ew_constants.h"

// D3DTEXTUREOP values
#define OP_DISABLE                    1
#define OP_SELECTARG1                 2
#define OP_SELECTARG2                 3
#define OP_MODULATE                   4
#define OP_MODULATE2X                 5
#define OP_MODULATE4X                 6
#define OP_ADD                        7
#define OP_ADDSIGNED                  8
#define OP_ADDSIGNED2X                9
#define OP_SUBTRACT                   10
#define OP_ADDSMOOTH                  11
#define OP_BLENDDIFFUSEALPHA          12
#define OP_BLENDTEXTUREALPHA          13
#define OP_BLENDFACTORALPHA           14
#define OP_BLENDTEXTUREALPHAPM        15
#define OP_BLENDCURRENTALPHA          16
#define OP_MODULATEALPHA_ADDCOLOR     18
#define OP_MODULATECOLOR_ADDALPHA     19
#define OP_MODULATEINVALPHA_ADDCOLOR  20
#define OP_MODULATEINVCOLOR_ADDALPHA  21
#define OP_DOTPRODUCT3                24
#define OP_MULTIPLYADD                25
#define OP_LERP                       26

// ---------------------------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------------------------

float4 WorldViewProj[4]       : EW_REG(EW_MESH_VS_WVP);
float4 World[3]               : EW_REG(EW_MESH_VS_WORLD);
float4 WorldNormal[3]         : EW_REG(EW_MESH_VS_WORLD_NORMAL);
float4 WorldView[3]           : EW_REG(EW_MESH_VS_WORLDVIEW);
float4 WorldViewNormal[3]     : EW_REG(EW_MESH_VS_WORLDVIEW_NORMAL);
float4 TexGen[2]              : EW_REG(EW_MESH_VS_TEXGEN);
float4 TexMatrix[8]           : EW_REG(EW_MESH_VS_TEXMATRIX);
float4 VsOptions              : EW_REG(EW_MESH_VS_OPTIONS);

struct VsIn
{
	float4 position : POSITION0;
	float3 normal   : NORMAL0;
	float4 diffuse  : COLOR0;
	float4 specular : COLOR1;
	float2 uv0      : TEXCOORD0;
	float2 uv1      : TEXCOORD1;
};

struct VsOut
{
	float4 position    : POSITION;
	float4 worldPosFog : TEXCOORD0;	// xyz world position, w fog distance
	float3 worldNormal : TEXCOORD1;
	float4 uv0         : TEXCOORD2;	// xy texcoord, w divisor (1 unless projected)
	float4 uv1         : TEXCOORD3;
	float4 diffuse     : COLOR0;	// vertex colours, the material colour sources
	float4 specular    : COLOR1;
};

// TexGen[s]: x source (0 passthru, 1 camera-space normal, 2 camera-space position,
// 3 camera-space reflection vector), y uv set, z transform count (0 off, 2..4), w projected.
float4 StageTexCoord(int s, float2 uv, float3 csPos, float3 csNormal)
{
	float4 tg = TexGen[s];

	float4 v = float4(uv, 1.0, 0.0);
	v = (tg.x > 0.5) ? float4(csNormal, 1.0) : v;
	v = (tg.x > 1.5) ? float4(csPos, 1.0) : v;
	v = (tg.x > 2.5) ? float4(reflect(normalize(csPos), csNormal), 1.0) : v;

	float4 t = float4(dot(v, TexMatrix[s * 4 + 0]), dot(v, TexMatrix[s * 4 + 1]),
		dot(v, TexMatrix[s * 4 + 2]), dot(v, TexMatrix[s * 4 + 3]));
	v = (tg.z > 0.5) ? t : v;

	// Projected: everything is divided by the last of the counted components.
	float q = v.y;
	q = (tg.z > 2.5) ? v.z : q;
	q = (tg.z > 3.5) ? v.w : q;
	q = (tg.w > 0.5) ? q : 1.0;
	return float4(v.xy, 0.0, q);
}

VsOut MeshVS(VsIn i, bool hasDiffuse, bool hasSpecular, int texSets)
{
	VsOut o;
	float4 p = float4(i.position.xyz, 1.0);

	o.position = float4(dot(p, WorldViewProj[0]), dot(p, WorldViewProj[1]),
		dot(p, WorldViewProj[2]), dot(p, WorldViewProj[3]));

	float3 worldPos = float3(dot(p, World[0]), dot(p, World[1]), dot(p, World[2]));
	o.worldNormal = float3(dot(i.normal, WorldNormal[0].xyz), dot(i.normal, WorldNormal[1].xyz),
		dot(i.normal, WorldNormal[2].xyz));

	float3 csPos = float3(dot(p, WorldView[0]), dot(p, WorldView[1]), dot(p, WorldView[2]));
	float3 csNormal = normalize(float3(dot(i.normal, WorldViewNormal[0].xyz),
		dot(i.normal, WorldViewNormal[1].xyz), dot(i.normal, WorldViewNormal[2].xyz)));

	// D3D vertex fog: camera-space depth, or distance with D3DRS_RANGEFOGENABLE.
	// W3D's camera looks down -z, so the depth is taken as a magnitude.
	float fogDistance = (VsOptions.x > 0.5) ? length(csPos) : abs(csPos.z);
	o.worldPosFog = float4(worldPos, fogDistance);

	// Fixed-function defaults for a missing colour: diffuse white, specular black.
	o.diffuse = hasDiffuse ? i.diffuse : float4(1.0, 1.0, 1.0, 1.0);
	o.specular = hasSpecular ? i.specular : float4(0.0, 0.0, 0.0, 0.0);

	float2 set0 = (texSets > 0) ? i.uv0 : float2(0.0, 0.0);
	float2 set1 = (texSets > 1) ? i.uv1 : set0;
	o.uv0 = StageTexCoord(0, (TexGen[0].y > 0.5) ? set1 : set0, csPos, csNormal);
	o.uv1 = StageTexCoord(1, (TexGen[1].y > 0.5) ? set1 : set0, csPos, csNormal);
	return o;
}

VsOut MeshVS_N_T0(VsIn i)   { return MeshVS(i, false, false, 0); }
VsOut MeshVS_ND_T0(VsIn i)  { return MeshVS(i, true,  false, 0); }
VsOut MeshVS_NS_T0(VsIn i)  { return MeshVS(i, false, true,  0); }
VsOut MeshVS_NDS_T0(VsIn i) { return MeshVS(i, true,  true,  0); }
VsOut MeshVS_N_T1(VsIn i)   { return MeshVS(i, false, false, 1); }
VsOut MeshVS_ND_T1(VsIn i)  { return MeshVS(i, true,  false, 1); }
VsOut MeshVS_NS_T1(VsIn i)  { return MeshVS(i, false, true,  1); }
VsOut MeshVS_NDS_T1(VsIn i) { return MeshVS(i, true,  true,  1); }
VsOut MeshVS_N_T2(VsIn i)   { return MeshVS(i, false, false, 2); }
VsOut MeshVS_ND_T2(VsIn i)  { return MeshVS(i, true,  false, 2); }
VsOut MeshVS_NS_T2(VsIn i)  { return MeshVS(i, false, true,  2); }
VsOut MeshVS_NDS_T2(VsIn i) { return MeshVS(i, true,  true,  2); }

// ---------------------------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------------------------

// Per stage, 4 registers: colour (op, arg1, arg2, arg0), colour modifiers (arg1, arg2, arg0, -),
// alpha (op, arg1, arg2, arg0), alpha modifiers. Arguments use the D3DTA_ select values;
// a modifier is complement (1) + alpha replicate (2).
float4 Stage[8]               : EW_REG(EW_MESH_PS_STAGE);
float4 TFactor                : EW_REG(EW_MESH_PS_TFACTOR);
float4 GlobalAmbient          : EW_REG(EW_MESH_PS_AMBIENT);
float4 Material[4]            : EW_REG(EW_MESH_PS_MATERIAL);	// diffuse, ambient, specular, emissive
float4 MaterialSource         : EW_REG(EW_MESH_PS_MATSOURCE);	// diffuse, ambient, specular, emissive: 0 material, 1 vertex diffuse, 2 vertex specular
float4 PsOptions              : EW_REG(EW_MESH_PS_OPTIONS);	// x specular power, y specular enable, z/w stage 0/1 has a texture
float4 Fog                    : EW_REG(EW_MESH_PS_FOG);		// x enable, y end, z 1 / (end - start)
float4 FogColor               : EW_REG(EW_MESH_PS_FOGCOLOR);
float4 Camera                 : EW_REG(EW_MESH_PS_CAMERA);		// xyz camera world position, w local viewer
float4 ViewDir                : EW_REG(EW_MESH_PS_VIEWDIR);		// xyz direction to the viewer without local viewer
float4 Lights[EW_MESH_MAX_LIGHTS * 5] : EW_REG(EW_MESH_PS_LIGHTS);
// Per light: diffuse (rgb, w type: 0 off, 1 point, 3 directional), specular, ambient,
// vector (direction to the light, or position), attenuation (a0, a1, a2, range).

sampler2D Stage0Texture : register(s0);
sampler2D Stage1Texture : register(s1);

struct PsIn
{
	float4 worldPosFog : TEXCOORD0;
	float3 worldNormal : TEXCOORD1;
	float4 uv0         : TEXCOORD2;
	float4 uv1         : TEXCOORD3;
	float4 diffuse     : COLOR0;
	float4 specular    : COLOR1;
};

float4 SourceColor(float source, float4 material, float4 vertexDiffuse, float4 vertexSpecular)
{
	float4 c = material;
	c = (source > 0.5) ? vertexDiffuse : c;
	c = (source > 1.5) ? vertexSpecular : c;
	return c;
}

void AddLight(int l, float3 P, float3 N, float3 V, float power,
	inout float3 ambient, inout float3 diffuse, inout float3 specular)
{
	float4 lightDiffuse = Lights[l * 5 + 0];
	float4 lightVector = Lights[l * 5 + 3];
	float4 lightAtten = Lights[l * 5 + 4];

	[branch] if (lightDiffuse.w > 0.5)
	{
		float3 L = lightVector.xyz;
		float atten = 1.0;
		if (lightDiffuse.w < 2.0)
		{
			float3 toLight = lightVector.xyz - P;
			float dist = length(toLight);
			L = toLight / max(dist, 1e-6);
			float d = lightAtten.x + lightAtten.y * dist + lightAtten.z * dist * dist;
			atten = (dist <= lightAtten.w) ? 1.0 / max(d, 1e-6) : 0.0;
		}

		ambient += atten * Lights[l * 5 + 2].rgb;

		float nl = dot(N, L);
		if (nl > 0.0)
		{
			diffuse += atten * nl * lightDiffuse.rgb;
			float nh = saturate(dot(N, normalize(L + V)));
			float s = (power > 0.0) ? pow(nh, power) : 1.0;
			specular += atten * s * Lights[l * 5 + 1].rgb;
		}
	}
}

float4 StageArg(float select, float modifier, float4 diffuse, float4 current, float4 tex, float4 specular)
{
	// D3DTA_DIFFUSE 0, CURRENT 1, TEXTURE 2, TFACTOR 3, SPECULAR 4
	float4 v = diffuse;
	v = (select > 0.5) ? current : v;
	v = (select > 1.5) ? tex : v;
	v = (select > 2.5) ? TFactor : v;
	v = (select > 3.5) ? specular : v;
	v = (modifier > 1.5) ? v.aaaa : v;
	float complement = (modifier > 1.5) ? modifier - 2.0 : modifier;
	return (complement > 0.5) ? 1.0 - v : v;
}

// One texture stage. Ops are per channel: rgb takes the colour op and arguments, a the alpha ones.
float4 RunStage(int s, float4 diffuse, float4 current, float4 tex, float4 specular)
{
	float4 colour = Stage[s * 4 + 0];
	float4 colourMod = Stage[s * 4 + 1];
	float4 alpha = Stage[s * 4 + 2];
	float4 alphaMod = Stage[s * 4 + 3];

	float4 c1 = StageArg(colour.y, colourMod.x, diffuse, current, tex, specular);
	float4 c2 = StageArg(colour.z, colourMod.y, diffuse, current, tex, specular);
	float4 c0 = StageArg(colour.w, colourMod.z, diffuse, current, tex, specular);
	float4 a1 = StageArg(alpha.y, alphaMod.x, diffuse, current, tex, specular);
	float4 a2 = StageArg(alpha.z, alphaMod.y, diffuse, current, tex, specular);
	float4 a0 = StageArg(alpha.w, alphaMod.z, diffuse, current, tex, specular);

	float4 op = float4(colour.xxx, alpha.x);
	float4 x1 = float4(c1.rgb, a1.a);
	float4 x2 = float4(c2.rgb, a2.a);
	float4 x0 = float4(c0.rgb, a0.a);

	float4 r = x1;
	r = (op == OP_SELECTARG2) ? x2 : r;
	r = (op == OP_MODULATE) ? x1 * x2 : r;
	r = (op == OP_MODULATE2X) ? 2.0 * x1 * x2 : r;
	r = (op == OP_MODULATE4X) ? 4.0 * x1 * x2 : r;
	r = (op == OP_ADD) ? x1 + x2 : r;
	r = (op == OP_ADDSIGNED) ? x1 + x2 - 0.5 : r;
	r = (op == OP_ADDSIGNED2X) ? 2.0 * (x1 + x2 - 0.5) : r;
	r = (op == OP_SUBTRACT) ? x1 - x2 : r;
	r = (op == OP_ADDSMOOTH) ? x1 + x2 - x1 * x2 : r;
	r = (op == OP_BLENDDIFFUSEALPHA) ? lerp(x2, x1, diffuse.a) : r;
	r = (op == OP_BLENDTEXTUREALPHA) ? lerp(x2, x1, tex.a) : r;
	r = (op == OP_BLENDFACTORALPHA) ? lerp(x2, x1, TFactor.a) : r;
	r = (op == OP_BLENDTEXTUREALPHAPM) ? x1 + x2 * (1.0 - tex.a) : r;
	r = (op == OP_BLENDCURRENTALPHA) ? lerp(x2, x1, current.a) : r;
	r = (op == OP_MODULATEALPHA_ADDCOLOR) ? x1 + c1.a * x2 : r;
	r = (op == OP_MODULATECOLOR_ADDALPHA) ? x1 * x2 + c1.a : r;
	r = (op == OP_MODULATEINVALPHA_ADDCOLOR) ? (1.0 - c1.a) * x2 + x1 : r;
	r = (op == OP_MODULATEINVCOLOR_ADDALPHA) ? (1.0 - x1) * x2 + c1.a : r;
	r = (op == OP_MULTIPLYADD) ? x0 + x1 * x2 : r;
	r = (op == OP_LERP) ? x0 * x1 + (1.0 - x0) * x2 : r;
	r = (op == OP_DISABLE) ? current : r;	// only the alpha op can be disabled here

	// DOTPRODUCT3 fills all four channels and overrides the alpha op.
	float dot3 = saturate(4.0 * dot(c1.rgb - 0.5, c2.rgb - 0.5));
	r = (colour.x == OP_DOTPRODUCT3) ? dot3.xxxx : r;

	return saturate(r);
}

float4 MeshPS(PsIn i) : COLOR0
{
	float3 P = i.worldPosFog.xyz;
	float3 N = normalize(i.worldNormal);
	float3 V = (Camera.w > 0.5) ? normalize(Camera.xyz - P) : ViewDir.xyz;
	float power = PsOptions.x;

	float4 matDiffuse = SourceColor(MaterialSource.x, Material[0], i.diffuse, i.specular);
	float4 matAmbient = SourceColor(MaterialSource.y, Material[1], i.diffuse, i.specular);
	float4 matSpecular = SourceColor(MaterialSource.z, Material[2], i.diffuse, i.specular);
	float4 matEmissive = SourceColor(MaterialSource.w, Material[3], i.diffuse, i.specular);

	float3 ambient = GlobalAmbient.rgb;
	float3 diffuse = 0.0;
	float3 specular = 0.0;
	AddLight(0, P, N, V, power, ambient, diffuse, specular);
	AddLight(1, P, N, V, power, ambient, diffuse, specular);
	AddLight(2, P, N, V, power, ambient, diffuse, specular);
	AddLight(3, P, N, V, power, ambient, diffuse, specular);

	float4 litDiffuse = float4(saturate(matAmbient.rgb * ambient + matDiffuse.rgb * diffuse + matEmissive.rgb), matDiffuse.a);
	float4 litSpecular = float4(saturate(matSpecular.rgb * specular) * PsOptions.y, 0.0);

	// A stage without a texture reads opaque white.
	float4 tex0 = (PsOptions.z > 0.5) ? tex2D(Stage0Texture, i.uv0.xy / i.uv0.w) : float4(1.0, 1.0, 1.0, 1.0);
	float4 tex1 = (PsOptions.w > 0.5) ? tex2D(Stage1Texture, i.uv1.xy / i.uv1.w) : float4(1.0, 1.0, 1.0, 1.0);

	// Stage 0 disabled leaves the lit diffuse colour; stage 1 runs only after an enabled stage 0.
	float4 current = litDiffuse;
	[branch] if (Stage[0].x != OP_DISABLE)
	{
		current = RunStage(0, litDiffuse, current, tex0, litSpecular);
		[branch] if (Stage[4].x != OP_DISABLE)
			current = RunStage(1, litDiffuse, current, tex1, litSpecular);
	}

	current.rgb = saturate(current.rgb + litSpecular.rgb);

	[branch] if (Fog.x > 0.5)
	{
		float f = saturate((Fog.y - i.worldPosFog.w) * Fog.z);
		current.rgb = lerp(FogColor.rgb, current.rgb, f);
	}
	return current;
}
