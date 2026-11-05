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

// EastWind GPU skinning (GpuSkinning): each vertex of a rigid skin follows one bone of its HTree.
// The vertex shader gets only the bones a mesh uses, as a compact palette of 3 rows per bone
// (mesh.hlsl Bones). This part has no device, so it can be tested on its own.

#pragma once

#include <cstring>
#include <vector>

// Fills bones with the distinct bones of links in first-use order, and row_base with each
// vertex's first palette row (its palette index * 3). False, with both empty, when there are no
// links or more than max_bones distinct bones: that mesh deforms on the CPU.
inline bool Build_Skin_Palette(const unsigned short *links, int count, int max_bones,
	std::vector<unsigned short> &bones, std::vector<float> &row_base)
{
	bones.clear();
	row_base.clear();
	if (links == nullptr || count <= 0)
		return false;
	row_base.resize(size_t(count));
	for (int v = 0; v < count; ++v)
	{
		size_t k = 0;
		while (k < bones.size() && bones[k] != links[v])
			++k;
		if (k == bones.size())
		{
			if (int(k) >= max_bones)
			{
				bones.clear();
				row_base.clear();
				return false;
			}
			bones.push_back(links[v]);
		}
		row_base[size_t(v)] = float(k * 3);
	}
	return true;
}

// Writes the transform of each palette bone, 3 rows of 4 floats as Matrix3D stores them, to rows.
// transform(bone) returns the bone's Matrix3D, the one the CPU skin deforms with.
template <class TRANSFORM>
void Pack_Skin_Rows(const unsigned short *bones, int count, TRANSFORM transform, float *rows)
{
	for (int i = 0; i < count; ++i)
		memcpy(rows + i * 12, &transform(bones[i]), 12 * sizeof(float));
}
