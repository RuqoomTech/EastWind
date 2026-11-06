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

// EastWind W3X conversion: turns one RA3 W3X file (XML) into the bytes of one Zero Hour W3D file in
// memory, so the engine's own W3D loaders can read it. The w3x2w3d tool writes the bytes to disk; a
// runtime loader can hand them to WW3DAssetManager::Load_3D_Assets through a RAMFileClass.
//
// The W3D file holds, in load order: the hierarchy (copied from an included file when the W3X only
// references it), the meshes, the collision boxes, the container as an HLod and the animation. Every
// one of them is named after the model, so the file stands on its own: the HLod and the hierarchy are
// <model>, the meshes <model>.<mesh> and the animation <model>.<model>.

#pragma once

#include <functional>
#include <string>
#include <vector>

// Reads a file that a W3X Include names (the part after "ART:"). False when it cannot.
typedef std::function<bool(const std::string &file, std::string &text)> W3xIncludeReader;

struct W3xConvertResult
{
	std::string Name; // the model name: save the bytes as Name + ".w3d"
	std::vector<unsigned char> W3d;
	std::vector<std::string> Warnings;
	std::string Error; // why the conversion failed
};

// Converts one W3X document. A non-empty name replaces the model name, for W3X ids longer than the
// 15 characters that W3D names hold. False, with result.Error set, when the file cannot be converted.
bool Convert_W3X(const std::string &xml, const std::string &name, const W3xIncludeReader &read_include,
	W3xConvertResult &result);
