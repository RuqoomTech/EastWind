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

#include "WW3D2/skincache.h"

TEST(SkinCacheTest, ReusesOnlyTheSameFrameModelAndCount)
{
	SkinCacheClass cache;
	const int modelA = 0, modelB = 0;
	Vector3 *loc = nullptr, *norm = nullptr;

	EXPECT_FALSE(cache.Fetch(1, &modelA, 100, loc, norm));	// empty
	ASSERT_NE(loc, nullptr);
	EXPECT_EQ(norm, loc + 100);
	loc[0].Set(1, 2, 3);
	EXPECT_TRUE(cache.Fetch(1, &modelA, 100, loc, norm));	// a later pass of the frame
	EXPECT_EQ(loc[0].Y, 2.0f);

	EXPECT_FALSE(cache.Fetch(2, &modelA, 100, loc, norm));	// next frame
	EXPECT_FALSE(cache.Fetch(2, &modelB, 100, loc, norm));	// another model
	EXPECT_FALSE(cache.Fetch(2, &modelB, 60, loc, norm));	// another vertex count
	EXPECT_EQ(norm, loc + 60);
	EXPECT_TRUE(cache.Fetch(2, &modelB, 60, loc, norm));

	const Vector3 *before = loc;
	EXPECT_FALSE(cache.Fetch(3, &modelB, 80, loc, norm));	// fits the 200 already there: no realloc
	EXPECT_EQ(loc, before);

	cache.Reset();
	EXPECT_FALSE(cache.Fetch(3, &modelB, 80, loc, norm));
}
