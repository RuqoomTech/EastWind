// EastWind DX9: river and trapezoid water pixel shaders. In DX8 W3DWater.cpp assembled them
// from ps.1.1 text at runtime; they run on fixed-function vertex processing.

sampler2D Tex0 : register(s0);
sampler2D Tex1 : register(s1);
sampler2D Tex2 : register(s2);
sampler2D Tex3 : register(s3);

struct PsIn
{
	float4 diffuse : COLOR0;
	float2 uv0     : TEXCOORD0;
	float2 uv1     : TEXCOORD1;
	float2 uv2     : TEXCOORD2;
	float2 uv3     : TEXCOORD3;
};

// Rivers: t0 water, t1 sparkles, t2 noise, t3 alpha edge.
// Vertex alpha fades the sparkles in, not the base water.
float4 RiverPS(PsIn i) : COLOR
{
	float4 t0 = tex2D(Tex0, i.uv0);
	float4 t1 = tex2D(Tex1, i.uv1);
	float4 t2 = tex2D(Tex2, i.uv2);
	float4 t3 = tex2D(Tex3, i.uv3);
	float3 sparkles = (t1.rgb * t2.rgb + t3.rgb) * i.diffuse.a;
	return float4(i.diffuse.rgb * t0.rgb + sparkles, t0.a * t3.a);
}

// Trapezoid (flat) water: t0 water, t1 sparkles, t2 noise with more tiling, t3 black shroud.
float4 TrapezoidPS(PsIn i) : COLOR
{
	float4 t0 = tex2D(Tex0, i.uv0);
	float4 t1 = tex2D(Tex1, i.uv1);
	float4 t2 = tex2D(Tex2, i.uv2);
	float4 t3 = tex2D(Tex3, i.uv3);
	float4 color = i.diffuse * t0;
	color.rgb = (t1.rgb * t2.rgb + color.rgb) * t3.rgb;
	return color;
}

// ------------------------------------------------------------------------------------------------
// M3.3a water shader (Options.ini EwWaterQuality = 1), bound by EwWater_dx9.cpp over the pass the
// parity shaders above would draw. It keeps their textures and adds:
// - waves: two layers of a tiling normal map (s4) scrolling in different directions;
// - Fresnel: the water reflects more where the view is flat to it;
// - reflection: the mirrored scene (s5), drawn before the frame, read at the pixel's screen
//   position and moved by the wave normal;
// - the sun's specular highlight on the waves.
// ps_3_0 has no fixed-function fog, so the pixel shader fogs itself.
// ------------------------------------------------------------------------------------------------

#include "ew_constants.h"

float4 WaterWorldViewProj[4] : EW_REG(EW_WATER_VS_WVP);
float4 WaterWorld[3]         : EW_REG(EW_WATER_VS_WORLD);
float4 WaterWorldView[3]     : EW_REG(EW_WATER_VS_WORLDVIEW);
float4 NoiseMatrix[2]        : EW_REG(EW_WATER_VS_NOISE);
float4 ShroudMatrix[2]       : EW_REG(EW_WATER_VS_SHROUD);
float4 WaterVsOptions        : EW_REG(EW_WATER_VS_OPTIONS);	// x range fog
float4 WaveOffsets           : EW_REG(EW_WATER_VS_WAVES);	// xy layer A, zw layer B
float4 WaveScale             : EW_REG(EW_WATER_VS_WAVE_SCALE);	// x layer A, y layer B, z river, w river tiling

float4 SunDirection          : EW_REG(EW_WATER_PS_SUN);
float4 SunColor              : EW_REG(EW_WATER_PS_SUN_COLOR);	// w specular power
float4 CameraPosition        : EW_REG(EW_WATER_PS_CAMERA);
float4 Fresnel               : EW_REG(EW_WATER_PS_FRESNEL);	// x bias, y scale, z power, w most reflection
float4 WaterPsOptions        : EW_REG(EW_WATER_PS_OPTIONS);	// x distortion, y normal strength, z specular strength
float4 WaterFog              : EW_REG(EW_WATER_PS_FOG);	// x enable, y end, z 1 / (end - start)
float4 WaterFogColor         : EW_REG(EW_WATER_PS_FOGCOLOR);
float4 SkyColor              : EW_REG(EW_WATER_PS_SKY);	// w has reflection
float4 RiverOptions          : EW_REG(EW_WATER_PS_RIVER);	// x INI vertex alpha
float4 WaveLook              : EW_REG(EW_WATER_PS_WAVE_LOOK);	// x wave shading, y texture wobble, z reflection opacity

sampler2D WaveNormals : register(s4);
sampler2D Reflection  : register(s5);

// M3.4: shadows of units, buildings and trees on the water take away the sun's highlights and
// some of the sunlit wave shading.
#include "ew_shadow.h"
#include "ew_linear.h"

// The water vertices are VertexFormatXYZNDUV2.
struct WaterVsIn
{
	float4 position : POSITION0;
	float3 normal   : NORMAL0;
	float4 diffuse  : COLOR0;
	float2 uv0      : TEXCOORD0;
	float2 uv1      : TEXCOORD1;
};

struct WaterVsOut
{
	float4 position : POSITION;
	float4 diffuse  : COLOR0;
	float4 uv01     : TEXCOORD0;	// xy water and sparkles, zw river alpha edge
	float4 uv23     : TEXCOORD1;	// xy noise, zw shroud
	float4 waves    : TEXCOORD2;	// xy layer A, zw layer B
	float4 worldFog : TEXCOORD3;	// xyz world position, w fog distance
	float3 screen   : TEXCOORD4;	// clip x, y, w
};

WaterVsOut WaterVS(WaterVsIn i)
{
	WaterVsOut o;
	float4 p = float4(i.position.xyz, 1.0);
	o.position = float4(dot(p, WaterWorldViewProj[0]), dot(p, WaterWorldViewProj[1]),
		dot(p, WaterWorldViewProj[2]), dot(p, WaterWorldViewProj[3]));
	float3 world = float3(dot(p, WaterWorld[0]), dot(p, WaterWorld[1]), dot(p, WaterWorld[2]));
	float4 cs = float4(dot(p, WaterWorldView[0]), dot(p, WaterWorldView[1]), dot(p, WaterWorldView[2]), 1.0);

	o.diffuse = i.diffuse;
	o.uv01 = float4(i.uv0, i.uv1);
	// Stages 2 and 3 used camera-space position through a texture matrix, two outputs each.
	o.uv23 = float4(dot(cs, NoiseMatrix[0]), dot(cs, NoiseMatrix[1]), dot(cs, ShroudMatrix[0]), dot(cs, ShroudMatrix[1]));

	// Trapezoid water lays the waves out in world xy. Rivers lay them along the river (u2 goes
	// across, v1 along it in river widths, scrolling with the flow), so the waves follow it.
	float2 waveBase = (WaveScale.z > 0.5) ? float2(i.uv1.x, i.uv0.y) * WaveScale.w : world.xy;
	o.waves = float4(waveBase * WaveScale.x + WaveOffsets.xy, waveBase.yx * WaveScale.y + WaveOffsets.zw);

	// D3D vertex fog: camera-space depth, or distance with D3DRS_RANGEFOGENABLE.
	float fogDistance = (WaterVsOptions.x > 0.5) ? length(cs.xyz) : abs(cs.z);
	o.worldFog = float4(world, fogDistance);
	o.screen = o.position.xyw;
	return o;
}

struct WaterPsIn
{
	float4 diffuse  : COLOR0;
	float4 uv01     : TEXCOORD0;
	float4 uv23     : TEXCOORD1;
	float4 waves    : TEXCOORD2;
	float4 worldFog : TEXCOORD3;
	float3 screen   : TEXCOORD4;
};

struct WaterLight
{
	float3 normal;		// the wave normal
	float3 reflection;	// what the water mirrors, linear with EwLinearLighting (ew_linear.h)
	float3 specular;	// the sun on the waves, linear with EwLinearLighting
	float fresnel;		// how much of the reflection shows
	float shade;		// sun light on the wave relative to flat water, 1 on average
	float shadow;		// sun shadow map: 1 lit, 0 in shadow
};

WaterLight Water_Light_From(WaterPsIn i, float3 n);

WaterLight Water_Light(WaterPsIn i)
{
	// Whiteout blend of the two layers, flattened or steepened by the normal strength.
	float3 a = tex2D(WaveNormals, i.waves.xy).xyz * 2.0 - 1.0;
	float3 b = tex2D(WaveNormals, i.waves.zw).xyz * 2.0 - 1.0;
	float3 n = normalize(float3((a.xy + b.xy) * WaterPsOptions.y, a.z * b.z));
	return Water_Light_From(i, n);
}

WaterLight Water_Light_From(WaterPsIn i, float3 n)
{
	float3 toEye = normalize(CameraPosition.xyz - i.worldFog.xyz);
	float facing = saturate(dot(n, toEye));

	WaterLight l;
	l.normal = n;
	l.shadow = Ew_Sun_Shadow(i.worldFog.xyz, float3(0.0, 0.0, 1.0), SunDirection.xyz);
	// Slopes toward the sun are brighter and slopes away darker, so the waves show on the water
	// itself. Relative to flat water, so the water keeps its classic brightness on average.
	float sunUp = max(SunDirection.z, 0.25);
	l.shade = max(1.0 + WaveLook.x * (dot(n, SunDirection.xyz) - SunDirection.z) / sunUp, 0.0);
	l.shade *= lerp(0.8, 1.0, l.shadow);
	l.fresnel = saturate(Fresnel.x + Fresnel.y * pow(1.0 - facing, Fresnel.z)) * Fresnel.w;

	// Points on the water plane are at the same screen position in the mirrored view.
	float2 screenUv = i.screen.xy / i.screen.z * float2(0.5, -0.5) + 0.5;
	float3 mirrored = tex2D(Reflection, screenUv + n.xy * WaterPsOptions.x).rgb;
	l.reflection = Ew_To_Linear(lerp(SkyColor.rgb, mirrored, SkyColor.w));

	float3 h = normalize(SunDirection.xyz + toEye);
	l.specular = Ew_To_Linear(SunColor.rgb) * (pow(saturate(dot(n, h)), SunColor.w) * WaterPsOptions.z * l.shadow);
	return l;
}

float3 Water_Fog(float3 c, float distance)
{
	[branch] if (WaterFog.x > 0.5)
	{
		float f = saturate((WaterFog.y - distance) * WaterFog.z);
		c = lerp(WaterFogColor.rgb, c, f);
	}
	return c;
}

// Trapezoid water: as TrapezoidPS, with the reflection and sun before the shroud darkens it.
float4 WaterTrapezoidPS(WaterPsIn i) : COLOR
{
	WaterLight l = Water_Light(i);
	// The waves wobble the water texture as if seen through moving water.
	float4 t0 = tex2D(Tex0, i.uv01.xy + l.normal.xy * WaveLook.y);
	float4 t1 = tex2D(Tex1, i.uv01.xy);
	float4 t2 = tex2D(Tex2, i.uv23.xy);
	float4 t3 = tex2D(Tex3, i.uv23.zw);

	float4 base = i.diffuse * t0;
	base.rgb *= l.shade;
	// The reflection and sun mix in linear space; the shroud multiplies, so it can stay gamma.
	float3 rgb = Ew_To_Gamma(lerp(Ew_To_Linear(base.rgb) + Ew_To_Linear(t1.rgb * t2.rgb), l.reflection, l.fresnel) + l.specular);
	rgb *= t3.rgb;
	// Reflections show on the water even where it is see-through, part way so it stays clear.
	float alpha = saturate(base.a + l.fresnel * WaveLook.z * (1.0 - base.a));
	return float4(Water_Fog(rgb, i.worldFog.w), alpha);
}

// Rivers: as RiverPS. The vertex alpha is the INI alpha times the shroud, so the shroud part
// fades the reflection and sun too.
float4 WaterRiverPS(WaterPsIn i) : COLOR
{
	WaterLight l = Water_Light(i);
	float4 t0 = tex2D(Tex0, i.uv01.xy + l.normal.xy * WaveLook.y);
	float4 t1 = tex2D(Tex1, i.uv01.xy);
	float4 t2 = tex2D(Tex2, i.uv23.xy);
	float4 t3 = tex2D(Tex3, i.uv01.zw);

	float shroud = saturate(i.diffuse.a / max(RiverOptions.x, 1.0 / 255.0));
	float3 sparkles = (t1.rgb * t2.rgb + t3.rgb) * i.diffuse.a;
	float3 base = Ew_To_Linear(i.diffuse.rgb * t0.rgb * l.shade) + Ew_To_Linear(sparkles);
	float fresnel = l.fresnel * shroud;
	float shroudLinear = Ew_To_Linear(shroud.xxx).x;
	float3 rgb = Ew_To_Gamma(lerp(base, l.reflection * shroudLinear, fresnel) + l.specular * shroudLinear);
	float alpha = saturate(t0.a + fresnel * WaveLook.z * (1.0 - t0.a)) * t3.a;
	return float4(Water_Fog(rgb, i.worldFog.w), alpha);
}

// ------------------------------------------------------------------------------------------------
// M3.3b water seen into (Options.ini EwWaterQuality = 2). The same waves, Fresnel, reflection and
// sun as above, but the water is no longer a see-through texture over the ground:
// - refraction: the scene behind the water (s6, copied before the first water draw of the frame),
//   read at the pixel's screen position and moved by the waves where the water is deep enough;
// - depth: the terrain height (s7) under the pixel gives how much water the light goes through.
//   Light is absorbed per colour, red first, so shallows show the ground in a clear blue-green and
//   deep water turns to the water's own colour, the map's classic water colour darkened;
// - caustics: light focused by the waves dances on the ground in the shallows;
// - shore foam (s8): a broken band along the shore that pulses with slow shore waves.
// The classic water and sparkle textures are not drawn; the average colour of the water texture
// (its smallest mip) tints the deep water, so each map keeps its water colour.
// ------------------------------------------------------------------------------------------------

float4 RefractionScreen      : EW_REG(EW_WATER_PS_SCREEN);	// xy scale, zw offset
float4 HeightMapping         : EW_REG(EW_WATER_PS_HEIGHT);	// xy scale, zw offset
float4 WaterBody             : EW_REG(EW_WATER_PS_BODY);	// x height per value, y has heights, z shore phase, w distortion
float4 Absorption            : EW_REG(EW_WATER_PS_ABSORB);	// rgb per world unit, w deep colour scale
float4 ShoreFoam             : EW_REG(EW_WATER_PS_FOAM);	// x foam depth, y foam strength, z caustics, w caustic fade depth
float4 AmbientLight          : EW_REG(EW_WATER_PS_AMBIENT);	// rgb ambient, w broad glint strength
float4 WaveMix               : EW_REG(EW_WATER_PS_WAVE_MIX);	// x swell size, y swell strength, z ripple strength

sampler2D Refraction    : register(s6);
sampler2D TerrainHeight : register(s7);
sampler2D FoamNoise     : register(s8);	// r foam lines and caustic web, g foam bubbles

// The waves seen into: a broad, slow swell (the same normal map, much larger and slower) under
// the two ripple layers, toned down, so the water rolls instead of crinkling.
float3 Water_Normal_Hq(WaterPsIn i)
{
	float3 s = tex2D(WaveNormals, i.waves.xy * WaveMix.x).xyz * 2.0 - 1.0;
	float3 a = tex2D(WaveNormals, i.waves.xy).xyz * 2.0 - 1.0;
	float3 b = tex2D(WaveNormals, i.waves.zw).xyz * 2.0 - 1.0;
	float2 slope = s.xy * WaveMix.y + (a.xy + b.xy) * WaveMix.z;
	return normalize(float3(slope, 1.0));
}

struct WaterLayers
{
	float3 behind;		// weight of the refraction, which is already shrouded and fogged
	float3 refraction;	// the scene behind the water
	float3 surface;		// all else: water colour, reflection, sun and foam, to shroud and fog
	float3 surfaceCover;	// 1 - behind, how much of the result the surface colour owns
};

WaterLayers Water_Layers(WaterPsIn i, WaterLight l, float3 tint)
{
	// How deep the water is here, and how far the view goes through it.
	float2 heightUv = i.worldFog.xy * HeightMapping.xy + HeightMapping.zw;
	float ground = tex2Dlod(TerrainHeight, float4(heightUv, 0, 0)).r * WaterBody.x;
	float depth = (WaterBody.y > 0.5) ? max(i.worldFog.z - ground, 0.0) : 40.0;
	float3 toEye = normalize(CameraPosition.xyz - i.worldFog.xyz);
	float path = depth * (1.0 + 1.0 / max(toEye.z, 0.25));	// down to the ground and back up
	float3 through = exp(-Absorption.rgb * path);

	// The ground behind, bent by the waves. No bending at the shore, where it would show the land.
	float2 screenUv = i.screen.xy / i.screen.z * RefractionScreen.xy + RefractionScreen.zw;
	float bend = WaterBody.w * saturate(depth * 0.1);
	float3 behind = Ew_To_Linear(tex2D(Refraction, screenUv + l.normal.xy * bend).rgb);
	float3 sun = Ew_To_Linear(SunColor.rgb);

	// Caustics: two webs of focused light crossing on the ground, strongest in the shallows.
	float2 causticUv = i.waves.xy * 3.1 + l.normal.xy * 0.08;
	float web = min(tex2D(FoamNoise, causticUv).r, tex2D(FoamNoise, i.waves.zw * 2.3 + 0.37).r);
	float causticFade = saturate(depth * 0.5) * saturate(1.0 - depth / ShoreFoam.w);
	behind *= 1.0 + sun * (pow(web, 4.0) * ShoreFoam.z * causticFade);

	// The water's own colour, lit by the waves, where the light is used up.
	float3 deep = Ew_To_Linear(tint) * Absorption.w * l.shade;

	// Shore foam: bubbles and streaks in a band along the shore that grows and shrinks with slow
	// waves running in, broken up by the noise so it is never a clean line.
	float streaks = tex2D(FoamNoise, i.waves.xy * 4.0 + l.normal.xy * 0.04).r;
	float bubbles = tex2D(FoamNoise, i.waves.zw * 6.0).g;
	float pattern = streaks * 0.55 + bubbles * 0.6;
	float wave = 0.6 + 0.4 * sin(WaterBody.z + depth * 1.2 + pattern * 1.5);
	float band = saturate(1.0 - depth / (ShoreFoam.x * wave));
	band *= band;
	float edgeLine = saturate(1.0 - depth * 3.0);	// a thin wash right at the water's edge
	float foam = saturate(saturate((pattern + band * 1.3 - 1.0) * 2.5) * band + edgeLine * 0.5 * pattern) * ShoreFoam.y;
	float3 foamColor = (Ew_To_Linear(AmbientLight.rgb) + sun * (saturate(SunDirection.z) * l.shadow)) * 0.85;

	float3 h = normalize(SunDirection.xyz + toEye);
	float3 glint = l.specular + sun * (pow(saturate(dot(l.normal, h)), 48.0) * AmbientLight.w * l.shadow);

	float fresnel = l.fresnel;
	WaterLayers o;
	o.refraction = behind;
	o.behind = through * (1.0 - fresnel) * (1.0 - foam);
	float3 body = deep * (1.0 - through) * (1.0 - fresnel) + l.reflection * fresnel + glint;
	o.surface = lerp(body, foamColor, foam);
	o.surfaceCover = 1.0 - o.behind;
	return o;
}

// The refraction is already shrouded and fogged in the scene; the rest is shrouded and fogged here.
// The layers are linear with EwLinearLighting, so the fog mixes in linear space here too.
float3 Water_Combine(WaterLayers w, float3 shroud, float fogDistance)
{
	float3 surface = w.surface * Ew_To_Linear(shroud);
	[branch] if (WaterFog.x > 0.5)
	{
		float f = saturate((WaterFog.y - fogDistance) * WaterFog.z);
		surface = lerp(Ew_To_Linear(WaterFogColor.rgb) * w.surfaceCover, surface, f);
	}
	return Ew_To_Gamma(w.refraction * w.behind + surface);
}

// The average colour of the classic water texture, tinted by the vertex colour.
float3 Water_Tint(WaterPsIn i)
{
	return i.diffuse.rgb * tex2Dlod(Tex0, float4(i.uv01.xy, 0, 16)).rgb;
}

float4 WaterTrapezoidHqPS(WaterPsIn i) : COLOR
{
	WaterLight l = Water_Light_From(i, Water_Normal_Hq(i));
	float3 shroud = tex2D(Tex3, i.uv23.zw).rgb;
	WaterLayers w = Water_Layers(i, l, Water_Tint(i));
	return float4(Water_Combine(w, shroud, i.worldFog.w), 1.0);
}

// Rivers: the vertex colour is already shrouded; the shroud part of the vertex alpha shrouds the
// reflection, sun and foam. The alpha edge texture still feathers the banks.
float4 WaterRiverHqPS(WaterPsIn i) : COLOR
{
	WaterLight l = Water_Light_From(i, Water_Normal_Hq(i));
	float edge = tex2D(Tex3, i.uv01.zw).a;
	float shroud = saturate(i.diffuse.a / max(RiverOptions.x, 1.0 / 255.0));
	float3 tint = i.diffuse.rgb * tex2Dlod(Tex0, float4(i.uv01.xy, 0, 16)).rgb / max(shroud, 1.0 / 255.0);
	WaterLayers w = Water_Layers(i, l, tint);
	return float4(Water_Combine(w, shroud.xxx, i.worldFog.w), edge);
}
