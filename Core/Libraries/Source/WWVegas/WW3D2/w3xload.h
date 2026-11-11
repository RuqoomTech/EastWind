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

// EastWind native W3X loader: builds engine objects from the structs of w3xparse.h without writing
// W3D bytes. The builders take final, full-length names from the caller and never touch the asset
// manager registries.

#pragma once

#include "w3xparse.h"

class RenderObjClass;

// A new oriented collision box with the state the converter's W3D box gives (white, oriented,
// physical and projectile collision, so it only draws when the box display mask asks for it).
// The caller owns the reference.
RenderObjClass *W3x_Create_Box(const W3xBox &box, const char *name);

// The file name without directories (or an "ART:" style prefix) and without its extension:
// "..\\Art\\W3D\\ABAirfield_L.w3x" and "ART:ABAirfield_L.w3x" both give "ABAirfield_L".
std::string W3x_File_Stem(const std::string &path);

// The registered name of a container, local hierarchy or animation. A file named after its model holds
// ids that are the model's (ABAirfield_L.w3x also says ABAIRFIELD), so the file stem replaces an id that
// equals the container's id (case-insensitive). Any other id stays.
std::string W3x_Asset_Name(const std::string &id, const std::string &containerId, const std::string &stem);
