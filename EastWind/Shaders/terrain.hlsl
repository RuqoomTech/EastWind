// EastWind DX9: terrain and road pixel shaders, ported from the ps.1.1 files in
// GeneralsMD/.../Shaders (terrain*.nvp, roadnoise2.nvp, fterrain*.nvp). They run on
// fixed-function vertex processing, so texture coordinate generation and fog are unchanged.
// The .pso files from the game data stay as the fallback.

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

// Two terrain tiles blended by vertex alpha, then lit by the vertex colour.
float4 Blend(PsIn i)
{
	return lerp(tex2D(Tex0, i.uv0), tex2D(Tex1, i.uv1), i.diffuse.a) * i.diffuse;
}

float4 TerrainPS(PsIn i) : COLOR       { return Blend(i); }
float4 TerrainNoise1PS(PsIn i) : COLOR { return Blend(i) * tex2D(Tex2, i.uv2); }
float4 TerrainNoise2PS(PsIn i) : COLOR { return Blend(i) * tex2D(Tex2, i.uv2) * tex2D(Tex3, i.uv3); }

// Road: t0 road, t1 cloud shadow, t2 noise.
float4 RoadNoise2PS(PsIn i) : COLOR
{
	return tex2D(Tex0, i.uv0) * tex2D(Tex1, i.uv1) * tex2D(Tex2, i.uv2) * i.diffuse;
}

// Flat terrain (multiply blend): t0 shroud, t1 terrain.
float4 FlatTerrainPS(PsIn i) : COLOR       { return tex2D(Tex1, i.uv1) * tex2D(Tex0, i.uv0) * i.diffuse; }
float4 FlatTerrain0PS(PsIn i) : COLOR      { return tex2D(Tex1, i.uv1) * i.diffuse; }
float4 FlatTerrainNoise1PS(PsIn i) : COLOR { return FlatTerrainPS(i) * tex2D(Tex2, i.uv2); }
float4 FlatTerrainNoise2PS(PsIn i) : COLOR { return FlatTerrainPS(i) * tex2D(Tex2, i.uv2) * tex2D(Tex3, i.uv3); }
