// EastWind DX9: reflective sea water (WATER_TYPE_2_PVSHADER), ported from wave.nvv and wave.nvp.
// The vertex shader projects the reflection texture onto the screen. The pixel shader offsets the
// reflection lookup by a bump map, which replaces ps.1.1 texbem (not in ps_2_0).

#include "ew_constants.h"

float4 WorldViewProj[4] : EW_REG(EW_WAVE_VS_WVP);
float4 TexProj          : EW_REG(EW_WAVE_VS_TEXPROJ);

struct VsIn
{
	float4 position : POSITION0;
	float4 diffuse  : COLOR0;
	float4 uv       : TEXCOORD0;
};

struct VsOut
{
	float4 position : POSITION;
	float4 diffuse  : COLOR0;
	float4 uv0      : TEXCOORD0;
	float2 uv1      : TEXCOORD1;
};

VsOut WaveVS(VsIn i)
{
	VsOut o;
	float4 clip = float4(dot(i.position, WorldViewProj[0]), dot(i.position, WorldViewProj[1]),
		dot(i.position, WorldViewProj[2]), dot(i.position, WorldViewProj[3]));
	o.position = clip;
	o.uv1 = (clip.xy / clip.w) * TexProj.xy + TexProj.zw;
	o.uv0 = i.uv;
	o.diffuse = i.diffuse;
	return o;
}

float4 BumpMatrix : EW_REG(EW_WAVE_PS_BUMPMAT); // (M00, M01, M10, M11)

sampler2D BumpMap    : register(s0);
sampler2D Reflection : register(s1);

struct PsIn
{
	float4 diffuse : COLOR0;
	float2 uv0     : TEXCOORD0;
	float2 uv1     : TEXCOORD1;
};

float4 WavePS(PsIn i) : COLOR
{
	float2 du = tex2D(BumpMap, i.uv0).rg;
	float2 uv = i.uv1 + float2(dot(du, BumpMatrix.xz), dot(du, BumpMatrix.yw));
	return tex2D(Reflection, uv) * i.diffuse;
}
