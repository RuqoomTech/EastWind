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
// The scene is 8 bits a channel, so the bloom is added with a screen blend, which
// brightens without clipping to flat white.
//
// Ambient occlusion (M3.5, EwAmbientOcclusion), from the scene's depth (an INTZ texture):
//   PostAoPS       the occlusion at half or full size: each pixel rebuilds its camera-space
//                  position and normal from the depth, then looks at 12 points on a spiral
//                  around it, a fixed world radius wide. A point counts as an occluder by how
//                  far it rises above the pixel's surface (the cosine to the normal), less with
//                  distance. The spiral turns by a per-pixel noise angle.
//   PostAoBlurPS   9-tap blur, run across and then down, that stops at depth edges, so the
//                  noise goes and the occlusion does not bleed from a unit onto the ground
//                  behind it.
// The AO texture holds the occlusion in R and the view depth in G. The final pass scales it
// up to the scene size by the four nearest AO texels whose depth matches the pixel's.
//
// PostFinalPS draws the scene onto the back buffer with what is on, in this order:
//   ambient occlusion  darkens the scene (M3.5, EwAmbientOcclusion)
//   bloom          the three blurred sizes, screen-blended over the scene (M3.2b, EwBloom)
//   tone mapping   a filmic curve (M3.2c, EwToneMapping)
//   colour grading a colour LUT (M3.2c, EwColorGrading)
// The switches are uniform branches, so the off parts cost nothing. Samples inside a
// branch use tex2Dlod, which needs no screen-space gradients.
//
// PostFxaaPS (M3.2d, EwFXAA) runs last, on the finished colours: it finds edges by their
// brightness contrast, follows each edge to its ends and blends across it, so jagged
// polygon and alpha-test edges look smooth. It reads only inside the view rectangle (Clamp).

#include "ew_constants.h"

float4 Texel        : EW_REG(EW_POST_PS_TEXEL);
float4 BloomParams  : EW_REG(EW_POST_PS_BLOOM);
float4 BloomWeights : EW_REG(EW_POST_PS_BLOOM_WEIGHTS);
float4 Final        : EW_REG(EW_POST_PS_FINAL);
float4 Tone         : EW_REG(EW_POST_PS_TONE);
float4 Clamp        : EW_REG(EW_POST_PS_CLAMP);

sampler2D Scene   : register(s0);
sampler2D Bloom4  : register(s1);
sampler2D Bloom8  : register(s2);
sampler2D Bloom16 : register(s3);
sampler2D ColorLut : register(s4);

float4 DepthParams : EW_REG(EW_POST_PS_DEPTH);
float4 AoRay       : EW_REG(EW_POST_PS_AO_RAY);
float4 AoParams    : EW_REG(EW_POST_PS_AO);
float4 AoParams2   : EW_REG(EW_POST_PS_AO2);
float4 AoFinal     : EW_REG(EW_POST_PS_AO_FINAL);

sampler2D AoMap      : register(s5);	// EW_POST_AO_SAMPLER
sampler2D SceneDepth : register(s6);	// EW_POST_DEPTH_SAMPLER

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

// Filmic curve: the ACES fit by Krzysztof Narkowicz. It darkens the deepest shadows a little,
// lifts the mid-tones and rolls the highlights off softly.
float3 FilmicCurve(float3 x)
{
	return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

// The scene holds display (gamma) values in 0..1. The curve works on linear light, and
// dividing by the curve at full white (Tone.y) keeps white at white, so nothing clips.
// The larger the exposure (Tone.x), the stronger the curve; near 0 it does nothing.
float3 ToneMap(float3 c)
{
	float3 linear_light = pow(max(c, 1.0e-5), 2.2) * Tone.x;
	return pow(saturate(FilmicCurve(linear_light) * Tone.y), 1.0 / 2.2);
}

// The colour LUT is a 256x16 strip: 16 slices of 16x16, blue picks the slice, red goes
// across a slice and green down. The two slices around the blue value are read with
// bilinear filtering and blended, so 16 steps a channel still give smooth colour.
float3 ColorGrade(float3 c)
{
	float3 cell = saturate(c) * 15.0;
	float slice = floor(cell.b);
	float2 uv = float2((slice * 16.0 + cell.r + 0.5) / 256.0, (cell.g + 0.5) / 16.0);
	float3 a = tex2Dlod(ColorLut, float4(uv, 0.0, 0.0)).rgb;
	float3 b = tex2Dlod(ColorLut, float4(uv.x + 16.0 / 256.0, uv.y, 0.0, 0.0)).rgb;
	return lerp(a, b, cell.b - slice);
}

// ---- Ambient occlusion (M3.5) ----

// The distance from the camera plane to what the pixel at uv shows.
float View_Depth(float2 uv)
{
	float z = tex2Dlod(SceneDepth, float4(uv, 0.0, 0.0)).r;
	return DepthParams.x / (DepthParams.y - z * DepthParams.z);
}

// Camera-space position (x right, y up, z the depth, away from the camera).
float3 View_Pos(float2 uv, float depth)
{
	return float3(uv * AoRay.xy + AoRay.zw, 1.0) * depth;
}

// The centre of the depth texel point sampling reads at uv (Texel.xy is one depth texel). A half-
// size AO pixel lands on a texel corner, where the texel picked flips from row to row; using the
// corner's position with that texel's depth put whole rows off the ground (horizontal stripes).
float2 Depth_Texel(float2 uv)
{
	return (floor(uv / Texel.xy) + 0.5) * Texel.xy;
}

float3 View_Pos(float2 uv)
{
	uv = Depth_Texel(uv);
	return View_Pos(uv, View_Depth(uv));
}

#define EW_AO_TAPS 12
// The spiral: tap k sits at (k + 0.5) / EW_AO_TAPS of the radius and turns 7 times around
// over all taps, so neighbouring taps point in very different directions.
static const float2 EwAoSpiral[EW_AO_TAPS] =
{
	float2(-0.258819,  0.965926), float2( 0.707107, -0.707107), float2(-0.965926,  0.258819),
	float2( 0.965926,  0.258819), float2(-0.707107, -0.707107), float2( 0.258819,  0.965926),
	float2( 0.258819, -0.965926), float2(-0.707107,  0.707107), float2( 0.965926, -0.258819),
	float2(-0.965926, -0.258819), float2( 0.707107,  0.707107), float2(-0.258819, -0.965926),
};

float4 PostAoPS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
	uv = Depth_Texel(uv);
	float depth = View_Depth(uv);
	float fade = saturate((DepthParams.w - depth) / (0.25 * DepthParams.w));
	[branch] if (fade <= 0.0)
		return float4(1.0, depth, 0.0, 1.0);

	float3 p = View_Pos(uv, depth);

	// Normal from the neighbouring depths. On each axis the nearer neighbour wins, so the
	// normal at a silhouette belongs to the surface the pixel is on.
	float2 tx = float2(Texel.x, 0.0);
	float2 ty = float2(0.0, Texel.y);
	float3 pl = View_Pos(uv - tx);
	float3 pr = View_Pos(uv + tx);
	float3 pu = View_Pos(uv - ty);
	float3 pd = View_Pos(uv + ty);
	float3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
	float3 dy = abs(pd.z - p.z) < abs(p.z - pu.z) ? pd - p : p - pu;
	float3 n = normalize(cross(dx, dy));
	n = dot(n, p) > 0.0 ? -n : n;	// facing the camera, which sits at the origin

	// The radius on screen, capped so close-ups stay fast.
	float2 radius_uv = AoParams.x * AoParams2.xy / depth;
	radius_uv *= min(1.0, AoParams.w / max(radius_uv.x, 1e-6));
	[branch] if (radius_uv.x < 2.0 * Texel.x)
		return float4(1.0, depth, 0.0, 1.0);

	// Interleaved gradient noise turns the spiral per pixel; the blur removes the pattern.
	float noise = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
	float s, c;
	sincos(noise * 6.2831853, s, c);
	float2x2 turn = float2x2(c, -s, s, c);

	float r2 = AoParams.x * AoParams.x;
	float sum = 0.0;
	[unroll] for (int k = 0; k < EW_AO_TAPS; ++k)
	{
		float along = (k + 0.5) / EW_AO_TAPS;
		float2 tap = mul(turn, EwAoSpiral[k]) * along;
		float2 tap_uv = clamp(uv + tap * radius_uv, Clamp.xy, Clamp.zw);
		float3 v = View_Pos(tap_uv) - p;
		float vv = dot(v, v);
		float rise = dot(v, n) * rsqrt(vv + 1e-4);
		float falloff = saturate(1.0 - vv / r2);
		sum += falloff * saturate(rise - AoParams.z);
	}
	float ao = saturate(1.0 - sum * AoParams.y * (2.0 / EW_AO_TAPS));
	return float4(lerp(1.0, ao, fade), depth, 0.0, 1.0);
}

// Depth-aware blur along Texel.zw (one AO texel across or down).
static const float EwAoBlurWeights[5] = { 0.2270, 0.1945, 0.1216, 0.0541, 0.0162 };

float Ao_Depth_Weight(float tap_depth, float depth)
{
	return saturate(1.0 - abs(tap_depth - depth) * AoFinal.y / depth);
}

float4 PostAoBlurPS(float2 uv : TEXCOORD0) : COLOR0
{
	float2 centre = tex2Dlod(AoMap, float4(uv, 0.0, 0.0)).rg;
	float sum = centre.r * EwAoBlurWeights[0];
	float weight = EwAoBlurWeights[0];
	[unroll] for (int k = 1; k < 5; ++k)
	{
		float2 a = tex2Dlod(AoMap, float4(clamp(uv + Texel.zw * k, Clamp.xy, Clamp.zw), 0.0, 0.0)).rg;
		float2 b = tex2Dlod(AoMap, float4(clamp(uv - Texel.zw * k, Clamp.xy, Clamp.zw), 0.0, 0.0)).rg;
		float wa = EwAoBlurWeights[k] * Ao_Depth_Weight(a.g, centre.g);
		float wb = EwAoBlurWeights[k] * Ao_Depth_Weight(b.g, centre.g);
		sum += a.r * wa + b.r * wb;
		weight += wa + wb;
	}
	return float4(sum / weight, centre.g, 0.0, 1.0);
}

// The occlusion at a scene pixel: the four AO texels around it, weighted by distance and by
// how well their depth matches the pixel's.
float Ao_At(float2 uv)
{
	float depth = View_Depth(uv);
	float2 t = uv / AoParams2.zw - 0.5;
	float2 base = floor(t);
	float2 f = t - base;
	float2 uv00 = (base + 0.5) * AoParams2.zw;
	float2 s00 = tex2Dlod(AoMap, float4(uv00, 0.0, 0.0)).rg;
	float2 s10 = tex2Dlod(AoMap, float4(uv00 + float2(AoParams2.z, 0.0), 0.0, 0.0)).rg;
	float2 s01 = tex2Dlod(AoMap, float4(uv00 + float2(0.0, AoParams2.w), 0.0, 0.0)).rg;
	float2 s11 = tex2Dlod(AoMap, float4(uv00 + AoParams2.zw, 0.0, 0.0)).rg;
	float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	w *= max(float4(Ao_Depth_Weight(s00.g, depth), Ao_Depth_Weight(s10.g, depth),
		Ao_Depth_Weight(s01.g, depth), Ao_Depth_Weight(s11.g, depth)), 1e-3);
	float ao = dot(w, float4(s00.r, s10.r, s01.r, s11.r)) / dot(w, 1.0);
	return pow(saturate(ao), AoFinal.z);
}

float4 PostFinalPS(float2 uv : TEXCOORD0) : COLOR0
{
	float4 scene = tex2D(Scene, uv);
	float3 c = scene.rgb;

	[branch] if (Final.w > 0.5)
	{
		float ao = Ao_At(uv);
		[branch] if (AoFinal.x > 0.5)
			return float4(ao.xxx, scene.a);
		c *= ao;
	}

	[branch] if (Final.x > 0.5)
	{
		float4 lod_uv = float4(uv, 0.0, 0.0);
		float3 bloom = tex2Dlod(Bloom4, lod_uv).rgb * BloomWeights.x
			+ tex2Dlod(Bloom8, lod_uv).rgb * BloomWeights.y
			+ tex2Dlod(Bloom16, lod_uv).rgb * BloomWeights.z;
		bloom = saturate(bloom * BloomParams.z);
		// Screen blend: 1 - (1 - scene) * (1 - bloom). Above white (EwHdr) it adds nothing.
		c = c + bloom * (1.0 - saturate(c));
	}

	[branch] if (Final.y > 0.5)
		c = ToneMap(c);

	[branch] if (Final.z > 0.5)
		c = ColorGrade(c);

	return float4(c, scene.a);
}

// FXAA strength, until it moves to INI data.
static const float FXAA_EDGE_RELATIVE = 0.125;	// contrast, relative to the brightest neighbour, that counts as an edge
static const float FXAA_EDGE_MIN = 0.0625;		// contrast below this is never an edge (dark areas)
static const float FXAA_SUBPIXEL = 0.75;		// how much single-pixel detail (thin lines, specks) is softened
static const int FXAA_SEARCH_STEPS = 8;
static const float FXAA_STEP_SIZE[FXAA_SEARCH_STEPS] = { 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

float FxaaLuma(float3 c)
{
	return dot(c, float3(0.299, 0.587, 0.114));
}

// The scene is read with linear filtering; uv stays inside the view rectangle.
float3 FxaaColor(float2 uv)
{
	return tex2Dlod(Scene, float4(clamp(uv, Clamp.xy, Clamp.zw), 0.0, 0.0)).rgb;
}

float FxaaLumaAt(float2 uv)
{
	return FxaaLuma(FxaaColor(uv));
}

float4 PostFxaaPS(float2 uv : TEXCOORD0) : COLOR0
{
	float2 px = Texel.xy;
	float4 centre = tex2Dlod(Scene, float4(uv, 0.0, 0.0));
	float lM = FxaaLuma(centre.rgb);
	float lN = FxaaLumaAt(uv + float2(0.0, -px.y));
	float lS = FxaaLumaAt(uv + float2(0.0,  px.y));
	float lW = FxaaLumaAt(uv + float2(-px.x, 0.0));
	float lE = FxaaLumaAt(uv + float2( px.x, 0.0));

	float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
	float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
	float range = lMax - lMin;
	[branch] if (range < max(FXAA_EDGE_MIN, lMax * FXAA_EDGE_RELATIVE))
		return centre;

	float lNW = FxaaLumaAt(uv + float2(-px.x, -px.y));
	float lNE = FxaaLumaAt(uv + float2( px.x, -px.y));
	float lSW = FxaaLumaAt(uv + float2(-px.x,  px.y));
	float lSE = FxaaLumaAt(uv + float2( px.x,  px.y));

	// Subpixel blend: how much the pixel stands out from the average around it.
	float lAverage = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
	float subpixel = saturate(abs(lAverage - lM) / range);
	subpixel = smoothstep(0.0, 1.0, subpixel);
	subpixel = subpixel * subpixel * FXAA_SUBPIXEL;

	// A horizontal edge changes brightness from row to row.
	float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
	float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
	bool horizontal = edgeH >= edgeV;

	// The edge lies between this pixel and the neighbour across it with the larger change.
	float l1 = horizontal ? lN : lW;
	float l2 = horizontal ? lS : lE;
	float gradient1 = abs(l1 - lM);
	float gradient2 = abs(l2 - lM);
	float across = horizontal ? px.y : px.x;
	float lEdge;
	float gradient;
	if (gradient1 >= gradient2)
	{
		across = -across;
		lEdge = 0.5 * (l1 + lM);
		gradient = gradient1;
	}
	else
	{
		lEdge = 0.5 * (l2 + lM);
		gradient = gradient2;
	}

	// Follow the edge both ways, half a pixel across, where filtering averages both sides.
	float2 edgeUv = uv;
	if (horizontal)
		edgeUv.y += 0.5 * across;
	else
		edgeUv.x += 0.5 * across;
	float2 along = horizontal ? float2(px.x, 0.0) : float2(0.0, px.y);
	float threshold = 0.25 * gradient;

	float2 uvP = edgeUv;
	float2 uvN = edgeUv;
	float endP = 0.0;
	float endN = 0.0;
	bool doneP = false;
	bool doneN = false;
	[unroll] for (int i = 0; i < FXAA_SEARCH_STEPS; ++i)
	{
		uvP = doneP ? uvP : uvP + along * FXAA_STEP_SIZE[i];
		uvN = doneN ? uvN : uvN - along * FXAA_STEP_SIZE[i];
		endP = doneP ? endP : FxaaLumaAt(uvP) - lEdge;
		endN = doneN ? endN : FxaaLumaAt(uvN) - lEdge;
		doneP = doneP || abs(endP) >= threshold;
		doneN = doneN || abs(endN) >= threshold;
	}

	float distP = horizontal ? uvP.x - uv.x : uvP.y - uv.y;
	float distN = horizontal ? uv.x - uvN.x : uv.y - uvN.y;
	bool nearerN = distN < distP;
	float dist = min(distP, distN);
	float span = distP + distN;

	// Blend only if the nearer end goes the other way from this pixel: then this pixel is on
	// the stair step the edge cuts through. The nearer the end, the more it blends.
	bool centreDarker = lM < lEdge;
	bool stepEnds = ((nearerN ? endN : endP) < 0.0) != centreDarker;
	float edgeBlend = stepEnds ? 0.5 - dist / span : 0.0;
	float blend = max(edgeBlend, subpixel);

	float2 finalUv = uv;
	if (horizontal)
		finalUv.y += blend * across;
	else
		finalUv.x += blend * across;
	return float4(FxaaColor(finalUv), centre.a);
}
