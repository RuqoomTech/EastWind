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

// EastWind DX9 graphics options: one switch per shader feature.
//
// W3DDisplay (DX9) fills them from Options.ini before the device is created; WW3D2 code reads
// them through EwGraphics(). Every option off gives the classic DX8 look, so a slow PC can turn
// single effects off. Anti-aliasing and anisotropic filtering are not here: the upstream
// Options.ini keys AntiAliasing, TextureFilter and AnisotropyLevel already drive them.

#pragma once

struct EwGraphicsOptions
{
	bool PerPixelLighting;	// EwPerPixelLighting: HLSL lighting for meshes and terrain (M3.1)
	bool NormalMaps;		// EwNormalMaps: normal maps in the M3.1 material (needs PerPixelLighting)
	int WaterQuality;		// EwWaterQuality: 0 classic, 1 waves and Fresnel, 2 adds refraction and shore (M3.3)
	int ShadowMapSize;		// EwShadowMapSize: 0 off (classic shadows), 1024, 2048 or 4096 (M3.4)
	bool Bloom;				// EwBloom (M3.2)
	bool ToneMapping;		// EwToneMapping (M3.2)
	bool ColorGrading;		// EwColorGrading (M3.2)
	bool FXAA;				// EwFXAA (M3.2)
	int AmbientOcclusion;	// EwAmbientOcclusion: 0 off, 1 at half resolution, 2 at full resolution (M3.5)
	bool Hdr;				// EwHdr: the post-processing scene texture is 16-bit float, so light adds up above white
	bool SoftParticles;		// EwSoftParticles: particles fade where they meet the scene (needs the AO depth texture's conditions)
	bool LinearLighting;	// EwLinearLighting: the shaders add light up in linear space (ew_linear.h)
	bool DebugKeys;			// EwDebugKeys: Ctrl+Shift+F1..F12 switch the features above while the game runs

	// Set by the debug keys (W3DDisplay), never saved. They only turn features off that
	// Options.ini turned on, so with the keys unused the game looks as Options.ini says.
	bool DebugAllOff;				// Ctrl+Shift+F9: classic look, every feature off
	bool DebugTerrainLightingOff;	// Ctrl+Shift+F10: terrain lit per vertex
	bool DebugMeshLightingOff;		// Ctrl+Shift+F11: meshes lit by fixed function
	bool DebugNormalMapsOff;		// Ctrl+Shift+F12: no normal maps or SpecMaps
	bool DebugTerrainLightOnly;		// Ctrl+Shift+F8: terrain shows its light only, no textures
	bool DebugPostOff;				// Ctrl+Shift+F7: no post-processing, the scene draws straight to the screen
	bool DebugToneMappingOff;		// Ctrl+Shift+F5: no tone mapping
	bool DebugColorGradingOff;		// Ctrl+Shift+F6: no colour grading
	bool DebugFxaaOff;				// Ctrl+Shift+F4: no FXAA
	bool DebugWaterOff;				// Ctrl+Shift+F3: classic water
	bool DebugShadowsOff;			// Ctrl+Shift+F2: classic shadows instead of the sun shadow map
	int DebugAoMode;				// Ctrl+Shift+F1 cycles: 0 ambient occlusion on, 1 the occlusion alone, 2 off
	bool DebugHdrOff;				// Ctrl+Shift+H: 8-bit scene texture
	bool DebugSoftParticlesOff;		// Ctrl+Shift+P: hard-edged particles
	bool DebugLinearLightingOff;	// Ctrl+Shift+L: light adds up in gamma space, as before

	EwGraphicsOptions()
		: PerPixelLighting(false)
		, NormalMaps(false)
		, WaterQuality(0)
		, ShadowMapSize(0)
		, Bloom(false)
		, ToneMapping(false)
		, ColorGrading(false)
		, FXAA(false)
		, AmbientOcclusion(0)
		, Hdr(false)
		, SoftParticles(false)
		, LinearLighting(false)
		, DebugKeys(false)
		, DebugAllOff(false)
		, DebugTerrainLightingOff(false)
		, DebugMeshLightingOff(false)
		, DebugNormalMapsOff(false)
		, DebugTerrainLightOnly(false)
		, DebugPostOff(false)
		, DebugToneMappingOff(false)
		, DebugColorGradingOff(false)
		, DebugFxaaOff(false)
		, DebugWaterOff(false)
		, DebugShadowsOff(false)
		, DebugAoMode(0)
		, DebugHdrOff(false)
		, DebugSoftParticlesOff(false)
		, DebugLinearLightingOff(false)
	{
	}

	// What the renderer uses: the Options.ini switch minus what the debug keys turned off.
	bool MeshLighting() const { return PerPixelLighting && !DebugAllOff && !DebugMeshLightingOff; }
	bool MeshNormalMaps() const { return NormalMaps && MeshLighting() && !DebugNormalMapsOff; }
	// The terrain shader also runs for the light-only view with per-vertex light.
	bool TerrainShader() const { return PerPixelLighting && !DebugAllOff && (!DebugTerrainLightingOff || DebugTerrainLightOnly); }
	bool TerrainPixelLight() const { return !DebugTerrainLightingOff; }
	// Post-processing (M3.2) runs when any post effect is on.
	bool PostProcessWanted() const { return Bloom || ToneMapping || ColorGrading || FXAA || AmbientOcclusion > 0 || SoftParticles; }
	bool PostProcess() const { return PostProcessWanted() && !DebugAllOff && !DebugPostOff; }
	bool PostToneMapping() const { return ToneMapping && !DebugToneMappingOff; }
	bool PostColorGrading() const { return ColorGrading && !DebugColorGradingOff; }
	bool PostFxaa() const { return FXAA && !DebugFxaaOff; }
	// Screen-space ambient occlusion (M3.5), a post pass from the scene's depth.
	bool PostAo() const { return AmbientOcclusion > 0 && DebugAoMode != 2; }
	bool PostAoOnly() const { return PostAo() && DebugAoMode == 1; }
	bool PostHdr() const { return Hdr && !DebugHdrOff; }
	// Soft particles read the scene depth, which the post-processing scene texture provides.
	bool PostSoftParticles() const { return SoftParticles && !DebugSoftParticlesOff; }
	bool Linear() const { return LinearLighting && !DebugAllOff && !DebugLinearLightingOff; }
	bool WaterShader() const { return WaterQuality >= 1 && !DebugAllOff && !DebugWaterOff; }
	// Sun shadow maps (M3.4) need the lit mesh and terrain shaders, which sample them.
	bool ShadowMaps() const { return ShadowMapSize > 0 && MeshLighting() && !DebugTerrainLightingOff && !DebugShadowsOff; }

	// Brings values read from Options.ini into range.
	void Validate()
	{
		if (WaterQuality < 0) WaterQuality = 0;
		if (WaterQuality > 2) WaterQuality = 2;
		if (AmbientOcclusion < 0) AmbientOcclusion = 0;
		if (AmbientOcclusion > 2) AmbientOcclusion = 2;
		if (ShadowMapSize != 0 && ShadowMapSize != 1024 && ShadowMapSize != 2048 && ShadowMapSize != 4096)
			ShadowMapSize = 0;
		if (!PerPixelLighting)
		{
			NormalMaps = false;
			ShadowMapSize = 0;
		}
	}
};

inline EwGraphicsOptions &EwGraphics()
{
	static EwGraphicsOptions options;
	return options;
}
