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

// EastWind DX9: post-processing (M3.2, Options.ini EwBloom, EwToneMapping, EwColorGrading, EwFXAA).
//
// W3DView::draw calls Begin_Scene() before the 3D scene and End_Scene() after it (and after the
// view filters). In between, the 3D scene draws into a scene texture the size and format of the
// back buffer, which shares the back buffer's depth and stencil. End_Scene() then draws the
// scene texture onto the back buffer with the post passes (post.hlsl). The 2D scene, the UI and
// the mouse draw on the back buffer afterwards, so post effects never touch them.
//
// With multisampling the scene draws into a multisampled surface, which End_Scene() resolves into
// the scene texture first.
//
// Without a post effect that changes the picture, the last pass is a plain copy, so the game
// looks exactly as without the scene texture (M3.2a).
//
// Bloom (M3.2b, EwBloom): the bright parts of the scene are averaged down to 1/4 size, blurred
// there and at 1/8 and 1/16 size, and the three blurred sizes are added back over the scene.
// The bloom textures have no depth buffer; the passes bind none while they draw.
//
// Code that copies the screen during the 3D scene must read the current render target, not the
// back buffer: the DX9 copies of W3DShaderManager (view filters) and W3DSmudgeManager (heat haze)
// do.

#pragma once

class EwPostProcess
{
public:
	// Points rendering at the scene texture and clears its colour to black, with the alpha the
	// frame clear uses (the soft water edge reads destination alpha). Depth is not cleared: it is
	// the back buffer's, which the frame clear already cleared. Returns true if rendering now goes
	// to the scene texture; then End_Scene() must follow.
	static bool Begin_Scene(float clear_alpha);

	// Resolves the scene, points rendering back at the back buffer and draws the post passes over
	// the view rectangle (in back buffer pixels). Does nothing if Begin_Scene() returned false.
	static void End_Scene(int x, int y, int width, int height);

	// Frees the scene texture and surfaces. Called from W3DShaderManager::shutdown, which also runs
	// before a device reset; Begin_Scene() creates them again when needed.
	static void Release();

	// For the debug keys (EwDebugKeys): W3DDisplay::draw calls New_Frame() once a frame.
	static void New_Frame();
	static unsigned Last_Frame_Passes();	// scenes drawn onto the back buffer in the last frame
	static const char *Last_Reason();		// why the scene texture was not used, if it was not
	static unsigned Last_Frame_Bloom_Passes();	// bloom passes drawn in the last frame
	static const char *Last_Bloom_Reason();		// why bloom did not run, if it did not
};
