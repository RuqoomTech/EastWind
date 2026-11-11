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
#include <vector>

#include "WW3D2/w3xparse.h"

namespace
{
std::string wrap(const std::string &body)
{
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<AssetDeclaration xmlns=\"uri:ea.com:eala:asset\">" + body + "</AssetDeclaration>";
}

bool parse(const std::string &xml, W3xFile &file, std::vector<std::string> &warnings, std::string &error)
{
	return W3x_Parse(xml.data(), xml.size(), file, error, warnings);
}

// A skin of one triangle with two vertex blocks. Vertex 1 is weighted 0.7 on bone 1 (block 0) and 0.3 on bone 2 (block 1).
const char SkinMesh[] = R"(
<W3DMesh id="TINY.SKIN" GeometryType="Skin" CastShadow="true" SortLevel="1">
	<Vertices><V X="1" Y="0" Z="0"/><V X="0" Y="1" Z="0"/><V X="0" Y="1" Z="0"/></Vertices>
	<Vertices><V X="0" Y="0" Z="0"/><V X="5" Y="5" Z="5"/><V X="0" Y="0" Z="0"/></Vertices>
	<Normals><N X="0" Y="0" Z="1"/><N X="0" Y="0" Z="1"/><N X="0" Y="0" Z="1"/></Normals>
	<Normals><N X="0" Y="0" Z="1"/><N X="0" Y="0" Z="1"/><N X="0" Y="0" Z="1"/></Normals>
	<TexCoords><T X="0" Y="0"/><T X="1" Y="0"/><T X="0" Y="1"/></TexCoords>
	<TexCoords><T X="0.5" Y="0.25"/><T X="0.5" Y="0.75"/><T X="1" Y="1"/></TexCoords>
	<BoneInfluences><I Bone="1" Weight="1"/><I Bone="1" Weight="0.7"/><I Bone="2" Weight="1"/></BoneInfluences>
	<BoneInfluences><I Bone="0" Weight="0"/><I Bone="2" Weight="0.3"/><I Bone="0" Weight="0"/></BoneInfluences>
	<Triangles><T><V>0</V><V>1</V><V>2</V><Nrm X="0" Y="0" Z="1"/><Dist>0</Dist></T></Triangles>
	<FXShader ShaderName="DefaultW3D.fx" TechniqueIndex="0"><Constants/></FXShader>
</W3DMesh>)";

std::string meshWithShader(const std::string &constants)
{
	return wrap(R"(<W3DMesh id="M.A"><Vertices><V X="0" Y="0" Z="0"/></Vertices><Normals><N X="0" Y="0" Z="1"/></Normals>
		<TexCoords><T X="0" Y="0"/></TexCoords>
		<Triangles><T><V>0</V><V>0</V><V>0</V></T></Triangles>
		<FXShader ShaderName="DefaultW3D.fx" TechniqueIndex="0"><Constants>)" + constants + "</Constants></FXShader></W3DMesh>");
}
} // namespace

TEST(W3xParse, KeepsLongPivotNames)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(wrap(R"(<W3DHierarchy id="A_HIERARCHY_WITH_A_LONG_NAME">
		<Pivot Name="ROOTTRANSFORM" Parent="-1"><Translation X="0" Y="0" Z="0"/><Rotation X="0" Y="0" Z="0" W="1"/></Pivot>
		<Pivot Name="BONE_WATER_POD_FRONT_RIGHT" Parent="0"><Translation X="1" Y="2" Z="3"/><Rotation X="0" Y="0" Z="0.5" W="0.5"/></Pivot>
	</W3DHierarchy>)"), file, warnings, error)) << error;
	ASSERT_EQ(file.hierarchies.size(), 1u);
	EXPECT_EQ(file.hierarchies[0].id, "A_HIERARCHY_WITH_A_LONG_NAME");
	ASSERT_EQ(file.hierarchies[0].pivots.size(), 2u);
	EXPECT_EQ(file.hierarchies[0].pivots[1].name, "BONE_WATER_POD_FRONT_RIGHT");
	EXPECT_EQ(file.hierarchies[0].pivots[1].parent, 0);
	EXPECT_FLOAT_EQ(file.hierarchies[0].pivots[1].translation.Z, 3.0f);
	EXPECT_FLOAT_EQ(file.hierarchies[0].pivots[1].rotation.W, 0.5f);
	EXPECT_TRUE(warnings.empty());
}

TEST(W3xParse, KeepsBothSkinBlocksAndSecondUvSet)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(wrap(SkinMesh), file, warnings, error)) << error;
	ASSERT_EQ(file.meshes.size(), 1u);
	const W3xMesh &mesh = file.meshes[0];
	EXPECT_EQ(mesh.geometryType, W3X_GEOMETRY_SKIN);
	EXPECT_TRUE(mesh.castShadow);
	EXPECT_FALSE(mesh.hidden);
	EXPECT_EQ(mesh.sortLevel, 1);
	ASSERT_EQ(mesh.blocks.size(), 2u);
	EXPECT_FLOAT_EQ(mesh.blocks[1].positions[1].X, 5.0f);
	ASSERT_EQ(mesh.blocks[0].bones.size(), 3u);
	EXPECT_EQ(mesh.blocks[0].bones[1], 1);
	EXPECT_EQ(mesh.blocks[1].bones[1], 2);
	EXPECT_FLOAT_EQ(mesh.blocks[0].weights[1], 0.7f);
	EXPECT_FLOAT_EQ(mesh.blocks[1].weights[1], 0.3f);
	ASSERT_EQ(mesh.uvSets.size(), 2u);
	EXPECT_FLOAT_EQ(mesh.uvSets[1][2].X, 1.0f);
	EXPECT_FLOAT_EQ(mesh.uvSets[1][0].Y, 0.25f);
	ASSERT_EQ(mesh.triangles.size(), 1u);
	EXPECT_EQ(mesh.triangles[0].v[2], 2u);
	EXPECT_TRUE(mesh.vertexColors.empty());

	// Vertex 1 keeps block 0 (the heavier), and the other block pulls it to another bone.
	bool blended = false;
	EXPECT_EQ(W3x_Heaviest_Block(mesh, 1, &blended), 0u);
	EXPECT_TRUE(blended);
	EXPECT_EQ(W3x_Heaviest_Block(mesh, 0, &blended), 0u);
	EXPECT_FALSE(blended);
}

TEST(W3xParse, HeaviestBlockFollowsTheWeight)
{
	std::string xml = SkinMesh;
	// Swap the weights of vertex 1: block 1 is now the heavier.
	xml.replace(xml.find("Weight=\"0.7\""), 12, "Weight=\"0.2\"");
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(wrap(xml), file, warnings, error)) << error;
	EXPECT_EQ(W3x_Heaviest_Block(file.meshes[0], 1), 1u);
}

TEST(W3xParse, KeepsVisibilityChannelAndBinaryMove)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(wrap(R"(<W3DAnimation id="LONG_ANIMATION_ID_OVER_FIFTEEN" Hierarchy="SKL" NumFrames="3">
		<Channels>
			<ChannelScalar Pivot="2" Type="Visibility" FirstFrame="0"><Frame>-1</Frame><Frame>-1</Frame><Frame>0</Frame></ChannelScalar>
			<ChannelScalar Pivot="1" Type="XTranslation" FirstFrame="0"><Frame>0</Frame><Frame BinaryMove="true">4</Frame><Frame>5</Frame></ChannelScalar>
			<ChannelQuaternion Pivot="1" Type="Orientation" FirstFrame="0"><Frame X="0" Y="0" Z="0" W="1"/><Frame X="0.5" Y="0" Z="0" W="0.5" BinaryMove="true"/></ChannelQuaternion>
			<ChannelScalar Pivot="1" Type="Scale" FirstFrame="0"><Frame>1</Frame></ChannelScalar>
		</Channels>
	</W3DAnimation>)"), file, warnings, error)) << error;
	ASSERT_EQ(file.animations.size(), 1u);
	const W3xAnimation &anim = file.animations[0];
	EXPECT_EQ(anim.id, "LONG_ANIMATION_ID_OVER_FIFTEEN");
	EXPECT_EQ(anim.frameRate, 30u); // the default
	ASSERT_EQ(anim.channels.size(), 3u);
	EXPECT_EQ(anim.channels[0].type, W3X_CHANNEL_VISIBILITY);
	EXPECT_EQ(anim.channels[0].pivot, 2u);
	ASSERT_EQ(anim.channels[0].values.size(), 3u);
	EXPECT_FLOAT_EQ(anim.channels[0].values[0], -1.0f);
	EXPECT_FLOAT_EQ(anim.channels[0].values[2], 0.0f);
	ASSERT_EQ(anim.channels[1].binaryMove.size(), 3u);
	EXPECT_FALSE(anim.channels[1].binaryMove[0]);
	EXPECT_TRUE(anim.channels[1].binaryMove[1]);
	EXPECT_EQ(anim.channels[2].type, W3X_CHANNEL_ORIENTATION);
	ASSERT_EQ(anim.channels[2].values.size(), 8u); // X, Y, Z, W per frame
	EXPECT_FLOAT_EQ(anim.channels[2].values[4], 0.5f);
	EXPECT_FLOAT_EQ(anim.channels[2].values[7], 0.5f);
	EXPECT_TRUE(anim.channels[2].binaryMove[1]);
	// An unknown channel type is skipped with a warning.
	ASSERT_EQ(warnings.size(), 1u);
	EXPECT_NE(warnings[0].find("Scale"), std::string::npos) << warnings[0];
}

TEST(W3xParse, KeepsPartialChannelRange)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(wrap(R"(<W3DAnimation id="A" Hierarchy="SKL" NumFrames="46" FrameRate="15">
		<Channels>
			<ChannelScalar Pivot="3" Type="ZTranslation" FirstFrame="1"><Frame>7</Frame><Frame>8</Frame></ChannelScalar>
		</Channels>
	</W3DAnimation>)"), file, warnings, error)) << error;
	const W3xAnimation &anim = file.animations.at(0);
	EXPECT_EQ(anim.numFrames, 46u);
	EXPECT_EQ(anim.frameRate, 15u);
	ASSERT_EQ(anim.channels.size(), 1u);
	EXPECT_EQ(anim.channels[0].type, W3X_CHANNEL_Z_TRANSLATION);
	EXPECT_EQ(anim.channels[0].firstFrame, 1u);
	EXPECT_EQ(anim.channels[0].values.size(), 2u);
}

TEST(W3xParse, KeepsEveryShaderConstant)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(meshWithShader(R"(
		<Texture Name="DiffuseTexture"><Value>ABAirfield</Value></Texture>
		<Texture Name="DamagedTexture"><Value>ABBuilding_Holes</Value></Texture>
		<Float Name="ColorAmbient"><Value>0.25</Value><Value>0.5</Value><Value>0.75</Value></Float>
		<Float Name="Opacity"><Value>0.5</Value></Float>
		<Int Name="BlendMode"><Value>2</Value></Int>
		<Bool Name="CullingEnable"><Value>false</Value></Bool>
		<Bool Name="AlphaTestEnable"><Value>true</Value></Bool>)"), file, warnings, error)) << error;
	const W3xMaterial &mat = file.meshes.at(0).material;
	EXPECT_EQ(mat.shaderName, "DefaultW3D.fx");
	EXPECT_EQ(mat.techniqueIndex, 0);
	EXPECT_EQ(mat.constants.size(), 7u);
	EXPECT_EQ(mat.textures.at("DiffuseTexture"), "ABAirfield");
	EXPECT_EQ(mat.textures.at("DamagedTexture"), "ABBuilding_Holes");
	ASSERT_EQ(mat.floats.at("ColorAmbient").size(), 3u);
	EXPECT_FLOAT_EQ(mat.floats.at("ColorAmbient")[1], 0.5f);
	EXPECT_FLOAT_EQ(mat.floats.at("Opacity")[0], 0.5f);
	EXPECT_EQ(mat.ints.at("BlendMode"), 2);
	EXPECT_FALSE(mat.bools.at("CullingEnable"));
	EXPECT_TRUE(mat.bools.at("AlphaTestEnable"));
}

TEST(W3xParse, MapsShaderConstantsLikeTheConverter)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(meshWithShader(R"(
		<Texture Name="DiffuseTexture"><Value>ABAirfield</Value></Texture>
		<Float Name="Opacity"><Value>0.5</Value></Float>
		<Int Name="BlendMode"><Value>2</Value></Int>
		<Bool Name="CullingEnable"><Value>false</Value></Bool>
		<Bool Name="AlphaTestEnable"><Value>true</Value></Bool>)"), file, warnings, error)) << error;
	W3xShading shading;
	W3x_Map_Shader(file.meshes[0], true, shading, warnings);
	EXPECT_EQ(shading.textureId, "ABAirfield");
	EXPECT_TRUE(shading.twoSided);
	EXPECT_EQ(shading.shader.AlphaTest, W3DSHADER_ALPHATEST_ENABLE);
	EXPECT_EQ(shading.shader.SrcBlend, W3DSHADER_SRCBLENDFUNC_ONE);
	EXPECT_EQ(shading.shader.DestBlend, W3DSHADER_DESTBLENDFUNC_ONE);
	EXPECT_NEAR(shading.material.Opacity, 0.5f, 1e-6f);
	EXPECT_TRUE(warnings.empty());

	// No UVs: the texture is dropped with a warning.
	W3x_Map_Shader(file.meshes[0], false, shading, warnings);
	EXPECT_TRUE(shading.textureId.empty());
	EXPECT_EQ(warnings.size(), 1u);
}

TEST(W3xParse, SplitsIncludesAndKeepsContainersAndBoxes)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	ASSERT_TRUE(parse(wrap(R"(<Includes>
			<Include type="all" source="ART:tiny_tex.xml"/>
			<Include type="all" source="ART:Dir/tiny_skl.W3X"/>
		</Includes>
		<W3DCollisionBox id="TINY.BOX"><Center X="0" Y="0" Z="1"/><Extent X="2" Y="2" Z="2"/></W3DCollisionBox>
		<W3DContainer id="TINY" Hierarchy="TINY_SKL">
			<SubObject SubObjectID="BOX" BoneIndex="3"><RenderObject><CollisionBox>TINY.BOX</CollisionBox></RenderObject></SubObject>
		</W3DContainer>
		<W3DContainer id="TINY2" Hierarchy="TINY_SKL"/>)"), file, warnings, error)) << error;
	ASSERT_EQ(file.w3xIncludes.size(), 1u);
	EXPECT_EQ(file.w3xIncludes[0], "Dir/tiny_skl.W3X");
	ASSERT_EQ(file.xmlIncludes.size(), 1u);
	EXPECT_EQ(file.xmlIncludes[0], "tiny_tex.xml");
	ASSERT_EQ(file.boxes.size(), 1u);
	EXPECT_FLOAT_EQ(file.boxes[0].extent.X, 2.0f);
	ASSERT_EQ(file.containers.size(), 2u);
	ASSERT_EQ(file.containers[0].subObjects.size(), 1u);
	EXPECT_EQ(file.containers[0].subObjects[0].renderObjectId, "TINY.BOX");
	EXPECT_TRUE(file.containers[0].subObjects[0].isBox);
	EXPECT_EQ(file.containers[0].subObjects[0].boneIndex, 3u);
}

TEST(W3xParse, RejectsBadDocuments)
{
	W3xFile file;
	std::vector<std::string> warnings;
	std::string error;
	EXPECT_FALSE(parse("<AssetDeclaration>", file, warnings, error));
	EXPECT_NE(error.find("XML error"), std::string::npos) << error;
	EXPECT_FALSE(parse("<Other/>", file, warnings, error));
	EXPECT_NE(error.find("not a W3X"), std::string::npos) << error;
	EXPECT_TRUE(parse(meshWithShader(""), file, warnings, error)) << error;
	std::string bad = meshWithShader("");
	bad.replace(bad.find("<V>0</V>"), 8, "<V>-1</V>");
	EXPECT_FALSE(parse(bad, file, warnings, error));
	EXPECT_NE(error.find("bad vertex indices"), std::string::npos) << error;
}

TEST(W3xParse, ParsesTextureDeclarations)
{
	const std::string xml = wrap(R"(<Texture id="ABAirfield" File="ABAirfield.tga"/><Texture id="Fx_Muzzle" File="fxmuzzle.dds"/>)");
	std::map<std::string, std::string> files;
	ASSERT_TRUE(W3x_Parse_Texture_Declarations(xml.data(), xml.size(), files));
	ASSERT_EQ(files.size(), 2u);
	EXPECT_EQ(files.at("abairfield"), "ABAirfield.tga");
	EXPECT_EQ(files.at("fx_muzzle"), "fxmuzzle.dds");
	EXPECT_FALSE(W3x_Parse_Texture_Declarations("<Other/>", 8, files));
}
