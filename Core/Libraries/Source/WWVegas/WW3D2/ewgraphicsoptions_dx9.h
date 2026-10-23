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
	bool DebugKeys;			// EwDebugKeys: Ctrl+Shift+F8..F12 switch the features above while the game runs

	// Set by the debug keys (W3DDisplay), never saved. They only turn features off that
	// Options.ini turned on, so with the keys unused the game looks as Options.ini says.
	bool DebugAllOff;				// Ctrl+Shift+F9: classic look, every feature off
	bool DebugTerrainLightingOff;	// Ctrl+Shift+F10: terrain lit per vertex
	bool DebugMeshLightingOff;		// Ctrl+Shift+F11: meshes lit by fixed function
	bool DebugNormalMapsOff;		// Ctrl+Shift+F12: no normal maps or SpecMaps
	bool DebugTerrainLightOnly;		// Ctrl+Shift+F8: terrain shows its light only, no textures

	EwGraphicsOptions()
		: PerPixelLighting(false)
		, NormalMaps(false)
		, WaterQuality(0)
		, ShadowMapSize(0)
		, Bloom(false)
		, ToneMapping(false)
		, ColorGrading(false)
		, FXAA(false)
		, DebugKeys(false)
		, DebugAllOff(false)
		, DebugTerrainLightingOff(false)
		, DebugMeshLightingOff(false)
		, DebugNormalMapsOff(false)
		, DebugTerrainLightOnly(false)
	{
	}

	// What the renderer uses: the Options.ini switch minus what the debug keys turned off.
	bool MeshLighting() const { return PerPixelLighting && !DebugAllOff && !DebugMeshLightingOff; }
	bool MeshNormalMaps() const { return NormalMaps && MeshLighting() && !DebugNormalMapsOff; }
	// The terrain shader also runs for the light-only view with per-vertex light.
	bool TerrainShader() const { return PerPixelLighting && !DebugAllOff && (!DebugTerrainLightingOff || DebugTerrainLightOnly); }
	bool TerrainPixelLight() const { return !DebugTerrainLightingOff; }

	// Brings values read from Options.ini into range.
	void Validate()
	{
		if (WaterQuality < 0) WaterQuality = 0;
		if (WaterQuality > 2) WaterQuality = 2;
		if (ShadowMapSize != 0 && ShadowMapSize != 1024 && ShadowMapSize != 2048 && ShadowMapSize != 4096)
			ShadowMapSize = 0;
		if (!PerPixelLighting)
			NormalMaps = false;
	}
};

inline EwGraphicsOptions &EwGraphics()
{
	static EwGraphicsOptions options;
	return options;
}
