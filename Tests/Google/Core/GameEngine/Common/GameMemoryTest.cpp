/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#include <climits>

#include "Common/Errors.h"
#include "Common/GameMemory.h"

#if !defined(DISABLE_GAMEMEMORY)
// A size that does not fit the allocator's Int byte counts must fail, not wrap into a small block.
TEST(GameMemory, OversizedAllocationThrows)
{
	const size_t tooBig = static_cast<size_t>(INT_MAX) + 1;
	EXPECT_THROW(TheDynamicMemoryAllocator->allocateBytes(tooBig, "test"), ErrorCode);
}

// Pool blocks sit back to back, so odd sizes must not push the next block off pointer alignment.
TEST(GameMemory, AllocationsArePointerAligned)
{
	void *blocks[3][64];
	for (int n = 0; n < 3; ++n)
		for (int size = 1; size <= 64; ++size)
		{
			void *p = TheDynamicMemoryAllocator->allocateBytes(size, "test");
			blocks[n][size - 1] = p;
			EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(void *), 0u) << "size " << size;
		}
	for (auto &row : blocks)
		for (void *p : row)
			TheDynamicMemoryAllocator->freeBytes(p);
}
#endif
