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

#include <string>

#include "WWLib/RAMFILE.h"
#include "WWMath/aabox.h"
#include "WWMath/matrix3d.h"
#include "WW3D2/assetmgr.h"
#include "WW3D2/hanim.h"
#include "WW3D2/htree.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/vertmaterial.h"
#include "WW3D2/w3xconvert.h"

namespace
{
// The hierarchy, in an included file as RA3 skins keep it: bone 1 sits at (10, 0, 0), turned 90 degrees about Z,
// and bone 2 sits 2 along bone 1's X, turned 90 degrees about its X, so it is at (10, 2, 0) in the model.
const char SkeletonW3x[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<AssetDeclaration xmlns="uri:ea.com:eala:asset">
	<W3DHierarchy id="TINY_SKL">
		<Pivot Name="ROOTTRANSFORM" Parent="-1">
			<Translation X="0" Y="0" Z="0"/>
			<Rotation X="0" Y="0" Z="0" W="1"/>
		</Pivot>
		<Pivot Name="BONE" Parent="0">
			<Translation X="10" Y="0" Z="0"/>
			<Rotation X="0" Y="0" Z="0.70710678" W="0.70710678"/>
		</Pivot>
		<Pivot Name="TIP" Parent="1">
			<Translation X="2" Y="0" Z="0"/>
			<Rotation X="0.70710678" Y="0" Z="0" W="0.70710678"/>
		</Pivot>
	</W3DHierarchy>
</AssetDeclaration>)";

// A skin with one triangle: vertex 0 on bone 1, vertex 1 blended 70/30 between bones 1 and 0, vertex 2 on bone 2.
// Each Vertices block holds the vertex in the space of the matching BoneInfluences block's bone.
const char TinyW3x[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<AssetDeclaration xmlns="uri:ea.com:eala:asset">
	<Includes>
		<Include type="all" source="ART:tiny_tex.xml"/>
		<Include type="all" source="ART:tiny_skl.w3x"/>
	</Includes>
	<W3DMesh id="TINY.SKIN" GeometryType="Skin" CastShadow="true">
		<BoundingBox>
			<Min X="9" Y="0" Z="0"/>
			<Max X="10" Y="2" Z="1"/>
		</BoundingBox>
		<Vertices>
			<V X="1" Y="0" Z="0"/>
			<V X="0" Y="1" Z="0"/>
			<V X="0" Y="1" Z="0"/>
		</Vertices>
		<Vertices>
			<V X="0" Y="0" Z="0"/>
			<V X="5" Y="5" Z="5"/>
			<V X="0" Y="0" Z="0"/>
		</Vertices>
		<Normals>
			<N X="0" Y="0" Z="1"/>
			<N X="0" Y="0" Z="1"/>
			<N X="0" Y="0" Z="1"/>
		</Normals>
		<Normals>
			<N X="0" Y="0" Z="1"/>
			<N X="0" Y="0" Z="1"/>
			<N X="0" Y="0" Z="1"/>
		</Normals>
		<TexCoords>
			<T X="0" Y="0"/>
			<T X="1" Y="0"/>
			<T X="0" Y="1"/>
		</TexCoords>
		<BoneInfluences>
			<I Bone="1" Weight="1"/>
			<I Bone="1" Weight="0.7"/>
			<I Bone="2" Weight="1"/>
		</BoneInfluences>
		<BoneInfluences>
			<I Bone="0" Weight="0"/>
			<I Bone="0" Weight="0.3"/>
			<I Bone="0" Weight="0"/>
		</BoneInfluences>
		<Triangles>
			<T>
				<V>0</V>
				<V>1</V>
				<V>2</V>
				<Nrm X="0" Y="0" Z="1"/>
				<Dist>0</Dist>
			</T>
		</Triangles>
		<FXShader ShaderName="DefaultW3D.fx" TechniqueIndex="0">
			<Constants>
				<Float Name="ColorAmbient">
					<Value>0</Value>
					<Value>0</Value>
					<Value>0</Value>
				</Float>
				<Float Name="ColorEmissive">
					<Value>1</Value>
					<Value>1</Value>
					<Value>1</Value>
				</Float>
				<Float Name="Opacity">
					<Value>0.5</Value>
				</Float>
				<Bool Name="AlphaTestEnable">
					<Value>true</Value>
				</Bool>
			</Constants>
		</FXShader>
	</W3DMesh>
	<W3DCollisionBox id="TINY.BOX">
		<Center X="0" Y="0" Z="1"/>
		<Extent X="2" Y="2" Z="2"/>
	</W3DCollisionBox>
	<W3DContainer id="TINY" Hierarchy="TINY_SKL">
		<SubObject SubObjectID="SKIN" BoneIndex="0">
			<RenderObject>
				<Mesh>TINY.SKIN</Mesh>
			</RenderObject>
		</SubObject>
		<SubObject SubObjectID="BOX" BoneIndex="0">
			<RenderObject>
				<CollisionBox>TINY.BOX</CollisionBox>
			</RenderObject>
		</SubObject>
	</W3DContainer>
	<W3DAnimation id="TINY" Hierarchy="TINY_SKL" NumFrames="2" FrameRate="30">
		<Channels>
			<ChannelScalar Pivot="1" Type="XTranslation" FirstFrame="0">
				<Frame>0</Frame>
				<Frame>4</Frame>
			</ChannelScalar>
			<ChannelQuaternion Pivot="1" Type="Orientation" FirstFrame="0">
				<Frame X="0" Y="0" Z="0" W="1"/>
				<Frame X="0.70710678" Y="0" Z="0" W="0.70710678"/>
			</ChannelQuaternion>
		</Channels>
	</W3DAnimation>
</AssetDeclaration>)";

bool readSkeleton(const std::string &file, std::string &text)
{
	if (_stricmp(file.c_str(), "tiny_skl.w3x") != 0)
		return false;
	text = SkeletonW3x;
	return true;
}

void expectNear(const Vector3 &actual, const Vector3 &expected)
{
	EXPECT_NEAR(actual.X, expected.X, 1e-4f);
	EXPECT_NEAR(actual.Y, expected.Y, 1e-4f);
	EXPECT_NEAR(actual.Z, expected.Z, 1e-4f);
}

Vector3 transform(const Matrix3D &tm, const Vector3 &v)
{
	Vector3 out;
	Matrix3D::Transform_Vector(tm, v, &out);
	return out;
}

std::string replaceAll(std::string text, const std::string &from, const std::string &to)
{
	for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
		text.replace(at, from.size(), to);
	return text;
}
} // namespace

// The converted bytes go through the asset manager's own loaders, as a runtime W3X loader would feed them.
TEST(W3xConvert, EngineLoadsConvertedSkin)
{
	W3xConvertResult result;
	ASSERT_TRUE(Convert_W3X(TinyW3x, "", readSkeleton, result)) << result.Error;
	EXPECT_EQ(result.Name, "TINY");
	ASSERT_EQ(result.Warnings.size(), 1u);
	EXPECT_NE(result.Warnings[0].find("1 vertices were blended"), std::string::npos) << result.Warnings[0];

	WW3DAssetManager assets;
	RAMFileClass file(result.W3d.data(), static_cast<int>(result.W3d.size()));
	ASSERT_TRUE(assets.Load_3D_Assets(file));

	HTreeClass *tree = assets.Get_HTree("TINY");
	ASSERT_NE(tree, nullptr);
	ASSERT_EQ(tree->Num_Pivots(), 3);
	EXPECT_STREQ(tree->Get_Bone_Name(1), "BONE");
	tree->Base_Update(Matrix3D(true));

	// Skin vertices stay in the space of their bone; the bind pose puts them back at their model position.
	RenderObjClass *skin = assets.Create_Render_Obj("TINY.SKIN");
	ASSERT_NE(skin, nullptr);
	ASSERT_EQ(skin->Class_ID(), RenderObjClass::CLASSID_MESH);
	MeshModelClass *model = static_cast<MeshClass *>(skin)->Peek_Model();
	ASSERT_EQ(model->Get_Vertex_Count(), 3);
	EXPECT_EQ(model->Get_Polygon_Count(), 1);
	EXPECT_TRUE(model->Get_Flag(MeshGeometryClass::SKIN));
	const uint16 *bones = model->Get_Vertex_Bone_Links();
	EXPECT_EQ(bones[0], 1);
	EXPECT_EQ(bones[1], 1); // the heavier of its two bones
	EXPECT_EQ(bones[2], 2);
	const Vector3 *verts = model->Get_Vertex_Array();
	expectNear(verts[0], Vector3(1, 0, 0));
	expectNear(transform(tree->Get_Transform(bones[0]), verts[0]), Vector3(10, 1, 0));
	expectNear(transform(tree->Get_Transform(bones[1]), verts[1]), Vector3(9, 0, 0));
	// Bone 2's X turn takes its Y to Z, then bone 1's Z turn leaves Z alone.
	expectNear(transform(tree->Get_Transform(bones[2]), verts[2]), Vector3(10, 2, 1));
	// The converter's own bind pose gives the bounds, so they check its parent-chain math.
	AABoxClass box;
	model->Get_Bounding_Box(&box);
	expectNear(box.Center, Vector3(9.5f, 1, 0.5f));
	expectNear(box.Extent, Vector3(0.5f, 1, 0.5f));

	// DefaultW3D.fx colours become the vertex material; the missing ColorDiffuse keeps its default.
	VertexMaterialClass *material = model->Get_Single_Material();
	ASSERT_NE(material, nullptr);
	Vector3 color;
	material->Get_Ambient(&color);
	expectNear(color, Vector3(0, 0, 0));
	material->Get_Diffuse(&color);
	expectNear(color, Vector3(1, 1, 1));
	material->Get_Emissive(&color);
	expectNear(color, Vector3(1, 1, 1));
	EXPECT_NEAR(material->Get_Opacity(), 0.5f, 1e-4f);
	material->Release_Ref();
	skin->Release_Ref();

	RenderObjClass *hlod = assets.Create_Render_Obj("TINY");
	ASSERT_NE(hlod, nullptr);
	EXPECT_EQ(hlod->Class_ID(), RenderObjClass::CLASSID_HLOD);
	ASSERT_EQ(hlod->Get_Num_Sub_Objects(), 2);
	RenderObjClass *sub = hlod->Get_Sub_Object(0);
	EXPECT_STREQ(sub->Get_Name(), "TINY.SKIN");
	sub->Release_Ref();
	sub = hlod->Get_Sub_Object(1);
	EXPECT_STREQ(sub->Get_Name(), "TINY.BOX");
	EXPECT_EQ(sub->Class_ID(), RenderObjClass::CLASSID_OBBOX);
	sub->Release_Ref();
	hlod->Release_Ref();

	HAnimClass *anim = assets.Get_HAnim("TINY.TINY");
	ASSERT_NE(anim, nullptr);
	EXPECT_EQ(anim->Get_Num_Frames(), 2);
	Vector3 move;
	anim->Get_Translation(move, 1, 1.0f);
	expectNear(move, Vector3(4, 0, 0));
	Quaternion turn;
	anim->Get_Orientation(turn, 1, 1.0f);
	EXPECT_NEAR(turn.X, 0.70710678f, 1e-4f);
	EXPECT_NEAR(turn.Y, 0.0f, 1e-4f);
	EXPECT_NEAR(turn.Z, 0.0f, 1e-4f);
	EXPECT_NEAR(turn.W, 0.70710678f, 1e-4f);
	anim->Release_Ref();
}

TEST(W3xConvert, ShortensLongMeshNames)
{
	W3xConvertResult result;
	const std::string xml = replaceAll(TinyW3x, "TINY.SKIN", "TINY.SKIN_WITH_A_LONG_NAME");
	ASSERT_TRUE(Convert_W3X(xml, "", readSkeleton, result)) << result.Error;
	const std::string bytes(result.W3d.begin(), result.W3d.end());
	EXPECT_NE(bytes.find("TINY.SKIN_WITH_A_LON"), std::string::npos);
	EXPECT_EQ(bytes.find("SKIN_WITH_A_LONG"), std::string::npos);
}

TEST(W3xConvert, FailsOnLongModelName)
{
	W3xConvertResult result;
	EXPECT_FALSE(Convert_W3X(replaceAll(TinyW3x, "\"TINY\"", "\"TINY_IS_TOO_LONG\""), "", readSkeleton, result));
	EXPECT_NE(result.Error.find("TINY_IS_TOO_LONG"), std::string::npos) << result.Error;
	EXPECT_TRUE(result.W3d.empty());

	// A short name given by the user fixes it.
	EXPECT_TRUE(Convert_W3X(replaceAll(TinyW3x, "\"TINY\"", "\"TINY_IS_TOO_LONG\""), "TINY", readSkeleton, result)) << result.Error;
}

TEST(W3xConvert, FailsWithoutHierarchy)
{
	W3xConvertResult result;
	EXPECT_FALSE(Convert_W3X(TinyW3x, "", nullptr, result));
	EXPECT_NE(result.Error.find("TINY_SKL"), std::string::npos) << result.Error;
}

TEST(W3xConvert, FailsOnAnimationWithoutHierarchyOrFrames)
{
	W3xConvertResult result;
	EXPECT_FALSE(Convert_W3X(replaceAll(TinyW3x, " Hierarchy=\"TINY_SKL\"", ""), "", readSkeleton, result));
	EXPECT_NE(result.Error.find("names no hierarchy"), std::string::npos) << result.Error;

	EXPECT_FALSE(Convert_W3X(replaceAll(TinyW3x, "NumFrames=\"2\"", "NumFrames=\"0\""), "", readSkeleton, result));
	EXPECT_NE(result.Error.find("no frames"), std::string::npos) << result.Error;
}

// Sub-objects of different containers can share their last part; each still needs its own name.
TEST(W3xConvert, RenamesClashingShortNames)
{
	W3xConvertResult result;
	ASSERT_TRUE(Convert_W3X(replaceAll(TinyW3x, "TINY.BOX", "OTHER.SKIN"), "", readSkeleton, result)) << result.Error;

	WW3DAssetManager assets;
	RAMFileClass file(result.W3d.data(), static_cast<int>(result.W3d.size()));
	ASSERT_TRUE(assets.Load_3D_Assets(file));
	RenderObjClass *hlod = assets.Create_Render_Obj("TINY");
	ASSERT_NE(hlod, nullptr);
	ASSERT_EQ(hlod->Get_Num_Sub_Objects(), 2);
	RenderObjClass *sub = hlod->Get_Sub_Object(0);
	EXPECT_EQ(sub->Class_ID(), RenderObjClass::CLASSID_MESH);
	EXPECT_STREQ(sub->Get_Name(), "TINY.SKIN");
	sub->Release_Ref();
	sub = hlod->Get_Sub_Object(1);
	EXPECT_EQ(sub->Class_ID(), RenderObjClass::CLASSID_OBBOX);
	EXPECT_STREQ(sub->Get_Name(), "TINY.SKIN~1");
	sub->Release_Ref();
	hlod->Release_Ref();
}

TEST(W3xConvert, FailsOnBadTriangleIndex)
{
	W3xConvertResult result;
	EXPECT_FALSE(Convert_W3X(replaceAll(TinyW3x, "<V>1</V>", "<V>-1</V>"), "", readSkeleton, result));
	EXPECT_NE(result.Error.find("bad vertex indices"), std::string::npos) << result.Error;
}
