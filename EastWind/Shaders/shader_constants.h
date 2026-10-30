/*
**	EastWind DX9 shaders: shared constant register layout.
**
**	Included by the HLSL sources (EastWind/Shaders/*.hlsl) and by the C++ code that sets
**	the constants, so both sides use the same register numbers.
**	HLSL uses SHADER_REG, C++ uses the plain numbers with Set*ShaderConstantF.
**
**	The parity ports (M3.0) keep the DX8 register layout of the shaders they replace,
**	so the C++ code that sets the constants did not have to change.
*/

#ifndef SHADER_CONSTANTS_H
#define SHADER_CONSTANTS_H

// fxc pastes the number only after the macro argument is expanded, hence two levels.
#define SHADER_REG_PASTE(n) register(c##n)
#define SHADER_REG(n) SHADER_REG_PASTE(n)

// --- trees.hlsl (vertex), was Trees.nvv ---
#define TREES_VS_WVP          4   // c4..c7: transposed world * view * projection
#define TREES_VS_WAVE         8   // c8: no sway, c9.. sway type 1..TREES_MAX_SWAY
#define TREES_MAX_SWAY        10  // MAX_SWAY_TYPES in W3DTreeBuffer.h
#define TREES_VS_SHROUD_OFS   32  // shroud texture offset
#define TREES_VS_SHROUD_SCALE 33  // shroud texture scale
// TreesLitPS (M3.1d, per-pixel lit trees); the vertex side uses the registers above.
#define TREES_PS_LIGHT        0   // c0..c6: object ambient, 3 directions to the light, 3 diffuse colours
#define TREES_PS_LIGHT_COUNT  7
#define TREES_PS_OPTIONS      7   // c7: x shroud on, y light only (debug key)
#define TREES_PS_COUNT        8

// --- wave.hlsl (vertex + pixel), was wave.nvv / wave.nvp ---
#define WAVE_VS_WVP           2   // c2..c5: transposed world * view * projection
#define WAVE_VS_TEXPROJ       6   // c6: xy scale, zw offset from clip space to reflection uv
#define WAVE_PS_BUMPMAT       0   // c0: bump matrix (M00, M01, M10, M11), was D3DTSS_BUMPENVMAT*

// --- monochrome.hlsl (pixel), was monochrome.nvp ---
#define MONO_PS_LUMINANCE     0   // c0: rgb weights for the grey value
#define MONO_PS_COLOR         1   // c1: filter colour
#define MONO_PS_FADE          2   // c2: blend factor between original and filtered image

// --- mesh.hlsl (vertex + pixel), M3.1 lit meshes and materials; set by meshshader.cpp ---
// Matrices are uploaded as their columns, so the shader computes out.k = dot(v, c[k]).
#define MESH_VS_WVP               0   // c0..c3: world * view * projection
#define MESH_VS_WORLD             4   // c4..c6: world
#define MESH_VS_WORLD_NORMAL      7   // c7..c9: inverse transpose of world (3x3)
#define MESH_VS_WORLDVIEW         10  // c10..c12: world * view
#define MESH_VS_WORLDVIEW_NORMAL  13  // c13..c15: inverse transpose of world * view (3x3)
#define MESH_VS_TEXGEN            16  // c16..c17: per stage (source, uv set, transform count, projected)
#define MESH_VS_TEXMATRIX         18  // c18..c25: per stage 4 registers, texture transform
#define MESH_VS_OPTIONS           26  // c26: x range fog
#define MESH_VS_COUNT             27

#define MESH_MAX_LIGHTS           4
#define MESH_PS_STAGE             0   // c0..c7: per stage colour op/args, colour modifiers, alpha op/args, alpha modifiers
#define MESH_PS_TFACTOR           8   // c8: D3DRS_TEXTUREFACTOR
#define MESH_PS_AMBIENT           9   // c9: D3DRS_AMBIENT
#define MESH_PS_MATERIAL          10  // c10..c13: material diffuse, ambient, specular, emissive
#define MESH_PS_MATSOURCE         14  // c14: colour source of the four material colours
#define MESH_PS_OPTIONS           15  // c15: specular power, specular enable, stage 0/1 has a texture
#define MESH_PS_FOG               16  // c16: enable, end, 1 / (end - start)
#define MESH_PS_FOGCOLOR          17  // c17: D3DRS_FOGCOLOR
#define MESH_PS_CAMERA            18  // c18: camera world position, w local viewer
#define MESH_PS_VIEWDIR           19  // c19: world direction to the viewer without local viewer
#define MESH_PS_LIGHTS            20  // c20..c39: MESH_MAX_LIGHTS lights, 5 registers each
#define MESH_PS_MATERIAL_OPTIONS  40  // c40: M3.1b normal map, SpecMap, house colour on, ground ambient scale
#define MESH_PS_HOUSECOLOR        41  // c41: rgb house colour, w SpecMap specular power
#define MESH_PS_COUNT             42
// ParticleVS/ParticlePS (soft particles) use the WVP, world-view and options registers of the
// vertex side and the stage, texture factor, options and fog registers of the pixel side, plus:
#define PARTICLE_PS_SOFT          42  // c42: xy 1 / render target size, z 1 / fade distance (world units), w fade mode
#define PARTICLE_DEPTH_SAMPLER    13  // s13: view depth of the scene before the particles (R32F, W3DPostProcess)

// --- terrain_lit.hlsl (vertex + pixel), M3.1c per-pixel terrain lighting; set by W3DTerrainLighting.cpp ---
// Matrices are uploaded as their columns, like the mesh shader's.
#define TERRAIN_VS_WVP            0   // c0..c3: world * view * projection
#define TERRAIN_VS_WORLD          4   // c4..c6: world
#define TERRAIN_VS_WORLDVIEW      7   // c7..c9: world * view
#define TERRAIN_VS_TEXMATRIX      10  // c10..c17: texture matrices of stages 2 and 3, 4 registers each
#define TERRAIN_VS_GRIDMAP        18  // c18: world xy -> height grid uv, xy scale, zw offset
#define TERRAIN_VS_OPTIONS        19  // c19: x range fog
#define TERRAIN_SUN               20  // c20..c26, vertex and pixel: ambient, 3 light directions, 3 diffuse colours
#define TERRAIN_SUN_COUNT         7
#define TERRAIN_VS_COUNT          27
#define TERRAIN_PS_FOG            27  // c27: enable, end, 1 / (end - start)
#define TERRAIN_PS_FOGCOLOR       28  // c28: D3DRS_FOGCOLOR
#define TERRAIN_PS_DEBUG          29  // c29: debug keys, x per-vertex light, y light only
#define TERRAIN_PS_COUNT          30  // the pixel constants start at TERRAIN_SUN

// --- post.hlsl (pixel), M3.2 post-processing; set by W3DPostProcess.cpp ---
#define POST_PS_TEXEL         0   // c0: xy one source texel in uv, zw blur step in uv
#define POST_PS_BLOOM         1   // c1: x threshold, y 1 / (1 - threshold), z strength
#define POST_PS_BLOOM_WEIGHTS 2   // c2: xyz weights of the 1/4, 1/8 and 1/16 size levels
#define POST_PS_FINAL         3   // c3: final pass, x bloom on, y tone mapping on, z colour grading on, w ambient occlusion on
#define POST_PS_TONE          4   // c4: x exposure, y 1 / curve(exposure), so white stays white
#define POST_PS_CLAMP         5   // c5: FXAA and AO, xy lowest and zw highest uv inside the view rectangle
// Ambient occlusion (M3.5): the AO pass, its blur and the final pass read these.
#define POST_PS_DEPTH         6   // c6: view depth = x / (y - z * stored depth); w: no occlusion from this depth on
#define POST_PS_AO_RAY        7   // c7: view ray at depth 1 = uv * xy + zw (camera space, x right, y up)
#define POST_PS_AO            8   // c8: x radius (world units), y intensity, z horizon bias (cosine), w largest radius in uv
#define POST_PS_AO2           9   // c9: xy uv per world unit at depth 1, zw one AO texel in uv
#define POST_PS_AO_FINAL      10  // c10: x show the occlusion alone, y depth edge sharpness, z occlusion power
#define POST_PS_COUNT         11
// Samplers of the AO passes, above the bloom levels and the colour LUT (s1..s4).
#define POST_AO_SAMPLER       5   // s5: AO texture (R occlusion, G view depth)
#define POST_DEPTH_SAMPLER    6   // s6: the scene's INTZ depth texture

// --- water.hlsl WaterVS / WaterTrapezoidPS / WaterRiverPS, M3.3a water shader; set by W3DWaterShader.cpp ---
// Matrices are uploaded as their columns, like the terrain shader's.
#define WATER_VS_WVP          0   // c0..c3: world * view * projection
#define WATER_VS_WORLD        4   // c4..c6: world
#define WATER_VS_WORLDVIEW    7   // c7..c9: world * view
#define WATER_VS_NOISE        10  // c10..c11: stage 2 texture matrix (noise from camera-space position)
#define WATER_VS_SHROUD       12  // c12..c13: stage 3 texture matrix (shroud from camera-space position)
#define WATER_VS_OPTIONS      14  // c14: x range fog
#define WATER_VS_WAVES        15  // c15: scroll offsets of the two wave layers, xy and zw
#define WATER_VS_WAVE_SCALE   16  // c16: x/y world scale of the two layers, z river, w river tiling across
#define WATER_VS_COUNT        17
#define WATER_PS_SUN          0   // c0: direction to the sun
#define WATER_PS_SUN_COLOR    1   // c1: rgb sun colour, w specular power
#define WATER_PS_CAMERA       2   // c2: camera world position
#define WATER_PS_FRESNEL      3   // c3: x bias, y scale, z power, w most reflection
#define WATER_PS_OPTIONS      4   // c4: x distortion, y wave normal strength, z specular strength
#define WATER_PS_FOG          5   // c5: enable, end, 1 / (end - start)
#define WATER_PS_FOGCOLOR     6   // c6: D3DRS_FOGCOLOR
#define WATER_PS_SKY          7   // c7: rgb sky colour without a reflection, w has reflection
#define WATER_PS_RIVER        8   // c8: x river vertex alpha from the INI (the shroud scales it)
#define WATER_PS_WAVE_LOOK    9   // c9: x wave shading, y water texture wobble, z reflection opacity
#define WATER_PS_SCREEN       10  // c10: xy scale, zw offset from clip xy / w to the refraction uv (M3.3b)
#define WATER_PS_HEIGHT       11  // c11: xy scale, zw offset from world xy to the terrain height uv
#define WATER_PS_BODY         12  // c12: x height per texel value, y has heights, z shore wave phase, w refraction distortion
#define WATER_PS_ABSORB       13  // c13: rgb light absorbed per world unit of water, w deep colour scale
#define WATER_PS_FOAM         14  // c14: x foam depth, y foam strength, z caustic strength, w caustic fade depth
#define WATER_PS_AMBIENT      15  // c15: rgb ambient light, w broad sun glint strength
#define WATER_PS_WAVE_MIX     16  // c16: x swell size relative to layer A, y swell strength, z ripple strength
#define WATER_PS_COUNT        17

// --- shader_shadow.h (pixel), M3.4 sun shadow map; set once a frame by shadowmap.cpp ---
// Every lit pixel shader (mesh, terrain, roads, trees, water) includes shader_shadow.h. The registers
// sit far above all of their own constants, so the shadow constants stay bound for the frame.
#define SHADOW_PS_MATRIX      200 // c200..c202: world -> shadow map u, v and depth
#define SHADOW_PS_PARAMS      204 // c204: x on, y 1 / map size, z filter radius in texels, w normal offset (world units)
#define SHADOW_PS_PARAMS2     205 // c205: x hardware depth compare (else INTZ), y depth bias, w strength
#define SHADOW_PS_FIRST       200
#define SHADOW_PS_COUNT       6
#define SHADOW_SAMPLER        12  // s12, above every texture stage the wrapper and fixed function use

// --- shader_linear.h (pixel), frame-wide options; set once a frame by W3DDisplay ---
#define FRAME_PS_OPTIONS      206 // c206: x linear lighting on (LinearLighting)

#endif // SHADER_CONSTANTS_H
