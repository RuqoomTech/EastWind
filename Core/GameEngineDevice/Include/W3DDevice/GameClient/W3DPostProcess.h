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

// EastWind DX9: post-processing (M3.2, Options.ini Bloom, ToneMapping, ColorGrading, FXAA;
// M3.5 AmbientOcclusion; Hdr).
//
// W3DView::draw calls Begin_Scene() before the 3D scene and End_Scene() after it (and after the
// view filters). In between, the 3D scene draws into a scene texture the size and format of the
// back buffer, which shares the back buffer's depth and stencil (with ambient occlusion on, it has
// its own; see below). End_Scene() then draws the
// scene texture onto the back buffer with the post passes (post.hlsl). The 2D scene, the UI and
// the mouse draw on the back buffer afterwards, so post effects never touch them.
//
// With multisampling the scene draws into a multisampled surface, which End_Scene() resolves into
// the scene texture first.
//
// HDR (Hdr): the scene texture and the bloom textures are 16-bit float (A16B16G16R16F) in
// place of the back buffer's format, so additive light (fire, explosions, muzzle flashes) adds up
// above white instead of clipping. The values keep the 8-bit scene's gamma encoding, so every
// draw and blend works as before; tone mapping then shows scene value 2 as white. Without the
// format (it must take blending and filtered reads) the scene stays 8-bit.
//
// Without a post effect that changes the picture, the last pass is a plain copy, so the game
// looks exactly as without the scene texture (M3.2a).
//
// Bloom (M3.2b, Bloom): the bright parts of the scene are averaged down to 1/4 size, blurred
// there and at 1/8 and 1/16 size, and the three blurred sizes are added back over the scene.
// The bloom textures have no depth buffer; the passes bind none while they draw.
//
// Tone mapping (M3.2c, ToneMapping): after bloom, a filmic curve (the ACES fit) on linear
// light gives deeper shadows and softer highlights. It is scaled so that white stays white.
//
// Colour grading (M3.2c, ColorGrading): last, each colour is looked up in a 16x16x16 colour
// LUT, stored as a 256x16 TGA strip of 16 slices (blue picks the slice, red goes across, green
// goes down). The map's own ColorGrading.tga (in the map's folder) wins over
// Art\Textures\ColorGrading.tga; without either, a neutral LUT changes nothing.
//
// Ambient occlusion (M3.5, AmbientOcclusion = 1 half size, 2 full size): the scene draws with
// its own depth buffer, an INTZ depth texture (with stencil) the size of the scene texture, in
// place of the back buffer's. End_Scene() reads it, rebuilds each pixel's camera-space position
// from the 3D camera and draws the occlusion into an AO texture (G16R16F: occlusion and depth).
// It is blurred across and down without crossing depth edges, and the final pass darkens the
// scene by it before bloom. Everything that writes depth takes part, so the water surface and
// the trees occlude too; smoke and other particles write no depth and get the occlusion of what
// lies behind them. A multisampled depth buffer cannot be read, so AO needs AntiAliasing = 0.
//
// Ambient occlusion, bloom, tone mapping and colour grading run in one final pass (PS_POST_FINAL).
//
// FXAA (M3.2d, FXAA): last, an edge anti-aliasing pass on the finished colours. With other
// effects on, the final pass draws into a second texture the size of the scene texture, which
// FXAA reads; FXAA alone reads the scene texture.
//
// Soft particles (SoftParticles): the scene draws with the depth texture of ambient occlusion
// (so also needs AntiAliasing = 0). Right before the particles, Begin_Particles() copies its view
// depth into an R32F texture, since the bound depth buffer cannot be read at the same time, and
// binds that for MeshShader's particle shaders; End_Particles() unbinds it after them.
//
// Code that copies the screen during the 3D scene must read the current render target, not the
// back buffer: the DX9 copies of W3DShaderManager (view filters) and W3DSmudgeManager (heat haze)
// do.

#pragma once

class CameraClass;

class W3DPostProcess
{
public:
	// For the debug status: the scene texture itself, then each effect of the final pass.
	enum Effect
	{
		SCENE,
		AMBIENT_OCCLUSION,
		BLOOM,
		TONE_MAPPING,
		COLOR_GRADING,
		FXAA,
		HDR,
		SOFT_PARTICLES,
		EFFECT_COUNT
	};

	// Points rendering at the scene texture and clears its colour to black, with the alpha the
	// frame clear uses (the soft water edge reads destination alpha). Depth is not cleared when it
	// is the back buffer's, which the frame clear already cleared; the ambient occlusion depth
	// texture is cleared here. Returns true if rendering now goes to the scene texture; then
	// End_Scene() must follow.
	static bool Begin_Scene(float clear_alpha);

	// Resolves the scene, points rendering back at the back buffer and draws the post passes over
	// the view rectangle (in back buffer pixels). camera is the 3D camera the scene drew with; the
	// ambient occlusion needs it. Does nothing if Begin_Scene() returned false.
	static void End_Scene(int x, int y, int width, int height, const CameraClass *camera);

	// Around the particles of the 3D scene (W3DScene): binds the scene's view depth for the soft
	// particle shaders, if the scene draws with the depth texture. camera is the 3D camera.
	static void Begin_Particles(const CameraClass *camera);
	static void End_Particles();

	// Frees the scene texture and surfaces. Called from W3DShaderManager::shutdown, which also runs
	// before a device reset; Begin_Scene() creates them again when needed.
	static void Release();

	// For the debug keys (DebugKeys): W3DDisplay::draw calls New_Frame() once a frame.
	static void New_Frame();
	static unsigned Last_Frame_Passes(Effect effect);	// passes the effect drew in the last frame
	static const char *Last_Reason(Effect effect);		// why the effect did not run, if it did not
	static const char *Color_Lut_File();				// the colour LUT file in use, or nullptr for the neutral one
};
