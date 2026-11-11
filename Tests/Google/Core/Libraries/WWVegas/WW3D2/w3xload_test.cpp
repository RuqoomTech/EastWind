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

#include "WW3D2/w3xload.h"

TEST(W3xLoad, AssetNameUsesStemForContainerId)
{
	EXPECT_EQ(W3x_Asset_Name("ABAIRFIELD", "ABAIRFIELD", "ABAirfield_L"), "ABAirfield_L");
	EXPECT_EQ(W3x_Asset_Name("abairfield", "ABAIRFIELD", "ABAirfield_L"), "ABAirfield_L");
}

TEST(W3xLoad, AssetNameKeepsOtherIds)
{
	EXPECT_EQ(W3x_Asset_Name("ABAIRFIELD_SKL", "ABAIRFIELD", "ABAirfield_L"), "ABAIRFIELD_SKL");
}

TEST(W3xLoad, AssetNameWithoutContainerKeepsId)
{
	EXPECT_EQ(W3x_Asset_Name("ABAIRFIELD", "", "ABAirfield_L"), "ABAIRFIELD");
	EXPECT_EQ(W3x_Asset_Name("", "", "x"), "");
}

TEST(W3xLoad, FileStemDropsDirectoriesAndExtension)
{
	EXPECT_EQ(W3x_File_Stem("..\\Art\\W3D\\ABAirfield_L.w3x"), "ABAirfield_L");
	EXPECT_EQ(W3x_File_Stem("Art/W3D/ABAirfield_L.w3x"), "ABAirfield_L");
	EXPECT_EQ(W3x_File_Stem("plain.w3x"), "plain");
	EXPECT_EQ(W3x_File_Stem("noext"), "noext");
}

TEST(W3xLoad, IncludeStemDropsPrefix)
{
	EXPECT_EQ(W3x_File_Stem("ART:AUJavelin_SKL.w3x"), "AUJavelin_SKL");
	EXPECT_EQ(W3x_File_Stem("DATA:ART:foo_skl.w3x"), "foo_skl");
}
