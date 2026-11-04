// EastWind DX9: linear lighting (LinearLighting), shared by the lit pixel shaders.
//
// The game's textures, vertex colours and light colours are gamma encoded (sRGB-like). Adding
// light in that space makes sums too dark in the mid-tones and shading falls off too sharply.
// With linear lighting on, the shaders decode their colours, add the light up in linear space
// and encode the result again before it is written.
//
// The scene target keeps gamma values: fixed-function draws (shadows, shroud, decals, lasers)
// still blend into it, and their maths is gamma. A product of colours is the same in both
// spaces (pow spreads over multiplication), so only sums need the round trip. Fog stays gamma,
// so shader fog matches the fixed-function fog of the other draws; only the deep water (M3.3b)
// fogs in linear, as it adds the scene behind the water after its fog.
//
// Users: the mesh, terrain, road and tree lighting, the water's reflection, sun and refraction
// mix, and the post passes (bloom adds and tone maps linear light, post.hlsl).
//
// W3DDisplay sets FrameOptions once a frame, W3DPostProcess FrameSun around the scene; registers
// this high are used by nothing else.

#ifndef SHADER_LINEAR_H
#define SHADER_LINEAR_H

#include "shader_constants.h"

float4 FrameOptions : SHADER_REG(FRAME_PS_OPTIONS);	// x linear lighting on

bool Linear_Lighting() { return FrameOptions.x > 0.5; }

// Signed, so a negative light correction (terrain "extra" light) keeps its sign.
float3 To_Linear(float3 c) { return Linear_Lighting() ? sign(c) * pow(abs(c), 2.2) : c; }
float3 To_Gamma(float3 c)  { return Linear_Lighting() ? pow(max(c, 0.0), 1.0 / 2.2) : c; }

// SunIntensity: scales the sun (directional) diffuse and specular of the lit shaders. It is above 1
// only while the scene draws into the Hdr float target; then the lit colours are not clamped to
// white either, so bloom and the tone curve get real highlights. At 1 (also the unset register
// after a device reset) everything is clamped as before.
float4 FrameSun : SHADER_REG(FRAME_PS_SUN);

float Sun_Lift() { return max(FrameSun.x, 1.0); }
float3 Light_Clamp(float3 c) { return (FrameSun.x > 1.0) ? max(c, 0.0) : saturate(c); }

#endif // SHADER_LINEAR_H
