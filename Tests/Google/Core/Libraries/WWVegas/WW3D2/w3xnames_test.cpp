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

#include <string.h>

#include "WW3D2/boxrobj.h"
#include "WW3D2/htree.h"
#include "WW3D2/pivot.h"

// W3X names are not limited to the W3D field sizes.
TEST(W3xNames, PivotCopyAndAssignmentKeepLongName)
{
	const char *name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 26 chars
	PivotClass a;
	a.Name = name;

	PivotClass copy(a);
	EXPECT_STREQ(copy.Name, name);

	PivotClass assigned;
	assigned = a;
	EXPECT_STREQ(assigned.Name, name);
}

TEST(W3xNames, HTreeNameRoundTrips)
{
	HTreeClass tree;
	tree.Set_Name("A_TREE_NAME_LONGER_THAN_SIXTEEN");
	EXPECT_STREQ(tree.Get_Name(), "A_TREE_NAME_LONGER_THAN_SIXTEEN");
	HTreeClass copy(tree);
	EXPECT_STREQ(copy.Get_Name(), "A_TREE_NAME_LONGER_THAN_SIXTEEN");
}

TEST(W3xNames, BoxNameRoundTrips)
{
	const char *name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456"; // 33 chars
	OBBoxRenderObjClass *box = NEW_REF(OBBoxRenderObjClass, ());
	box->Set_Name(name);
	EXPECT_STREQ(box->Get_Name(), name);

	RenderObjClass *clone = box->Clone();
	EXPECT_STREQ(clone->Get_Name(), name);

	REF_PTR_RELEASE(clone);
	REF_PTR_RELEASE(box);
}
