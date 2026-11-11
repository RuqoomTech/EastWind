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

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "WW3D2/meshmdl.h"
#include "WW3D2/w3xparse.h"

// The pure helpers of MeshModelClass::Load_W3X. The builder itself needs a device and is covered by the parity test.
TEST(W3xMesh, TextureFileFallsBackToTga)
{
	const std::map<std::string, std::string> none;
	EXPECT_EQ(W3x_Texture_File("ABAirfield", none), "ABAirfield.tga");
}

TEST(W3xMesh, TextureFileUsesDeclaredFileWithoutDirectory)
{
	std::map<std::string, std::string> files;
	files["abairfield"] = "Art/Textures/ABAirfield_D.dds";
	EXPECT_EQ(W3x_Texture_File("ABAirfield", files), "ABAirfield_D.dds");
	// An empty declaration is ignored.
	files["other"] = "";
	EXPECT_EQ(W3x_Texture_File("OTHER", files), "OTHER.tga");
}

TEST(W3xMesh, SecondaryBlendModeMapsToDetailFunction)
{
	EXPECT_EQ(W3x_Detail_Color_Func(0), ShaderClass::DETAILCOLOR_SCALE);
	EXPECT_EQ(W3x_Detail_Color_Func(1), ShaderClass::DETAILCOLOR_SCALE2X);
	EXPECT_EQ(W3x_Detail_Color_Func(2), ShaderClass::DETAILCOLOR_DETAILBLEND);
	EXPECT_EQ(W3x_Detail_Color_Func(3), ShaderClass::DETAILCOLOR_DETAILBLEND);
}

TEST(W3xMesh, AttributesFollowGeometryTypeAndFlags)
{
	W3xMesh mesh;
	EXPECT_EQ(W3x_Mesh_Attributes(mesh, false), (uint32)W3D_MESH_FLAG_GEOMETRY_TYPE_NORMAL);

	mesh.geometryType = W3X_GEOMETRY_SKIN;
	mesh.castShadow = true;
	mesh.hidden = true;
	const uint32 skin = W3x_Mesh_Attributes(mesh, true);
	EXPECT_EQ(skin & W3D_MESH_FLAG_GEOMETRY_TYPE_MASK, (uint32)W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN);
	EXPECT_NE(skin & W3D_MESH_FLAG_CAST_SHADOW, 0u);
	EXPECT_NE(skin & W3D_MESH_FLAG_HIDDEN, 0u);
	EXPECT_NE(skin & W3D_MESH_FLAG_TWO_SIDED, 0u);

	mesh.geometryType = W3X_GEOMETRY_CAMERA_ORIENTED;
	EXPECT_EQ(W3x_Mesh_Attributes(mesh, false) & W3D_MESH_FLAG_GEOMETRY_TYPE_MASK, (uint32)W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ORIENTED);
}
