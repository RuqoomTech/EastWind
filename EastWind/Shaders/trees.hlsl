// EastWind DX9: tree and bush vertex shader, ported from Trees.nvv (vs.1.1).
// Sways trees by their wave entry, darkens pushed-aside trees and maps the shroud texture.
// The pixel stages stay fixed-function, as in DX8 (Trees.pso was never used).
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
