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

#include "WW3D2/vertmaterial.h"

TEST(VertexMaterialExtras, DefaultsKeepTheCrc)
{
	VertexMaterialClass plain;
	VertexMaterialClass withDefaults;
	withDefaults.Set_W3x_Extras(VertexMaterialClass::W3xExtrasStruct());
	EXPECT_TRUE(plain.Get_W3x_Extras().Is_Default());
	EXPECT_EQ(plain.Get_CRC(), withDefaults.Get_CRC());
}

TEST(VertexMaterialExtras, EdgeFadeChangesTheCrc)
{
	VertexMaterialClass plain;
	VertexMaterialClass faded;
	VertexMaterialClass::W3xExtrasStruct extras;
	extras.EdgeFadeOut = 0.5f;
	faded.Set_W3x_Extras(extras);
	EXPECT_NE(plain.Get_CRC(), faded.Get_CRC());
}

TEST(VertexMaterialExtras, CopyKeepsTheExtras)
{
	VertexMaterialClass src;
	VertexMaterialClass::W3xExtrasStruct extras;
	extras.EdgeFadeOut = 0.25f;
	extras.BumpScale = 2.0f;
	extras.EmissiveHDR = 3.0f;
	src.Set_W3x_Extras(extras);

	VertexMaterialClass copy(src);
	EXPECT_EQ(copy.Get_W3x_Extras().EdgeFadeOut, 0.25f);
	EXPECT_EQ(copy.Get_W3x_Extras().BumpScale, 2.0f);
	EXPECT_EQ(copy.Get_W3x_Extras().EmissiveHDR, 3.0f);

	VertexMaterialClass assigned;
	assigned = src;
	EXPECT_EQ(assigned.Get_W3x_Extras().BumpScale, 2.0f);
	EXPECT_EQ(assigned.Get_CRC(), src.Get_CRC());
}
