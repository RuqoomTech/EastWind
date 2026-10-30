// EastWind DX9: linear lighting (EwLinearLighting), shared by the lit pixel shaders.
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
// W3DDisplay sets FrameOptions once a frame; registers this high are used by nothing else.

#ifndef EW_LINEAR_H
#define EW_LINEAR_H

#include "shader_constants.h"

float4 FrameOptions : EW_REG(EW_FRAME_PS_OPTIONS);	// x linear lighting on

bool Ew_Linear() { return FrameOptions.x > 0.5; }

// Signed, so a negative light correction (terrain "extra" light) keeps its sign.
float3 Ew_To_Linear(float3 c) { return Ew_Linear() ? sign(c) * pow(abs(c), 2.2) : c; }
float3 Ew_To_Gamma(float3 c)  { return Ew_Linear() ? pow(max(c, 0.0), 1.0 / 2.2) : c; }

#endif // EW_LINEAR_H
