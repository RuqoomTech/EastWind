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
// PostFinalPS draws the scene onto the back buffer with what is on, in this order:
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

float4 PostFinalPS(float2 uv : TEXCOORD0) : COLOR0
{
	float4 scene = tex2D(Scene, uv);
	float3 c = scene.rgb;

	[branch] if (Final.x > 0.5)
	{
		float4 lod_uv = float4(uv, 0.0, 0.0);
		float3 bloom = tex2Dlod(Bloom4, lod_uv).rgb * BloomWeights.x
			+ tex2Dlod(Bloom8, lod_uv).rgb * BloomWeights.y
			+ tex2Dlod(Bloom16, lod_uv).rgb * BloomWeights.z;
		bloom = saturate(bloom * BloomParams.z);
		// Screen blend: 1 - (1 - scene) * (1 - bloom).
		c = c + bloom * (1.0 - c);
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
