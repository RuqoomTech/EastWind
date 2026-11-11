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

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "WW3D2/boxrobj.h"
#include "WW3D2/hlod.h"
#include "WW3D2/hrawanim.h"
#include "WW3D2/htree.h"
#include "WW3D2/htreemgr.h"
#include "WW3D2/w3xload.h"
#include "WW3D2/w3xparse.h"

namespace
{
const char LongPivot[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 26 characters

W3xPivot pivot(const char *name, int parent, Vector3 translation)
{
	W3xPivot p;
	p.name = name;
	p.parent = parent;
	p.translation = translation;
	return p;
}

// Root, a bone at (1,2,3) and a child with the long name at (0,0,5) below it.
W3xHierarchy sampleTree()
{
	W3xHierarchy tree;
	tree.id = "A_VERY_LONG_HIERARCHY_NAME_28";
	tree.pivots.push_back(pivot("ROOTTRANSFORM", -1, Vector3(0, 0, 0)));
	tree.pivots.push_back(pivot("BONE01", 0, Vector3(1, 2, 3)));
	tree.pivots.push_back(pivot(LongPivot, 1, Vector3(0, 0, 5)));
	return tree;
}

W3xChannel channel(uint32 pivot, W3xChannelType type, uint32 first, std::vector<float> values)
{
	W3xChannel c;
	c.pivot = pivot;
	c.type = type;
	c.firstFrame = first;
	c.values = values;
	return c;
}
} // namespace

TEST(W3xBuild, HTreeKeepsLongNamesAndBindPose)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, source.id.c_str()));

	EXPECT_STREQ(tree.Get_Name(), "A_VERY_LONG_HIERARCHY_NAME_28");
	ASSERT_EQ(tree.Num_Pivots(), 3);
	EXPECT_EQ(tree.Get_Bone_Index("abcdefghijklmnopqrstuvwxyz"), 2);
	EXPECT_STREQ(tree.Get_Bone_Name(2), LongPivot);
	EXPECT_EQ(tree.Get_Parent_Index(2), 1);
	EXPECT_EQ(tree.Get_Parent_Index(0), 0); // the engine reports the root's parent as 0

	Matrix3D root;
	root.Make_Identity();
	tree.Base_Update(root);
	const Vector3 end = tree.Get_Transform(2).Get_Translation();
	EXPECT_NEAR(end.X, 1.0f, 1e-5f);
	EXPECT_NEAR(end.Y, 2.0f, 1e-5f);
	EXPECT_NEAR(end.Z, 8.0f, 1e-5f);
	EXPECT_TRUE(tree.Get_Visibility(2));
}

TEST(W3xBuild, HTreeRejectsBadParents)
{
	W3xHierarchy source = sampleTree();
	source.pivots[1].parent = 2; // a parent after its child
	HTreeClass tree;
	EXPECT_FALSE(tree.Load_W3X(source, "BAD"));
	EXPECT_EQ(tree.Num_Pivots(), 0);
}

TEST(W3xBuild, AddTreeRejectsDuplicateName)
{
	const W3xHierarchy source = sampleTree();
	std::unique_ptr<HTreeManagerClass> manager(new HTreeManagerClass);

	HTreeClass *first = new HTreeClass;
	ASSERT_TRUE(first->Load_W3X(source, "Tree"));
	ASSERT_TRUE(manager->Add_Tree(first));
	EXPECT_EQ(manager->Get_Tree("TREE"), first);

	HTreeClass *second = new HTreeClass;
	ASSERT_TRUE(second->Load_W3X(source, "TREE"));
	EXPECT_FALSE(manager->Add_Tree(second));
	delete second; // the caller deletes a rejected tree

	EXPECT_EQ(manager->Num_Trees(), 1);
}

TEST(W3xBuild, PartialChannelHoldsEdgeKeys)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, "T"));

	W3xAnimation anim;
	anim.id = "MOVE";
	anim.hierarchy = "T";
	anim.numFrames = 10;
	anim.frameRate = 30;
	anim.channels.push_back(channel(1, W3X_CHANNEL_X_TRANSLATION, 3, { 10.0f, 20.0f, 30.0f })); // frames 3 to 5

	HRawAnimClass raw;
	ASSERT_TRUE(raw.Load_W3X(anim, "T.MOVE", "T", &tree));
	EXPECT_STREQ(raw.Get_Name(), "T.MOVE");
	EXPECT_STREQ(raw.Get_HName(), "T");
	EXPECT_EQ(raw.Get_Num_Frames(), 10);
	EXPECT_EQ(raw.Get_Num_Pivots(), 3);
	EXPECT_TRUE(raw.Has_X_Translation(1));
	EXPECT_FALSE(raw.Has_Y_Translation(1));

	const struct { float frame; float x; } expected[] = {
		{ 0.0f, 10.0f }, { 2.0f, 10.0f }, { 3.0f, 10.0f }, { 4.0f, 20.0f }, { 5.0f, 30.0f }, { 6.0f, 30.0f }, { 9.0f, 30.0f },
	};
	for (const auto &e : expected) {
		Vector3 t;
		raw.Get_Translation(t, 1, e.frame);
		EXPECT_NEAR(t.X, e.x, 1e-4f) << "frame " << e.frame;
	}
}

TEST(W3xBuild, StepKeyHoldsPreviousValue)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, "T"));

	W3xAnimation anim;
	anim.id = "STEP";
	anim.hierarchy = "T";
	anim.numFrames = 4;
	anim.frameRate = 30;
	W3xChannel ch = channel(1, W3X_CHANNEL_X_TRANSLATION, 0, { 0.0f, 10.0f, 20.0f, 30.0f });
	ch.binaryMove = { false, false, true, false };
	anim.channels.push_back(ch);

	HRawAnimClass raw;
	ASSERT_TRUE(raw.Load_W3X(anim, "T.STEP", "T", &tree));

	const struct { float frame; float x; } expected[] = {
		{ 0.5f, 5.0f }, { 1.5f, 10.0f }, { 2.0f, 20.0f }, { 2.5f, 25.0f },
	};
	for (const auto &e : expected) {
		Vector3 t;
		raw.Get_Translation(t, 1, e.frame);
		EXPECT_NEAR(t.X, e.x, 1e-4f) << "frame " << e.frame;
	}
}

TEST(W3xBuild, StepKeyOnOrientation)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, "T"));

	const float s = 0.70710678f; // 90 degrees about Z
	W3xAnimation anim;
	anim.id = "TURN";
	anim.hierarchy = "T";
	anim.numFrames = 2;
	anim.frameRate = 30;
	W3xChannel ch = channel(1, W3X_CHANNEL_ORIENTATION, 0, { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, s });
	ch.binaryMove = { false, true };
	anim.channels.push_back(ch);

	HRawAnimClass raw;
	ASSERT_TRUE(raw.Load_W3X(anim, "T.TURN", "T", &tree));

	Quaternion q;
	raw.Get_Orientation(q, 1, 0.5f);
	EXPECT_NEAR(q.X, 0.0f, 1e-4f);
	EXPECT_NEAR(q.Y, 0.0f, 1e-4f);
	EXPECT_NEAR(q.Z, 0.0f, 1e-4f);
	EXPECT_NEAR(q.W, 1.0f, 1e-4f);
}

TEST(W3xBuild, NoBinaryMoveStillLerps)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, "T"));

	W3xAnimation anim;
	anim.id = "LERP";
	anim.hierarchy = "T";
	anim.numFrames = 4;
	anim.frameRate = 30;
	anim.channels.push_back(channel(1, W3X_CHANNEL_X_TRANSLATION, 0, { 0.0f, 10.0f, 20.0f, 30.0f }));

	HRawAnimClass raw;
	ASSERT_TRUE(raw.Load_W3X(anim, "T.LERP", "T", &tree));

	Vector3 t;
	raw.Get_Translation(t, 1, 1.5f);
	EXPECT_NEAR(t.X, 15.0f, 1e-4f);
}

TEST(W3xBuild, VisibilityChannelIsABitChannel)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, "T"));

	W3xAnimation anim;
	anim.id = "VIS";
	anim.hierarchy = "T";
	anim.numFrames = 8;
	anim.frameRate = 30;
	anim.channels.push_back(channel(2, W3X_CHANNEL_VISIBILITY, 2, { -1.0f, 0.0f })); // RA3 true is -1

	HRawAnimClass raw;
	ASSERT_TRUE(raw.Load_W3X(anim, "T.VIS", "T", &tree));
	EXPECT_TRUE(raw.Has_Visibility(2));
	EXPECT_TRUE(raw.Get_Visibility(2, 2.0f));
	EXPECT_FALSE(raw.Get_Visibility(2, 3.0f));
	EXPECT_TRUE(raw.Get_Visibility(2, 0.0f)); // before the range
	EXPECT_TRUE(raw.Get_Visibility(2, 6.0f)); // after the range
	EXPECT_TRUE(raw.Get_Visibility(1, 3.0f)); // no channel
}

TEST(W3xBuild, ChannelOnMissingPivotIsDropped)
{
	const W3xHierarchy source = sampleTree();
	HTreeClass tree;
	ASSERT_TRUE(tree.Load_W3X(source, "T"));

	W3xAnimation anim;
	anim.id = "BAD";
	anim.hierarchy = "T";
	anim.numFrames = 4;
	anim.frameRate = 30;
	anim.channels.push_back(channel(9, W3X_CHANNEL_Z_TRANSLATION, 0, { 1.0f, 2.0f }));

	HRawAnimClass raw;
	ASSERT_TRUE(raw.Load_W3X(anim, "T.BAD", "T", &tree));
	for (int i = 0; i < 3; i++) {
		EXPECT_FALSE(raw.Is_Node_Motion_Present(i));
	}
}

TEST(W3xBuild, HLodDefTakesNamesAndBones)
{
	HLodDefClass def;
	const std::vector<std::pair<std::string, int>> subs = { { "MODEL.BODY", 0 }, { "MODEL.TURRET", 2 } };
	ASSERT_TRUE(def.Load_W3X("MODEL", "MODEL", subs));
	EXPECT_STREQ(def.Get_Name(), "MODEL");
}

TEST(W3xBuild, BoxHasLongNameAndCollisionBits)
{
	W3xBox box;
	box.id = "BOX";
	box.center = Vector3(1, 2, 3);
	box.extent = Vector3(4, 5, 6);
	const char *name = "A_BOX_NAME_LONGER_THAN_THIRTY_THREE_CHARS";

	RenderObjClass *obj = W3x_Create_Box(box, name);
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ(obj->Class_ID(), RenderObjClass::CLASSID_OBBOX);
	EXPECT_STREQ(obj->Get_Name(), name);

	W3dBoxStruct def;
	memset(&def, 0, sizeof(def));
	def.Attributes = W3D_BOX_ATTRIBUTE_ORIENTED | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PHYSICAL | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PROJECTILE;
	RenderObjClass *reference = NEW_REF(OBBoxRenderObjClass, (def));
	EXPECT_EQ(obj->Get_Collision_Type(), reference->Get_Collision_Type());
	reference->Release_Ref();

	OBBoxRenderObjClass *obb = static_cast<OBBoxRenderObjClass *>(obj);
	EXPECT_NEAR(obb->Get_Local_Center().Z, 3.0f, 1e-6f);
	EXPECT_NEAR(obb->Get_Local_Extent().Y, 5.0f, 1e-6f);
	obj->Release_Ref();
}

TEST(W3xBuild, ObjectDamageDefaultsToZeroAndRoundTrips)
{
	W3xBox box;
	box.id = "BOX";
	RenderObjClass *obj = W3x_Create_Box(box, "DAMAGE_BOX");
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ(obj->Get_ObjectDamage(), 0.0f);
	obj->Set_ObjectDamage(0.5f);
	EXPECT_EQ(obj->Get_ObjectDamage(), 0.5f);
	obj->Release_Ref();
}
