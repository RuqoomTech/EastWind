/*
**	EastWind: Command & Conquer Generals Zero Hour engine fork
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// EastWind DX9: per-pixel lighting for lit meshes (M3.1a, Options.ini EwPerPixelLighting).
//
// DX8Wrapper::Draw and Draw_Sorting_IB_VB call Begin() right before DrawIndexedPrimitive.
// When the draw is a fixed-function lit mesh that mesh.hlsl can reproduce, Begin() binds the
// mesh shaders on the device with constants taken from the device state (transforms, lights,
// material, texture stages, fog) and returns true; End() unbinds them after the draw.
//
// Any state the shader does not reproduce leaves the draw to fixed-function, and the reason is
// logged once. Draws that bring their own shaders are left alone.
// Unlit draws (lighting off: lasers, lines, decals, particles) get mesh.hlsl UnlitPS, which runs
// the texture stages on the vertex colours as fixed function does; screen-space draws (the UI)
// stay fixed function. They follow
// EwPerPixelLighting; with EwDebugKeys, Ctrl+Shift+U leaves them to fixed function.
// Binding only around one draw means code that drives the device directly (terrain, water,
// shadows) never sees these shaders, and the wrapper's shader caches stay correct.
//
// M3.1b (Options.ini EwNormalMaps): RA3-style material maps. For a diffuse texture "name.tga"
// (or .dds), "name_nrm" is a tangent-space normal map and "name_spm" a SpecMap (R specular,
// G glow, B house colour); each is optional, looked up once (.dds, then .tga) and cached.
// The maps are found the first time a texture is drawn on stage 0 of a lit draw, and the
// texture keeps them (TextureClass::Ew_Material_Maps), so they are freed with it. Begin() then
// uses the material pixel shader and binds the maps to samplers 2 and 3 for that draw only.
// The house colour is the object colour the game gives team-coloured objects
// (RenderObjClass::Get_ObjectColor). DX8TextureCategoryClass::Render puts it in the render
// state around each mesh, so sorted (transparent) draws replay it too. Textures the game
// already recolours on the CPU (ZHC*) keep that and ignore the SpecMap's house colour mask.
//
// Soft particles: Begin() also binds mesh.hlsl ParticleVS/ParticlePS for unlit billboard particles
// while EwPostProcess has the scene depth bound, so they fade where they meet the scene.

#pragma once

class TextureClass;
class RenderObjClass;

class EwMeshShader
{
public:
	// Binds the mesh shaders for the next draw if this draw qualifies.
	// Returns true if it did; the caller then calls End() after the draw.
	static bool Begin();
	static void End();

	// Between these, End() leaves the shaders bound for the next draw, and Begin() unbinds them
	// before a fixed-function one. Only around code whose every draw goes through Begin()
	// (the mesh and sorting renderers' flushes): other code would draw with our shaders.
	// Ctrl+Shift+T (EwDebugKeys): per-frame averages since the last call. SetupMs is the CPU time in
	// Begin(); ShaderDrawMs runs from Begin() to End() of the shader draws, so it includes the draw call.
	struct StatsStruct
	{
		float Calls, LitDraws, UnlitDraws, SetupMs, ShaderDrawMs;
		StatsStruct() : Calls(0), LitDraws(0), UnlitDraws(0), SetupMs(0), ShaderDrawMs(0) {}
	};
	static StatsStruct Take_Stats();

	static void Begin_Batch();
	static void End_Batch();

	// Sets the house colour for the draws of object (a mesh) until Clear_Object(). The colour
	// is taken from the first container that has one.
	static void Set_Object(const RenderObjClass *object);
	static void Clear_Object();

	// Soft particles (EwSoftParticles): EwPostProcess bound the scene's view depth on
	// EW_PARTICLE_DEPTH_SAMPLER, and these are 1 / its size; 0, 0 when it is unbound. While it is
	// bound, billboard particles (DX8Wrapper::Set_Ew_Soft_Particle) draw with the particle shaders.
	static void Set_Scene_Depth(float inv_width, float inv_height);

	// Forgets which map files exist. Called from DX8Wrapper::Shutdown.
	static void Shutdown();
};
