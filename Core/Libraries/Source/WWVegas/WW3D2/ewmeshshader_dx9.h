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
// logged once. Draws that bring their own shaders, and unlit draws, are left alone.
// Binding only around one draw means code that drives the device directly (terrain, water,
// shadows) never sees these shaders, and the wrapper's shader caches stay correct.

#pragma once

class EwMeshShader
{
public:
	// Binds the mesh shaders for the next draw if this draw qualifies.
	// Returns true if it did; the caller then calls End() after the draw.
	static bool Begin();
	static void End();
};
