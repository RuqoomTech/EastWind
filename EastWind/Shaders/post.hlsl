// EastWind DX9: post-processing passes (M3.2).
// The 3D scene draws into a scene texture (EwPostProcess). These passes draw a
// screen quad from that texture onto the back buffer, before the 2D scene and UI.
//
// PostVS takes the quad already in clip space; EwPostProcess applies the D3D9
// half-pixel offset to the positions, so texel centres land on pixel centres.
//
// Bloom (M3.2b, EwBloom):
//   PostBrightPS   scene -> 1/4 size: 4x4 box average, keeps what is brighter than the threshold
//   PostDownPS     1/4 -> 1/8 -> 1/16: 4x4 box average
//   PostBlurPS     9-tap Gaussian along Texel.zw, run across and then down at each size
//   PostBloomPS    scene plus the three blurred sizes, onto the back buffer
// The scene is 8 bits a channel, so the bloom is added with a screen blend, which
// brightens without clipping to flat white.

#include "ew_constants.h"

float4 Texel        : EW_REG(EW_POST_PS_TEXEL);
float4 BloomParams  : EW_REG(EW_POST_PS_BLOOM);
float4 BloomWeights : EW_REG(EW_POST_PS_BLOOM_WEIGHTS);

sampler2D Scene   : register(s0);
sampler2D Bloom4  : register(s1);
sampler2D Bloom8  : register(s2);
sampler2D Bloom16 : register(s3);

struct PostVsIn
{
	float3 position : POSITION0;
	float2 uv       : TEXCOORD0;
};

struct PostVsOut
{
	float4 position : POSITION;
	float2 uv       : TEXCOORD0;
};

PostVsOut PostVS(PostVsIn i)
{
	PostVsOut o;
	o.position = float4(i.position.xy, 0.0, 1.0);
	o.uv = i.uv;
	return o;
}

// M3.2a: a plain copy, so the frame looks exactly as without the scene texture.
// Alpha is copied too: the 2D scene does not read it, but screenshots keep it.
float4 PostCopyPS(float2 uv : TEXCOORD0) : COLOR0
{
	return tex2D(Scene, uv);
}

// Four bilinear taps one source texel from the centre average a 4x4 block.
float3 Box4x4(float2 uv)
{
	float2 o = Texel.xy;
	float3 c = tex2D(Scene, uv + float2(-o.x, -o.y)).rgb;
	c += tex2D(Scene, uv + float2( o.x, -o.y)).rgb;
	c += tex2D(Scene, uv + float2(-o.x,  o.y)).rgb;
	c += tex2D(Scene, uv + float2( o.x,  o.y)).rgb;
	return c * 0.25;
}

float4 PostBrightPS(float2 uv : TEXCOORD0) : COLOR0
{
	float3 c = Box4x4(uv);
	float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
	// 0 at the threshold, 1 at full white; squared, so mid-bright ground glows little.
	float k = saturate((luma - BloomParams.x) * BloomParams.y);
	return float4(c * (k * k), 1.0);
}

float4 PostDownPS(float2 uv : TEXCOORD0) : COLOR0
{
	return float4(Box4x4(uv), 1.0);
}

// 9-tap Gaussian (sigma about 2 texels) in 5 bilinear taps.
float4 PostBlurPS(float2 uv : TEXCOORD0) : COLOR0
{
	float2 step1 = Texel.zw * 1.3846153846;
	float2 step2 = Texel.zw * 3.2307692308;
	float3 c = tex2D(Scene, uv).rgb * 0.2270270270;
	c += (tex2D(Scene, uv + step1).rgb + tex2D(Scene, uv - step1).rgb) * 0.3162162162;
	c += (tex2D(Scene, uv + step2).rgb + tex2D(Scene, uv - step2).rgb) * 0.0702702703;
	return float4(c, 1.0);
}

float4 PostBloomPS(float2 uv : TEXCOORD0) : COLOR0
{
	float4 scene = tex2D(Scene, uv);
	float3 bloom = tex2D(Bloom4, uv).rgb * BloomWeights.x
		+ tex2D(Bloom8, uv).rgb * BloomWeights.y
		+ tex2D(Bloom16, uv).rgb * BloomWeights.z;
	bloom = saturate(bloom * BloomParams.z);
	// Screen blend: 1 - (1 - scene) * (1 - bloom).
	return float4(scene.rgb + bloom * (1.0 - scene.rgb), scene.a);
}
