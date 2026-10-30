// EastWind DX9: per-pixel terrain lighting (M3.1c/M3.1d, EwPerPixelLighting).
//
// Replaces the terrain base pass shaders of terrain.hlsl (TerrainPS, TerrainNoise1PS,
// TerrainNoise2PS) for the main heightmap. EwTerrainLighting (W3DTerrainLighting.cpp) binds it
// right after TerrainShaderPixelShader::set has set the textures, so the samplers are the same:
// s0/s1 the two blended terrain tiles, s2/s3 the cloud and light-map textures.
//
// The CPU lights the terrain per vertex (BaseHeightMapRenderObjClass::doTheLight): ambient plus
// the sun lights (up to 3) against the grid normal, plus map and dynamic lights, clamped to 0..1.
// Here the sun part runs per pixel instead:
//   - A texture holds the grid normal of every height sample (n * 0.5 + 0.5, 8 bits per channel),
//     computed on the CPU with the same formula as updateVB. The vertex shader reads it at the
//     vertex (vertex texture fetch, point sampled), the pixel shader reads it bilinear between
//     the vertices. Both read the same texel at a vertex, so the vertex keeps the CPU colour
//     exactly, however coarse the stored normal is.
//   - The vertex shader takes the sun part, computed as the CPU did, back out of the vertex
//     colour. What is left ("extra") is the map and dynamic light (explosions), which stays
//     per vertex. The pixel shader adds the per-pixel sun light to it.
// With no extra light the result is the per-pixel version of the classic lighting, so the
// terrain looks the same, but smooth between vertices. ps_3_0 has no fixed-function fog, so
// the pixel shader does the linear fog.
//
// M3.1d: roads and the extra blend tiles (three-way blends) use the road shaders below. They
// replace RoadShader2Stage and RoadShaderPixelShader (RoadNoise2PS) in one pass. Their vertex
// colour is the CPU light of the nearest height sample (getStaticDiffuse), which is the texel
// the vertex shader reads, so the same split into sun and extra light works. Samplers: s0 the
// road or blend texture, s1/s2 the textures of stages 1 and 2 (cloud and light map), with the
// stage 1 and 2 texture matrices in TexMatrix[0..3] and [4..7].
//
// M3.4: the first sun light is darkened by the sun shadow map (shader_shadow.h), so units, buildings
// and trees cast on the terrain and roads. The terrain itself does not cast.

#include "shader_constants.h"
#include "shader_shadow.h"
#include "shader_linear.h"

float4 WorldViewProj[4]                : EW_REG(EW_TERRAIN_VS_WVP);
float4 World[3]                        : EW_REG(EW_TERRAIN_VS_WORLD);
float4 WorldView[3]                    : EW_REG(EW_TERRAIN_VS_WORLDVIEW);
float4 TexMatrix[8]                    : EW_REG(EW_TERRAIN_VS_TEXMATRIX);
float4 GridMap                         : EW_REG(EW_TERRAIN_VS_GRIDMAP);
float4 VsOptions                       : EW_REG(EW_TERRAIN_VS_OPTIONS);
float4 Sun[EW_TERRAIN_SUN_COUNT]       : EW_REG(EW_TERRAIN_SUN);	// [0] ambient, [1..3] direction to the light, [4..6] diffuse
float4 Fog                             : EW_REG(EW_TERRAIN_PS_FOG);	// x enable, y end, z 1 / (end - start)
float4 FogColor                        : EW_REG(EW_TERRAIN_PS_FOGCOLOR);
float4 Debug                           : EW_REG(EW_TERRAIN_PS_DEBUG);	// x per-vertex light (as terrain.hlsl), y light only

// Vertex sampler 0 (D3DVERTEXTEXTURESAMPLER0) and pixel sampler 4 hold the same grid normals.
// Each entry point uses only one of the two s0 samplers, so fxc accepts the shared register.
sampler2D VsGridNormals : register(s0);
sampler2D Tex0          : register(s0);
sampler2D Tex1          : register(s1);
sampler2D Tex2          : register(s2);
sampler2D Tex3          : register(s3);
sampler2D GridNormals   : register(s4);

// doTheLight without the map lights: ambient of the first light plus each sun light's diffuse.
// Unused lights have a black diffuse colour.
float3 Grid_Normal(float4 texel)
{
	return normalize(texel.xyz * 2.0 - 1.0);
}

// shadow scales the first light, the sun the shadow map is drawn from. linearLight (pixel
// shaders only) adds the colours up in linear space (shader_linear.h).
float3 Sun_Colour(int k, uniform bool linearLight)
{
	return linearLight ? Ew_To_Linear(Sun[k].rgb) : Sun[k].rgb;
}

float3 SunLight(float3 n, float shadow, uniform bool linearLight)
{
	float3 c = Sun_Colour(0, linearLight) + shadow * Sun_Colour(4, linearLight) * saturate(dot(Sun[1].xyz, n));
	[unroll] for (int k = 1; k < 3; ++k)
		c += Sun_Colour(4 + k, linearLight) * saturate(dot(Sun[1 + k].xyz, n));
	return saturate(c);
}

// The vertex shader's split of the CPU light, which was added up in gamma space.
float3 SunLight(float3 n)
{
	return SunLight(n, 1.0, false);
}

struct VsIn
{
	float4 position : POSITION0;
	float4 diffuse  : COLOR0;	// rgb CPU lighting, a tile blend
	float2 uv0      : TEXCOORD0;
	float2 uv1      : TEXCOORD1;
};

struct VsOut
{
	float4 position : POSITION;
	float4 diffuse  : COLOR0;
	float4 uv01     : TEXCOORD0;	// the two tile texture coordinates
	float4 uv23     : TEXCOORD1;	// cloud and light-map coordinates
	float4 gridFog  : TEXCOORD2;	// xy grid uv, w fog distance
	float3 extra    : TEXCOORD3;	// vertex light that is not the sun: map and dynamic lights
	float3 world    : TEXCOORD4;	// world position, for the shadow map
};

// The part of the vertex shader the terrain and road passes share: clip position, camera-space
// position, grid uv, the vertex light that is not the sun, and the fog distance.
struct LitVertex
{
	float4 position;
	float4 cameraSpace;
	float4 gridFog;
	float3 extra;
	float3 world;
};

LitVertex Light_Vertex(float3 position, float3 vertexLight)
{
	LitVertex o;
	float4 p = float4(position, 1.0);

	o.position = float4(dot(p, WorldViewProj[0]), dot(p, WorldViewProj[1]),
		dot(p, WorldViewProj[2]), dot(p, WorldViewProj[3]));

	o.world = float3(dot(p, World[0]), dot(p, World[1]), dot(p, World[2]));
	float2 worldXY = o.world.xy;
	o.cameraSpace = float4(dot(p, WorldView[0]), dot(p, WorldView[1]), dot(p, WorldView[2]), 1.0);

	float2 grid = worldXY * GridMap.xy + GridMap.zw;
	float3 n = Grid_Normal(tex2Dlod(VsGridNormals, float4(grid, 0.0, 0.0)));
	o.extra = vertexLight - SunLight(n);

	// D3D vertex fog: camera-space depth, or distance with D3DRS_RANGEFOGENABLE.
	float fogDistance = (VsOptions.x > 0.5) ? length(o.cameraSpace.xyz) : abs(o.cameraSpace.z);
	o.gridFog = float4(grid, 0.0, fogDistance);
	return o;
}

// Camera-space position through the texture matrix at TexMatrix[k], two outputs, as
// D3DTSS_TCI_CAMERASPACEPOSITION with D3DTTFF_COUNT2.
float2 Camera_Uv(float4 cs, int k)
{
	return float2(dot(cs, TexMatrix[k]), dot(cs, TexMatrix[k + 1]));
}

VsOut TerrainLitVS(VsIn i)
{
	VsOut o;
	LitVertex v = Light_Vertex(i.position.xyz, i.diffuse.rgb);
	o.position = v.position;
	o.diffuse = i.diffuse;
	o.uv01 = float4(i.uv0, i.uv1);
	// Stages 2 and 3 used camera-space position through a texture matrix, two outputs each.
	o.uv23 = float4(Camera_Uv(v.cameraSpace, 0), Camera_Uv(v.cameraSpace, 4));
	o.gridFog = v.gridFog;
	o.extra = v.extra;
	o.world = v.world;
	return o;
}

struct PsIn
{
	float4 diffuse : COLOR0;
	float4 uv01    : TEXCOORD0;
	float4 uv23    : TEXCOORD1;
	float4 gridFog : TEXCOORD2;
	float3 extra   : TEXCOORD3;
	float3 world   : TEXCOORD4;
};

// The per-pixel light: the sun against the filtered grid normal, shadowed, plus the vertex's
// extra light. With linear lighting the extra light is taken as a colour and decoded too.
float3 Pixel_Light(float2 grid, float3 world, float3 extra, float3 vertexLight)
{
	float3 n = Grid_Normal(tex2D(GridNormals, grid));
	float shadow = Ew_Sun_Shadow(world, n, Sun[1].xyz);
	float3 light = Ew_To_Gamma(saturate(SunLight(n, shadow, true) + Ew_To_Linear(extra)));
	// Debug keys: the vertex colour is the classic per-vertex light.
	if (Debug.x > 0.5)
		light = vertexLight;
	return light;
}

float4 Apply_Fog(float4 c, float distance)
{
	[branch] if (Fog.x > 0.5)
	{
		float f = saturate((Fog.y - distance) * Fog.z);
		c.rgb = lerp(FogColor.rgb, c.rgb, f);
	}
	return c;
}

float4 ShadeTerrain(PsIn i, int noiseTextures)
{
	float3 light = Pixel_Light(i.gridFog.xy, i.world, i.extra, i.diffuse.rgb);

	// As terrain.hlsl's Blend, with the per-pixel light in place of the vertex colour.
	float4 t = lerp(tex2D(Tex0, i.uv01.xy), tex2D(Tex1, i.uv01.zw), i.diffuse.a);
	float4 c = float4(t.rgb * light, t.a * i.diffuse.a);
	// Debug keys: the light alone, without textures, clouds or fog.
	if (Debug.y > 0.5)
		return float4(light, c.a);
	if (noiseTextures > 0)
		c *= tex2D(Tex2, i.uv23.xy);
	if (noiseTextures > 1)
		c *= tex2D(Tex3, i.uv23.zw);
	return Apply_Fog(c, i.gridFog.w);
}

float4 TerrainLitPS(PsIn i) : COLOR0       { return ShadeTerrain(i, 0); }
float4 TerrainLitNoise1PS(PsIn i) : COLOR0 { return ShadeTerrain(i, 1); }
float4 TerrainLitNoise2PS(PsIn i) : COLOR0 { return ShadeTerrain(i, 2); }

// --- Roads and extra blend tiles (M3.1d) ---

struct RoadVsIn
{
	float4 position : POSITION0;
	float4 diffuse  : COLOR0;	// rgb CPU light of the nearest height sample, a blend tile alpha
	float2 uv0      : TEXCOORD0;
};

struct RoadVsOut
{
	float4 position : POSITION;
	float4 diffuse  : COLOR0;
	float2 uv0      : TEXCOORD0;
	float4 uv12     : TEXCOORD1;	// stage 1 and 2 coordinates
	float4 gridFog  : TEXCOORD2;
	float3 extra    : TEXCOORD3;
	float3 world    : TEXCOORD4;
};

RoadVsOut RoadLitVS(RoadVsIn i)
{
	RoadVsOut o;
	LitVertex v = Light_Vertex(i.position.xyz, i.diffuse.rgb);
	o.position = v.position;
	o.diffuse = i.diffuse;
	o.uv0 = i.uv0;
	o.uv12 = float4(Camera_Uv(v.cameraSpace, 0), Camera_Uv(v.cameraSpace, 4));
	o.gridFog = v.gridFog;
	o.extra = v.extra;
	o.world = v.world;
	return o;
}

struct RoadPsIn
{
	float4 diffuse : COLOR0;
	float2 uv0     : TEXCOORD0;
	float4 uv12    : TEXCOORD1;
	float4 gridFog : TEXCOORD2;
	float3 extra   : TEXCOORD3;
	float3 world   : TEXCOORD4;
};

// As RoadShader2Stage (texture * diffuse, then each noise stage modulated in, alpha included)
// and RoadNoise2PS, with the per-pixel light in place of the vertex colour.
float4 ShadeRoad(RoadPsIn i, int noiseTextures)
{
	float3 light = Pixel_Light(i.gridFog.xy, i.world, i.extra, i.diffuse.rgb);
	float4 c = tex2D(Tex0, i.uv0) * float4(light, i.diffuse.a);
	// Debug keys: the light alone. Where the road covers the terrain it should not show.
	if (Debug.y > 0.5)
		return float4(light, c.a);
	if (noiseTextures > 0)
		c *= tex2D(Tex1, i.uv12.xy);
	if (noiseTextures > 1)
		c *= tex2D(Tex2, i.uv12.zw);
	return Apply_Fog(c, i.gridFog.w);
}

float4 RoadLitPS(RoadPsIn i) : COLOR0       { return ShadeRoad(i, 0); }
float4 RoadLitNoise1PS(RoadPsIn i) : COLOR0 { return ShadeRoad(i, 1); }
float4 RoadLitNoise2PS(RoadPsIn i) : COLOR0 { return ShadeRoad(i, 2); }
