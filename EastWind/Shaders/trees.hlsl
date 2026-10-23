// EastWind DX9: tree and bush vertex shader, ported from Trees.nvv (vs.1.1).
// Sways trees by their wave entry, darkens pushed-aside trees and maps the shroud texture.
// The pixel stages stay fixed-function, as in DX8 (Trees.pso was never used).
// TreesLitVS/TreesLitPS at the end light trees per pixel instead (M3.1d).
//
// Vertex format DX8_FVF_XYZNDUV1, with the normal used for tree data:
//   normal.x = sway type (index into Wave), normal.y = colour scale, normal.z = base z of the tree.

#include "ew_constants.h"

float4 WorldViewProj[4]                 : EW_REG(EW_TREES_VS_WVP);
float4 Wave[EW_TREES_MAX_SWAY + 1]      : EW_REG(EW_TREES_VS_WAVE);
float4 ShroudOffset                     : EW_REG(EW_TREES_VS_SHROUD_OFS);
float4 ShroudScale                      : EW_REG(EW_TREES_VS_SHROUD_SCALE);

struct VsIn
{
	float4 position : POSITION0;
	float3 treeData : NORMAL0;
	float4 diffuse  : COLOR0;
	float4 uv       : TEXCOORD0;
};

struct VsOut
{
	float4 position : POSITION;
	float4 diffuse  : COLOR0;
	float4 uv0      : TEXCOORD0;
	float4 uv1      : TEXCOORD1;
};

VsOut TreesVS(VsIn i)
{
	VsOut o;

	// Height above the tree base scales the sway, so the base stays put.
	float height = i.position.z - i.treeData.z;
	float4 wave = Wave[(int)i.treeData.x];
	float4 swayed = float4(i.position.xyz + height * wave.xyz, 1.0);

	o.position = float4(dot(swayed, WorldViewProj[0]), dot(swayed, WorldViewProj[1]),
		dot(swayed, WorldViewProj[2]), dot(swayed, WorldViewProj[3]));
	o.diffuse = i.diffuse * float4(i.treeData.yyy, 1.0);
	o.uv0 = i.uv;
	o.uv1 = (i.position + ShroudOffset) * ShroudScale;
	return o;
}

// --- Per-pixel lit trees (M3.1d, EwPerPixelLighting); set by W3DTreeBuffer::drawTrees ---
//
// The CPU lights trees per vertex (W3DTreeBuffer::doLighting): the ambient of the first object
// light plus the material emissive plus each light's diffuse against the vertex normal, clamped
// to 0..1, times the mesh vertex colour. drawTrees then darkens pushed-aside trees (normal.y).
// TreesLitPS does the same per pixel. With EwPerPixelLighting the vertex buffer has the
// DX8_FVF_XYZNDUV1TG3 layout, and the three extra coordinates hold what doLighting needs:
//   TEXCOORD1 the vertex normal, turned with the tree (as doLighting gets it)
//   TEXCOORD2 the material emissive
//   TEXCOORD3 the mesh vertex colour (white without one)
// The diffuse colour is still the CPU light, so TreesVS draws the same buffer the classic way.
// The pixel side replaces the fixed-function stages: stage 0 the tree texture times the light,
// stage 1 the shroud modulating the colour. Trees draw with fog off (detailAlphaShader), so
// there is no fog here.

float4 TreeLight[EW_TREES_PS_LIGHT_COUNT] : EW_REG(EW_TREES_PS_LIGHT);	// [0] ambient, [1..3] direction to the light, [4..6] diffuse
float4 TreeOptions                         : EW_REG(EW_TREES_PS_OPTIONS);	// x shroud on, y light only (debug key)

sampler2D TreeTexture : register(s0);
sampler2D TreeShroud  : register(s1);

struct LitVsIn
{
	float4 position : POSITION0;
	float3 treeData : NORMAL0;
	float4 diffuse  : COLOR0;
	float2 uv       : TEXCOORD0;
	float3 normal   : TEXCOORD1;
	float3 emissive : TEXCOORD2;
	float3 colour   : TEXCOORD3;
};

struct LitVsOut
{
	float4 position : POSITION;
	float4 uv       : TEXCOORD0;	// xy tree texture, zw shroud
	float3 normal   : TEXCOORD1;
	float3 emissive : TEXCOORD2;
	float3 colour   : TEXCOORD3;	// vertex colour times the push-aside darkening
};

LitVsOut TreesLitVS(LitVsIn i)
{
	LitVsOut o;

	// As TreesVS.
	float height = i.position.z - i.treeData.z;
	float4 wave = Wave[(int)i.treeData.x];
	float4 swayed = float4(i.position.xyz + height * wave.xyz, 1.0);

	o.position = float4(dot(swayed, WorldViewProj[0]), dot(swayed, WorldViewProj[1]),
		dot(swayed, WorldViewProj[2]), dot(swayed, WorldViewProj[3]));
	float4 shroud = (float4(i.position.xyz, 1.0) + ShroudOffset) * ShroudScale;
	o.uv = float4(i.uv, shroud.xy);
	o.normal = i.normal;
	o.emissive = i.emissive;
	o.colour = i.colour * i.treeData.y;
	return o;
}

float4 TreesLitPS(LitVsOut i) : COLOR0
{
	float3 n = normalize(i.normal);
	float3 light = TreeLight[0].rgb + i.emissive;
	[unroll] for (int k = 0; k < 3; ++k)
		light += TreeLight[4 + k].rgb * saturate(dot(TreeLight[1 + k].xyz, n));
	light = saturate(light) * i.colour;

	float4 t = tex2D(TreeTexture, i.uv.xy);
	// Debug keys: the light alone. The alpha still cuts the leaves out (alpha test).
	if (TreeOptions.y > 0.5)
		return float4(light, t.a);
	float4 c = float4(t.rgb * light, t.a);
	if (TreeOptions.x > 0.5)
		c.rgb *= tex2D(TreeShroud, i.uv.zw).rgb;
	return c;
}
