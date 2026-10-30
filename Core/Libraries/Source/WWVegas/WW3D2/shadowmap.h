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

// EastWind DX9: sun shadow map (M3.4, Options.ini ShadowMapSize = 1024, 2048 or 4096).
//
// Once a frame, before the water reflection and the main scene, W3DDisplay calls Render(). It
// fits an orthographic camera looking along the sun to the ground the main camera sees and
// draws the depth of the shadow casters into a depth texture:
//   - RTS3DScene::Render sees Caster_Pass() and draws only units, buildings and props (meshes
//     and HLods) that are opaque, not hidden and inside the light's box; sorted (transparent)
//     triangles are thrown away. W3DTreeBuffer draws the trees with their sway, alpha tested.
//   - MeshShader stays out of the way (fixed function, no colour writes).
// Then the map is bound to sampler s12 and the constants c200..c205 for the rest of the frame,
// and every lit pixel shader (meshes, terrain, roads, trees, water) darkens the sun light by
// Sun_Shadow (EastWind/Shaders/shader_shadow.h).
//
// While the map is active the classic shadows are off: stencil volumes, and the projected and
// decal object shadows. Art decals (scorch marks, terrain decals) and cloud shadows stay.

#pragma once

class SceneClass;
class CameraClass;
class Vector3;

class ShadowMap
{
public:
	// Start of a frame: last frame's numbers become the status, and the map is unbound until
	// Render() draws a new one.
	static void New_Frame();

	// Draws the shadow map for this frame and binds it. view is the main camera, sun_travel the
	// direction the sunlight travels (the map's first light, pointing down), ground_min and
	// ground_max the heights of the terrain the camera sees. Does nothing when
	// Get_Graphics_Options().ShadowMaps() is off.
	static void Render(SceneClass *scene, CameraClass *view, const Vector3 &sun_travel, float ground_min, float ground_max);

	// True while Render() draws the casters.
	static bool Caster_Pass();

	// In the caster pass: true if a bounding sphere touches the light's box. The light camera's
	// own frustum cannot tell, since CameraClass builds it as a perspective frustum.
	static bool Caster_Visible(const Vector3 &center, float radius);
	// In the caster pass: counts a drawn caster for the status.
	static void Count_Caster();

	// True when this frame's map is drawn and bound. The classic shadows are skipped then.
	static bool Active();

	// True when the direction to a light (unit length) is this frame's sun, so the light's
	// diffuse is shadowed.
	static bool Is_Sun(float x, float y, float z);

	// Status of the last finished frame for the debug key text: true and the numbers if it drew
	// a map, otherwise false and why not.
	static bool Last_Frame(int *size, const char **format, unsigned *casters, float *box, const char **why_not);

	// Frees the device resources (device reset, shutdown). They are made again when needed.
	static void Release();
};
