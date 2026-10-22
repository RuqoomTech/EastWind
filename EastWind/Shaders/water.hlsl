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
