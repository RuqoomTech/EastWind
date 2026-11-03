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

// EastWind: the deformed vertices and normals of one skin for one pose frame
// (WW3D::Get_Pose_Frame). The shadow casters, the water reflection and the main view of a frame
// draw the same pose, so only the first of them deforms the skin; the others reuse it.

#pragma once

#include "WWMath/vector3.h"
#include <vector>

class SkinCacheClass
{
public:
	// True when loc and norm already hold the skin of model for frame. Otherwise the buffers are
	// keyed to it and sized for vertex_count, and the caller deforms into them.
	bool Fetch(unsigned frame, const void *model, int vertex_count, Vector3 *&loc, Vector3 *&norm)
	{
		const bool hit = Model == model && Frame == frame && Count == vertex_count;
		if (!hit)
		{
			// Grow only, so a skin allocates once.
			if (Buffer.size() < size_t(vertex_count) * 2)
				Buffer.resize(size_t(vertex_count) * 2);
			Model = model;
			Frame = frame;
			Count = vertex_count;
		}
		loc = Buffer.data();
		norm = Buffer.data() + vertex_count;
		return hit;
	}

	void Reset() { Model = nullptr; }

private:
	std::vector<Vector3> Buffer;
	const void *Model = nullptr;
	unsigned Frame = 0;
	int Count = 0;
};
