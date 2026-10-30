// EastWind DX9: sun shadow map lookup (M3.4, ShadowMapSize), shared by the lit pixel shaders.
//
// ShadowMap (shadowmap.cpp) draws the depth of the shadow casters (meshes and trees) seen
// from the sun into an orthographic depth map once a frame, then binds it to s12 and the
// constants below for the rest of the frame. The terrain does not cast, so it never shadows
// itself; hills do not throw shadows, as in the classic game.
//
// Two depth formats:
//   - D24X8 depth texture: the sampler compares the depth itself and filters the result
//     bilinearly (hardware PCF). The reference depth goes in the z coordinate of tex2Dlod.
//   - INTZ: the sampler returns the stored depth, the shader compares (point sampled).
// Each lookup takes a Poisson disc of taps around the pixel, which softens the edge over a few
// texels. Biases against acne: the receiver point moves along its normal by about a texel,
// more where the light is at a grazing angle, and a small constant depth bias. The caster pass
// adds the slope-scaled depth bias.

#ifndef SHADER_SHADOW_H
#define SHADER_SHADOW_H

#include "shader_constants.h"

float4 ShadowMatrix[3] : SHADER_REG(SHADOW_PS_MATRIX);	// rows: u, v, depth
float4 ShadowParams    : SHADER_REG(SHADOW_PS_PARAMS);	// x on, y 1 / size, z radius in texels, w normal offset
float4 ShadowParams2   : SHADER_REG(SHADOW_PS_PARAMS2);	// x hardware compare, y depth bias, w strength
sampler2D ShadowMap    : register(s12);

#define SHADOW_TAPS 12
static const float2 ShadowPoisson[SHADOW_TAPS] =
{
	float2(-0.326212, -0.405805), float2(-0.840144, -0.073580), float2(-0.695914,  0.457137),
	float2(-0.203345,  0.620716), float2( 0.962340, -0.194983), float2( 0.473434, -0.480026),
	float2( 0.519456,  0.767022), float2( 0.185461, -0.893124), float2( 0.507431,  0.064425),
	float2( 0.896420,  0.412458), float2(-0.321940, -0.932615), float2(-0.791559, -0.597705),
};

// How much sun reaches world point p with normal n (unit), l the unit direction to the sun:
// 1 lit, 0 in shadow. 1 when shadow maps are off or p lies outside the map.
float Sun_Shadow(float3 p, float3 n, float3 l)
{
	[branch] if (ShadowParams.x < 0.5)
		return 1.0;

	float nl = saturate(dot(n, l));
	float4 q = float4(p + n * (ShadowParams.w * (1.0 - nl)), 1.0);
	float3 uvz = float3(dot(q, ShadowMatrix[0]), dot(q, ShadowMatrix[1]), dot(q, ShadowMatrix[2]));

	// Fade out over the outer texels of the map, so its edge never shows as a line.
	float2 edge = min(uvz.xy, 1.0 - uvz.xy);
	float fade = saturate(min(edge.x, edge.y) * 24.0);
	[branch] if (fade <= 0.0 || uvz.z >= 1.0)
		return 1.0;

	float z = uvz.z - ShadowParams2.y;
	float2 radius = ShadowParams.yy * ShadowParams.z;
	float lit = 0.0;
	[branch] if (ShadowParams2.x > 0.5)
	{
		[unroll] for (int k = 0; k < SHADOW_TAPS; ++k)
			lit += tex2Dlod(ShadowMap, float4(uvz.xy + ShadowPoisson[k] * radius, z, 0.0)).r;
	}
	else
	{
		[unroll] for (int k = 0; k < SHADOW_TAPS; ++k)
			lit += (z <= tex2Dlod(ShadowMap, float4(uvz.xy + ShadowPoisson[k] * radius, 0.0, 0.0)).r) ? 1.0 : 0.0;
	}
	lit *= 1.0 / SHADOW_TAPS;
	return lerp(1.0, lit, fade * ShadowParams2.w);
}

#endif // SHADER_SHADOW_H
