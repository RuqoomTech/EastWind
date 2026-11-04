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

#include "W3DDevice/GameClient/W3DPostProcess.h"

TEST(TaaJitterTest, HaltonIsTheRadicalInverse)
{
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(0, 2), 0.0f);
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(1, 2), 0.5f);
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(2, 2), 0.25f);
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(3, 2), 0.75f);
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(1, 3), 1.0f / 3.0f);
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(2, 3), 2.0f / 3.0f);
	EXPECT_FLOAT_EQ(W3DPostProcess::Halton(4, 3), 4.0f / 9.0f);
}

TEST(TaaJitterTest, EightDistinctOffsetsInsideThePixelThatRepeat)
{
	float x[W3DPostProcess::TAA_SAMPLES];
	float y[W3DPostProcess::TAA_SAMPLES];
	for (unsigned f = 0; f < W3DPostProcess::TAA_SAMPLES; ++f)
	{
		W3DPostProcess::Jitter(f, x[f], y[f]);
		EXPECT_GE(x[f], -0.5f);
		EXPECT_LT(x[f], 0.5f);
		EXPECT_GE(y[f], -0.5f);
		EXPECT_LT(y[f], 0.5f);
		for (unsigned g = 0; g < f; ++g)
			EXPECT_FALSE(x[f] == x[g] && y[f] == y[g]) << "frames " << g << " and " << f;

		float rx, ry;
		W3DPostProcess::Jitter(f + W3DPostProcess::TAA_SAMPLES, rx, ry);
		EXPECT_EQ(rx, x[f]);
		EXPECT_EQ(ry, y[f]);
	}
	// Frame 0 uses Halton index 1, not the all-zero index 0.
	EXPECT_FLOAT_EQ(x[0], 0.0f);
	EXPECT_FLOAT_EQ(y[0], 1.0f / 3.0f - 0.5f);

	// Over the cycle the offsets average near the pixel centre, so the picture does not drift.
	float sx = 0.0f, sy = 0.0f;
	for (unsigned f = 0; f < W3DPostProcess::TAA_SAMPLES; ++f)
	{
		sx += x[f];
		sy += y[f];
	}
	EXPECT_NEAR(sx / (float)W3DPostProcess::TAA_SAMPLES, 0.0f, 0.07f);
	EXPECT_NEAR(sy / (float)W3DPostProcess::TAA_SAMPLES, 0.0f, 0.07f);
}
