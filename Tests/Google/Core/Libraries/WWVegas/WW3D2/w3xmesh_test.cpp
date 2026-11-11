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

static W3xHierarchy ThreePivotTree()
{
	W3xHierarchy tree;
	tree.pivots.resize(3);
	tree.pivots[0].parent = -1;
	tree.pivots[1].parent = 0;
	tree.pivots[2].parent = 0;
	return tree;
}

static W3xMesh TwoBlockSkin(float w0, float w1)
{
	W3xMesh mesh;
	mesh.id = "skin";
	mesh.geometryType = W3X_GEOMETRY_SKIN;
	mesh.blocks.resize(2);
	// Vertex 0 is split between bones 1 and 2; vertex 1 is all bone 1.
	mesh.blocks[0].positions = { Vector3(1, 0, 0), Vector3(5, 0, 0) };
	mesh.blocks[0].normals = { Vector3(0, 0, 1), Vector3(0, 0, 1) };
	mesh.blocks[0].bones = { 1, 1 };
	mesh.blocks[0].weights = { w0, 1.0f };
	mesh.blocks[1].positions = { Vector3(2, 0, 0), Vector3(6, 0, 0) };
	mesh.blocks[1].normals = { Vector3(0, 1, 0), Vector3(0, 1, 0) };
	mesh.blocks[1].bones = { 2, 1 };
	mesh.blocks[1].weights = { w1, 0.0f };
	return mesh;
}

TEST(W3xMesh, SecondInfluenceIsKept)
{
	const W3xHierarchy tree = ThreePivotTree();
	W3xMeshVertices out;
	std::string error;
	ASSERT_TRUE(W3x_Single_Bone_Vertices(TwoBlockSkin(0.75f, 0.25f), &tree, out, error)) << error;
	ASSERT_EQ(out.bones2.size(), 2u);
	EXPECT_EQ(out.bones2[0], 2);
	EXPECT_EQ(out.bones2[1], 1);
	EXPECT_FLOAT_EQ(out.weights2[0], 0.25f);
	EXPECT_FLOAT_EQ(out.weights2[1], 0.0f);
	EXPECT_FLOAT_EQ(out.positions2[0].X, 2.0f);
	EXPECT_FLOAT_EQ(out.positions2[1].X, 5.0f);
}

TEST(W3xMesh, UnblendedSkinHasNoSecondInfluence)
{
	const W3xHierarchy tree = ThreePivotTree();
	W3xMeshVertices out;
	std::string error;
	ASSERT_TRUE(W3x_Single_Bone_Vertices(TwoBlockSkin(1.0f, 0.0f), &tree, out, error)) << error;
	EXPECT_TRUE(out.bones2.empty());
	EXPECT_TRUE(out.weights2.empty());
}
