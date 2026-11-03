// EastWind DX9: sun shadow map lookup (M3.4, ShadowMapSize), shared by the lit pixel shaders.
//
// ShadowMap (shadowmap.cpp) draws the depth of the shadow casters (meshes and trees) seen
// from the sun into an orthographic depth map once a frame, then binds it to s12 and the
// constants below for the rest of the frame. The terrain casts too, so hills and cliffs throw
// shadows over the ground and units behind them.
//
// Two cascades share one atlas, each a square of the map size: the near cascade (left half)
// covers the ground up to the view centre in finer texels, the far cascade (right half) the
// whole view. Each pixel takes the near one when its whole filter disc fits inside it.
//
// Two depth formats:
//   - INTZ (preferred): the sampler returns the stored depth and the shader compares (point
//     sampled). A first ring of taps finds how far the casters are; the edge is sharp where a
//     caster meets the ground and widens with that distance, as real shadows do.
//   - D24X8 depth texture: the sampler compares the depth itself and filters the result
//     bilinearly (hardware PCF), at one fixed softness. The reference depth goes in the z
//     coordinate of tex2Dlod.
// Each lookup takes a disc of taps around the pixel (a golden-angle spiral, turned by a
// per-pixel angle so the taps do not show as steps), which softens the edge over a few world
// units. Biases against acne: the receiver point moves along its normal by about a texel,
// more where the light is at a grazing angle, a small constant depth bias, and a bias for the
// receiver's slope across the disc. The caster pass adds the slope-scaled depth bias.

#ifndef SHADER_SHADOW_H
#define SHADER_SHADOW_H

#include "shader_constants.h"

float4 ShadowMatrix[3] : SHADER_REG(SHADOW_PS_MATRIX);	// rows: u, v, depth
float4 ShadowParams3   : SHADER_REG(SHADOW_PS_PARAMS3);	// x world per u, y world per depth, z softness at contact, w softness per unit
float4 ShadowParams    : SHADER_REG(SHADOW_PS_PARAMS);	// x on, y 1 / size, z radius in texels, w normal offset
float4 ShadowParams2   : SHADER_REG(SHADOW_PS_PARAMS2);	// x hardware compare, y depth bias, z radius in depth units, w strength
float4 ShadowNearMatrix[3] : SHADER_REG(SHADOW_PS_NEAR_MATRIX);	// the near cascade's rows: u, v, depth
float4 ShadowNear      : SHADER_REG(SHADOW_PS_NEAR);	// near cascade: x world per u, y radius in texels, z normal offset, w radius in depth units
sampler2D ShadowMap    : register(s12);	// the atlas: near cascade in the left half, far in the right

#define SHADOW_TAPS 24

// Tap k of the disc: a golden-angle spiral, even over the area; constant once unrolled.
float2 Shadow_Tap(int k)
{
	float r = sqrt((k + 0.5) / SHADOW_TAPS);
	float a = k * 2.39996323;
	return r * float2(cos(a), sin(a));
}

// How much sun reaches world point p with normal n (unit), l the unit direction to the sun:
// 1 lit, 0 in shadow. 1 when shadow maps are off or p lies outside the map.
float Sun_Shadow(float3 p, float3 n, float3 l)
{
	[branch] if (ShadowParams.x < 0.5)
		return 1.0;

	float nl = saturate(dot(n, l));

	// The near cascade (finer texels) when the point and its widest filter disc lie inside it,
	// else the far cascade, which covers the whole view. cascade: x world per u, y filter radius in
	// texels, z normal offset, w filter radius in depth units.
	float4 cascade = ShadowNear;
	float4 q = float4(p + n * (cascade.z * (1.0 - nl)), 1.0);
	float3 uvz = float3(dot(q, ShadowNearMatrix[0]), dot(q, ShadowNearMatrix[1]), dot(q, ShadowNearMatrix[2]));
	float2 edge = min(uvz.xy, 1.0 - uvz.xy);
	float atlasU = 0.0;
	float fade = 1.0;
	[branch] if (min(edge.x, edge.y) <= (cascade.y + 1.0) * ShadowParams.y)
	{
		cascade = float4(ShadowParams3.x, ShadowParams.z, ShadowParams.w, ShadowParams2.z);
		q = float4(p + n * (cascade.z * (1.0 - nl)), 1.0);
		uvz = float3(dot(q, ShadowMatrix[0]), dot(q, ShadowMatrix[1]), dot(q, ShadowMatrix[2]));
		// Fade out over the outer texels of the map, so its edge never shows as a line.
		edge = min(uvz.xy, 1.0 - uvz.xy);
		fade = saturate(min(edge.x, edge.y) * 24.0);
		atlasU = 0.5;
	}
	[branch] if (fade <= 0.0 || uvz.z >= 1.0)
		return 1.0;

	// A tilted receiver drifts toward the light across the disc; tan of the light angle, capped.
	float slope = min(sqrt(saturate(1.0 - nl * nl)) / max(nl, 0.05), 2.0);
	float z = uvz.z - ShadowParams2.y - cascade.w * slope;

	// Per-pixel turn of the disc (interleaved gradient noise on the map texel), so the wide
	// filter shows as fine grain instead of stepped copies of the edge.
	float2 texel = uvz.xy / ShadowParams.y;
	float turn = 6.2831853 * frac(52.9829189 * frac(dot(texel, float2(0.06711056, 0.00583715))));
	float2 cs = float2(cos(turn), sin(turn));
	float2x2 spin = float2x2(cs.x, -cs.y, cs.y, cs.x);
	float2 radius = ShadowParams.yy * cascade.y;
	// The cascade's u (0..1) into its half of the atlas.
	float2 toAtlas = float2(0.5, 1.0);
	float2 atlas = uvz.xy * toAtlas + float2(atlasU, 0.0);
	float lit = 0.0;
	[branch] if (ShadowParams2.x > 0.5)
	{
		[unroll] for (int k = 0; k < SHADOW_TAPS; ++k)
			lit += tex2Dlod(ShadowMap, float4(atlas + mul(spin, Shadow_Tap(k)) * radius * toAtlas, z, 0.0)).r;
	}
	else
	{
		// Blockers: the average depth of the casters within the widest edge.
		float blocker = 0.0;
		float blockers = 0.0;
		[unroll] for (int b = 0; b < SHADOW_TAPS; b += 2)
		{
			float d = tex2Dlod(ShadowMap, float4(atlas + mul(spin, Shadow_Tap(b)) * radius * toAtlas, 0.0, 0.0)).r;
			if (d < z)
			{
				blocker += d;
				blockers += 1.0;
			}
		}
		[branch] if (blockers == 0.0)
			return 1.0;
		// Every tap of the widest disc is covered: deep shadow. The filter disc lies inside it and
		// compares at zr >= z, so it finds the same, short of a lit gap narrower than the taps.
		[branch] if (blockers == SHADOW_TAPS / 2)
			return lerp(1.0, 0.0, fade * ShadowParams2.w);

		// Edge width from the caster's distance, between one texel and the widest edge.
		float reach = (z - blocker / blockers) * ShadowParams3.y;
		float soft = ShadowParams3.z + reach * ShadowParams3.w;
		float r = clamp(soft / cascade.x, ShadowParams.y, radius.x);
		float zr = z + cascade.w * slope * (1.0 - r / radius.x);	// the slope bias of the smaller disc
		[unroll] for (int k = 0; k < SHADOW_TAPS; ++k)
			lit += (zr <= tex2Dlod(ShadowMap, float4(atlas + mul(spin, Shadow_Tap(k)) * r * toAtlas, 0.0, 0.0)).r) ? 1.0 : 0.0;
	}
	lit *= 1.0 / SHADOW_TAPS;
	return lerp(1.0, lit, fade * ShadowParams2.w);
}

#endif // SHADER_SHADOW_H
