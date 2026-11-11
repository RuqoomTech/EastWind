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

#include <cstddef>

#include "WWMath/matrix3d.h"
#include "WW3D2/gpuskin.h"

TEST(GpuSkinTest, PaletteKeepsFirstUseOrder)
{
	const unsigned short links[] = { 5, 2, 5, 9, 2 };
	std::vector<unsigned short> bones;
	std::vector<float> rows;
	ASSERT_TRUE(Build_Skin_Palette(links, 5, 64, bones, rows));
	EXPECT_EQ(bones, (std::vector<unsigned short>{ 5, 2, 9 }));
	EXPECT_EQ(rows, (std::vector<float>{ 0, 3, 0, 6, 3 }));
}

TEST(GpuSkinTest, PaletteOverBudgetFails)
{
	std::vector<unsigned short> links;
	for (unsigned short b = 0; b < 65; ++b)
		links.push_back(b * 2);	// any bone index, not only the first 64
	std::vector<unsigned short> bones;
	std::vector<float> rows;
	EXPECT_FALSE(Build_Skin_Palette(links.data(), 65, 64, bones, rows));
	EXPECT_TRUE(bones.empty());
	EXPECT_TRUE(rows.empty());

	ASSERT_TRUE(Build_Skin_Palette(links.data(), 64, 64, bones, rows));
	EXPECT_EQ(bones.size(), 64u);
	EXPECT_EQ(rows[63], 63.0f * 3);
}

TEST(GpuSkinTest, PaletteWithoutLinksFails)
{
	const unsigned short links[] = { 1 };
	std::vector<unsigned short> bones(1, 7);
	std::vector<float> rows(1, 7.0f);
	EXPECT_FALSE(Build_Skin_Palette(nullptr, 1, 64, bones, rows));
	EXPECT_TRUE(bones.empty());
	EXPECT_TRUE(rows.empty());
	EXPECT_FALSE(Build_Skin_Palette(links, 0, 64, bones, rows));
}

TEST(GpuSkinTest, Palette2UnionOfBothLinks)
{
	const unsigned short links[] = { 5, 2, 5 };
	const unsigned short links2[] = { 2, 7, 5 };
	std::vector<unsigned short> bones;
	std::vector<float> rows, rows2;
	ASSERT_TRUE(Build_Skin_Palette2(links, links2, 3, 64, bones, rows, rows2));
	EXPECT_EQ(bones, (std::vector<unsigned short>{ 5, 2, 7 }));
	EXPECT_EQ(rows, (std::vector<float>{ 0, 3, 0 }));
	EXPECT_EQ(rows2, (std::vector<float>{ 3, 6, 0 }));
}

TEST(GpuSkinTest, Palette2RejectsTooManyBones)
{
	// 33 vertices, each with two new bones: 66 distinct, over 64.
	std::vector<unsigned short> links, links2;
	for (unsigned short v = 0; v < 33; ++v)
	{
		links.push_back(v * 2);
		links2.push_back(v * 2 + 1);
	}
	std::vector<unsigned short> bones;
	std::vector<float> rows, rows2;
	EXPECT_FALSE(Build_Skin_Palette2(links.data(), links2.data(), 33, 64, bones, rows, rows2));
	EXPECT_TRUE(bones.empty());
	EXPECT_TRUE(rows.empty());
	EXPECT_TRUE(rows2.empty());
	EXPECT_TRUE(Build_Skin_Palette2(links.data(), links2.data(), 32, 64, bones, rows, rows2));
	EXPECT_FALSE(Build_Skin_Palette2(links.data(), nullptr, 1, 64, bones, rows, rows2));
}

TEST(GpuSkinTest, BlendVertexLayout)
{
	static_assert(sizeof(GpuSkinVertex2Struct) == 80, "the blend skin vertex is 80 bytes");
	EXPECT_EQ(offsetof(GpuSkinVertex2Struct, row), 44u);
	EXPECT_EQ(offsetof(GpuSkinVertex2Struct, x2), 48u);
	EXPECT_EQ(offsetof(GpuSkinVertex2Struct, nx2), 60u);
	EXPECT_EQ(offsetof(GpuSkinVertex2Struct, row2), 72u);
	EXPECT_EQ(offsetof(GpuSkinVertex2Struct, weight2), 76u);
}

// The vertex shader's dot(row, (p, 1)) per packed row must give the CPU skin's tm * p.
TEST(GpuSkinTest, PackedRowsTransformAsTheCpuSkin)
{
	Matrix3D bone_tm[2];
	bone_tm[0].Make_Identity();
	bone_tm[0].Set_Translation(Vector3(1.0f, 2.0f, 3.0f));
	bone_tm[1].Make_Identity();
	bone_tm[1].Rotate_Z(0.7f);
	bone_tm[1].Set_Translation(Vector3(-4.0f, 0.5f, 9.0f));

	const unsigned short bones[] = { 1, 0 };
	float rows[2 * 12];
	Pack_Skin_Rows(bones, 2, [&bone_tm](unsigned short b) -> const Matrix3D & { return bone_tm[b]; }, rows);

	const Vector3 p(0.25f, -1.5f, 2.0f);
	for (int k = 0; k < 2; ++k)
	{
		Vector3 cpu;
		Matrix3D::Transform_Vector(bone_tm[bones[k]], p, &cpu);
		for (int r = 0; r < 3; ++r)
		{
			const float *row = rows + k * 12 + r * 4;
			const float gpu = row[0] * p.X + row[1] * p.Y + row[2] * p.Z + row[3];
			EXPECT_NEAR(gpu, cpu[r], 1e-5f);
		}
	}
}
