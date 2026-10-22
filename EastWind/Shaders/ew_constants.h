/*
**	EastWind DX9 shaders: shared constant register layout.
**
**	Included by the HLSL sources (EastWind/Shaders/*.hlsl) and by the C++ code that sets
**	the constants, so both sides use the same register numbers.
**	HLSL uses EW_VS_REG / EW_PS_REG, C++ uses the plain numbers with Set*ShaderConstantF.
**
**	The parity ports (M3.0) keep the DX8 register layout of the shaders they replace,
**	so the C++ code that sets the constants did not have to change.
*/

#ifndef EW_CONSTANTS_H
#define EW_CONSTANTS_H

// fxc pastes the number only after the macro argument is expanded, hence two levels.
#define EW_REG_PASTE(n) register(c##n)
#define EW_REG(n) EW_REG_PASTE(n)

// --- Trees.hlsl (vertex), was Trees.nvv ---
#define EW_TREES_VS_WVP          4   // c4..c7: transposed world * view * projection
#define EW_TREES_VS_WAVE         8   // c8: no sway, c9.. sway type 1..EW_TREES_MAX_SWAY
#define EW_TREES_MAX_SWAY        10  // MAX_SWAY_TYPES in W3DTreeBuffer.h
#define EW_TREES_VS_SHROUD_OFS   32  // shroud texture offset
#define EW_TREES_VS_SHROUD_SCALE 33  // shroud texture scale

// --- wave.hlsl (vertex + pixel), was wave.nvv / wave.nvp ---
#define EW_WAVE_VS_WVP           2   // c2..c5: transposed world * view * projection
#define EW_WAVE_VS_TEXPROJ       6   // c6: xy scale, zw offset from clip space to reflection uv
#define EW_WAVE_PS_BUMPMAT       0   // c0: bump matrix (M00, M01, M10, M11), was D3DTSS_BUMPENVMAT*

// --- monochrome.hlsl (pixel), was monochrome.nvp ---
#define EW_MONO_PS_LUMINANCE     0   // c0: rgb weights for the grey value
#define EW_MONO_PS_COLOR         1   // c1: filter colour
#define EW_MONO_PS_FADE          2   // c2: blend factor between original and filtered image

#endif // EW_CONSTANTS_H
