// EastWind DX9: black and white screen filter, ported from monochrome.nvp.

#include "ew_constants.h"

float4 Luminance : EW_REG(EW_MONO_PS_LUMINANCE);
float4 Color     : EW_REG(EW_MONO_PS_COLOR);
float4 Fade      : EW_REG(EW_MONO_PS_FADE);

sampler2D Scene : register(s0);

float4 MonochromePS(float2 uv : TEXCOORD0) : COLOR
{
	float4 scene = tex2D(Scene, uv);
	float4 grey = dot(scene.rgb, Luminance.rgb) * Color;
	return lerp(scene, grey, Fade);
}
