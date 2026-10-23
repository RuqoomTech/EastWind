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

// --- trees.hlsl (vertex), was Trees.nvv ---
#define EW_TREES_VS_WVP          4   // c4..c7: transposed world * view * projection
#define EW_TREES_VS_WAVE         8   // c8: no sway, c9.. sway type 1..EW_TREES_MAX_SWAY
#define EW_TREES_MAX_SWAY        10  // MAX_SWAY_TYPES in W3DTreeBuffer.h
#define EW_TREES_VS_SHROUD_OFS   32  // shroud texture offset
#define EW_TREES_VS_SHROUD_SCALE 33  // shroud texture scale
// TreesLitPS (M3.1d, per-pixel lit trees); the vertex side uses the registers above.
#define EW_TREES_PS_LIGHT        0   // c0..c6: object ambient, 3 directions to the light, 3 diffuse colours
#define EW_TREES_PS_LIGHT_COUNT  7
#define EW_TREES_PS_OPTIONS      7   // c7: x shroud on, y light only (debug key)
#define EW_TREES_PS_COUNT        8

// --- wave.hlsl (vertex + pixel), was wave.nvv / wave.nvp ---
#define EW_WAVE_VS_WVP           2   // c2..c5: transposed world * view * projection
#define EW_WAVE_VS_TEXPROJ       6   // c6: xy scale, zw offset from clip space to reflection uv
#define EW_WAVE_PS_BUMPMAT       0   // c0: bump matrix (M00, M01, M10, M11), was D3DTSS_BUMPENVMAT*

// --- monochrome.hlsl (pixel), was monochrome.nvp ---
#define EW_MONO_PS_LUMINANCE     0   // c0: rgb weights for the grey value
#define EW_MONO_PS_COLOR         1   // c1: filter colour
#define EW_MONO_PS_FADE          2   // c2: blend factor between original and filtered image

// --- mesh.hlsl (vertex + pixel), M3.1 lit meshes and materials; set by ewmeshshader_dx9.cpp ---
// Matrices are uploaded as their columns, so the shader computes out.k = dot(v, c[k]).
#define EW_MESH_VS_WVP               0   // c0..c3: world * view * projection
#define EW_MESH_VS_WORLD             4   // c4..c6: world
#define EW_MESH_VS_WORLD_NORMAL      7   // c7..c9: inverse transpose of world (3x3)
#define EW_MESH_VS_WORLDVIEW         10  // c10..c12: world * view
#define EW_MESH_VS_WORLDVIEW_NORMAL  13  // c13..c15: inverse transpose of world * view (3x3)
#define EW_MESH_VS_TEXGEN            16  // c16..c17: per stage (source, uv set, transform count, projected)
#define EW_MESH_VS_TEXMATRIX         18  // c18..c25: per stage 4 registers, texture transform
#define EW_MESH_VS_OPTIONS           26  // c26: x range fog
#define EW_MESH_VS_COUNT             27

#define EW_MESH_MAX_LIGHTS           4
#define EW_MESH_PS_STAGE             0   // c0..c7: per stage colour op/args, colour modifiers, alpha op/args, alpha modifiers
#define EW_MESH_PS_TFACTOR           8   // c8: D3DRS_TEXTUREFACTOR
#define EW_MESH_PS_AMBIENT           9   // c9: D3DRS_AMBIENT
#define EW_MESH_PS_MATERIAL          10  // c10..c13: material diffuse, ambient, specular, emissive
#define EW_MESH_PS_MATSOURCE         14  // c14: colour source of the four material colours
#define EW_MESH_PS_OPTIONS           15  // c15: specular power, specular enable, stage 0/1 has a texture
#define EW_MESH_PS_FOG               16  // c16: enable, end, 1 / (end - start)
#define EW_MESH_PS_FOGCOLOR          17  // c17: D3DRS_FOGCOLOR
#define EW_MESH_PS_CAMERA            18  // c18: camera world position, w local viewer
#define EW_MESH_PS_VIEWDIR           19  // c19: world direction to the viewer without local viewer
#define EW_MESH_PS_LIGHTS            20  // c20..c39: EW_MESH_MAX_LIGHTS lights, 5 registers each
#define EW_MESH_PS_MATERIAL_OPTIONS  40  // c40: M3.1b normal map, SpecMap, house colour on, ground ambient scale
#define EW_MESH_PS_HOUSECOLOR        41  // c41: rgb house colour, w SpecMap specular power
#define EW_MESH_PS_COUNT             42

// --- terrain_lit.hlsl (vertex + pixel), M3.1c per-pixel terrain lighting; set by EwTerrainLighting_dx9.cpp ---
// Matrices are uploaded as their columns, like the mesh shader's.
#define EW_TERRAIN_VS_WVP            0   // c0..c3: world * view * projection
#define EW_TERRAIN_VS_WORLD          4   // c4..c6: world
#define EW_TERRAIN_VS_WORLDVIEW      7   // c7..c9: world * view
#define EW_TERRAIN_VS_TEXMATRIX      10  // c10..c17: texture matrices of stages 2 and 3, 4 registers each
#define EW_TERRAIN_VS_GRIDMAP        18  // c18: world xy -> height grid uv, xy scale, zw offset
#define EW_TERRAIN_VS_OPTIONS        19  // c19: x range fog
#define EW_TERRAIN_SUN               20  // c20..c26, vertex and pixel: ambient, 3 light directions, 3 diffuse colours
#define EW_TERRAIN_SUN_COUNT         7
#define EW_TERRAIN_VS_COUNT          27
#define EW_TERRAIN_PS_FOG            27  // c27: enable, end, 1 / (end - start)
#define EW_TERRAIN_PS_FOGCOLOR       28  // c28: D3DRS_FOGCOLOR
#define EW_TERRAIN_PS_DEBUG          29  // c29: debug keys, x per-vertex light, y light only
#define EW_TERRAIN_PS_COUNT          30  // the pixel constants start at EW_TERRAIN_SUN

// --- post.hlsl (pixel), M3.2 post-processing; set by EwPostProcess_dx9.cpp ---
#define EW_POST_PS_TEXEL         0   // c0: xy one source texel in uv, zw blur step in uv
#define EW_POST_PS_BLOOM         1   // c1: x threshold, y 1 / (1 - threshold), z strength
#define EW_POST_PS_BLOOM_WEIGHTS 2   // c2: xyz weights of the 1/4, 1/8 and 1/16 size levels
#define EW_POST_PS_COUNT         3

#endif // EW_CONSTANTS_H
