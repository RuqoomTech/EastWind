// EastWind DX9: BGRA to RGBA swizzle for the profiler frame capture.
// DX8 assembled it from ps.1.4 text at runtime (three mask constants); ps_2_0 swizzles directly.

sampler2D Frame : register(s0);

float4 ProfilerSwizzlePS(float2 uv : TEXCOORD0) : COLOR
{
	return tex2D(Frame, uv).bgra;
}
