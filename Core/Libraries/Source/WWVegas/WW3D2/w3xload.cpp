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

#include "w3xload.h"

#include "assetmgr.h"
#include "boxrobj.h"
#include "hanimmgr.h"
#include "hlod.h"
#include "hrawanim.h"
#include "htree.h"
#include "htreemgr.h"
#include "mesh.h"
#include "proto.h"
#include "WWDebug/wwdebug.h"
#include "WWLib/ffactory.h"

#include <ctype.h>
#include <set>
#include <string.h>

namespace
{
std::string Lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

// Part of a mesh or box id after its last dot: the id "ABAIRFIELD.BODY" names the mesh "<stem>.BODY".
std::string Sub_Name(const std::string &id)
{
	const size_t dot = id.rfind('.');
	return dot == std::string::npos ? id : id.substr(dot + 1);
}

bool Read_File(FileClass &file, std::string &out)
{
	if (!file.Open())
		return false;
	const int size = file.Size();
	bool ok = size > 0;
	if (ok) {
		out.resize((size_t)size);
		ok = file.Read(&out[0], size) == size;
	}
	file.Close();
	return ok;
}

// Stems being loaded now: an include chain that comes back to its own file must stop.
std::set<std::string> g_loading;

// Every W3X hierarchy registered, by lower-case registered name. A skin mesh needs the parsed pose,
// which the HTreeClass does not keep. Entries outlive Free_Assets; they are only read for trees the
// manager still finds.
std::map<std::string, W3xHierarchy> g_trees;

struct LoadGuard
{
	explicit LoadGuard(const std::string &k) : key(k) {}
	~LoadGuard() { g_loading.erase(key); }
	std::string key;
};
} // namespace

std::string W3x_File_Stem(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\:");
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos)
		name.resize(dot);
	return name;
}

std::string W3x_Asset_Name(const std::string &id, const std::string &containerId, const std::string &stem)
{
	if (!containerId.empty() && _stricmp(id.c_str(), containerId.c_str()) == 0)
		return stem;
	return id;
}

bool WW3DAssetManager::Load_W3X_Assets(FileClass &file)
{
	const std::string stem = W3x_File_Stem(file.File_Name());
	if (!g_loading.insert(Lower(stem)).second)
		return true;
	LoadGuard guard(Lower(stem));

	std::string xml;
	if (!Read_File(file, xml)) {
		WWDEBUG_SAY(("W3X: cannot read '%s'.", file.File_Name()));
		return false;
	}

	W3xFile w3x;
	std::string error;
	std::vector<std::string> warnings;
	if (!W3x_Parse(xml.data(), xml.size(), w3x, error, warnings)) {
		WWDEBUG_SAY(("W3X: '%s' not loaded: %s", file.File_Name(), error.c_str()));
		return false;
	}
	for (const std::string &w : warnings) {
		(void)w; // WWDEBUG_SAY is empty in Release
		WWDEBUG_SAY(("W3X: '%s': %s", file.File_Name(), w.c_str()));
	}

	// Texture ids: this file's own declarations win over the included .xml ones.
	std::map<std::string, std::string> textureFiles;
	W3x_Parse_Texture_Declarations(xml.data(), xml.size(), textureFiles);
	for (const std::string &include : w3x.xmlIncludes) {
		const std::string base = include.substr(include.find_last_of("/\\:") + 1);
		FileClass *f = _TheFileFactory->Get_File(base.c_str());
		if (f) {
			std::string text;
			if (f->Is_Available() && Read_File(*f, text)) {
				std::map<std::string, std::string> declared;
				W3x_Parse_Texture_Declarations(text.data(), text.size(), declared);
				textureFiles.insert(declared.begin(), declared.end());
			}
			_TheFileFactory->Return_File(f);
		}
	}

	// Skeleton files: through the virtual call, so a .w3d of that name still wins and the game's override runs.
	for (const std::string &include : w3x.w3xIncludes) {
		const std::string includeStem = W3x_File_Stem(include);
		if (includeStem.empty() || _stricmp(includeStem.c_str(), stem.c_str()) == 0)
			continue;
		if (HTreeManager.Get_Tree_ID(includeStem.c_str()) != -1)
			continue;
		Load_3D_Assets((includeStem + ".w3d").c_str());
	}

	const std::string containerId = w3x.containers.empty() ? std::string() : w3x.containers[0].id;

	// Hierarchies. The first one registered under a name wins.
	std::map<std::string, std::string> treeNames; // lower-case local id -> registered name
	for (const W3xHierarchy &h : w3x.hierarchies) {
		const std::string name = W3x_Asset_Name(h.id, containerId, stem);
		treeNames[Lower(h.id)] = name;
		if (HTreeManager.Get_Tree_ID(name.c_str()) != -1) {
			WWDEBUG_SAY(("W3X: '%s': hierarchy %s already exists, keeping the first.", file.File_Name(), name.c_str()));
			continue;
		}
		HTreeClass *tree = W3DNEW HTreeClass;
		if (!tree->Load_W3X(h, name.c_str()) || !HTreeManager.Add_Tree(tree)) {
			delete tree;
			WWDEBUG_SAY(("W3X: '%s': hierarchy %s not registered.", file.File_Name(), name.c_str()));
			continue;
		}
		g_trees[Lower(name)] = h;
	}

	// A local id goes through the rename above; any other id names a tree from an include or a .w3d.
	const std::string defaultTreeId = w3x.hierarchies.empty() ? std::string() : w3x.hierarchies[0].id;
	auto tree_name = [&](const std::string &id) {
		const auto found = treeNames.find(Lower(id));
		return found != treeNames.end() ? found->second : id;
	};
	// The mesh skin pose, as the converter: the container's hierarchy, else the file's first.
	const std::string meshTreeName = tree_name(!w3x.containers.empty() && !w3x.containers[0].hierarchy.empty() ? w3x.containers[0].hierarchy : defaultTreeId);
	// Load the tree on demand first: that fills g_trees when it is a .w3x outside this file's includes.
	if (!meshTreeName.empty())
		Get_HTree(meshTreeName.c_str());
	const auto cached = meshTreeName.empty() ? g_trees.end() : g_trees.find(Lower(meshTreeName));
	const W3xHierarchy *meshTree = cached != g_trees.end() ? &cached->second : nullptr;

	// Meshes and boxes are "<stem>.<id after the last dot>"; the container names them by their full id.
	std::map<std::string, std::string> subNames;
	for (const W3xMesh &m : w3x.meshes) {
		const std::string name = stem + "." + Sub_Name(m.id);
		subNames[Lower(m.id)] = name;
		if (Render_Obj_Exists(name.c_str())) {
			WWDEBUG_SAY(("W3X: '%s': mesh %s already exists, keeping the first.", file.File_Name(), name.c_str()));
			continue;
		}
		const bool skin = m.geometryType == W3X_GEOMETRY_SKIN;
		if (skin && !meshTree) {
			WWDEBUG_SAY(("W3X: '%s': skin mesh %s skipped, hierarchy '%s' has no W3X pose.", file.File_Name(), name.c_str(), meshTreeName.c_str()));
			continue;
		}
		MeshClass *mesh = NEW_REF(MeshClass, ());
		if (mesh->Load_W3X(m, name.c_str(), skin ? meshTree : nullptr, textureFiles))
			Add_Prototype(W3DNEW PrimitivePrototypeClass(mesh));
		else
			WWDEBUG_SAY(("W3X: '%s': mesh %s not loaded.", file.File_Name(), name.c_str()));
		mesh->Release_Ref();
	}
	for (const W3xBox &b : w3x.boxes) {
		const std::string name = stem + "." + Sub_Name(b.id);
		subNames[Lower(b.id)] = name;
		if (Render_Obj_Exists(name.c_str())) {
			WWDEBUG_SAY(("W3X: '%s': box %s already exists, keeping the first.", file.File_Name(), name.c_str()));
			continue;
		}
		RenderObjClass *box = W3x_Create_Box(b, name.c_str());
		Add_Prototype(W3DNEW PrimitivePrototypeClass(box));
		box->Release_Ref();
	}

	for (const W3xContainer &c : w3x.containers) {
		const std::string name = W3x_Asset_Name(c.id, containerId, stem);
		if (Render_Obj_Exists(name.c_str())) {
			WWDEBUG_SAY(("W3X: '%s': container %s already exists, keeping the first.", file.File_Name(), name.c_str()));
			continue;
		}
		const std::string treeName = tree_name(c.hierarchy.empty() ? defaultTreeId : c.hierarchy);
		HTreeClass *tree = treeName.empty() ? nullptr : Get_HTree(treeName.c_str());
		if (!treeName.empty() && !tree)
			WWDEBUG_SAY(("W3X: '%s': hierarchy %s of container %s not found.", file.File_Name(), treeName.c_str(), name.c_str()));

		std::vector<std::pair<std::string, int>> subs;
		const int boneCount = tree ? tree->Num_Pivots() : 1;
		for (const W3xSubObject &s : c.subObjects) {
			const auto found = subNames.find(Lower(s.renderObjectId));
			if (found == subNames.end()) {
				WWDEBUG_SAY(("W3X: '%s': container %s sub-object %s names %s, which is not in this file.", file.File_Name(), name.c_str(), s.subObjectId.c_str(), s.renderObjectId.c_str()));
				continue;
			}
			if (s.boneIndex >= (uint32)boneCount) {
				WWDEBUG_SAY(("W3X: '%s': container %s sub-object %s is on bone %u, the hierarchy has %d.", file.File_Name(), name.c_str(), s.renderObjectId.c_str(), s.boneIndex, boneCount));
				continue;
			}
			subs.push_back(std::make_pair(found->second, (int)s.boneIndex));
		}

		HLodDefClass *def = W3DNEW HLodDefClass;
		if (def->Load_W3X(name.c_str(), treeName.c_str(), subs))
			Add_Prototype(W3DNEW HLodPrototypeClass(def));
		else
			delete def;
	}

	// Animations are "<tree>.<id>"; the INI names them that way and Get_HAnim loads "<id>.w3d" for them.
	for (const W3xAnimation &a : w3x.animations) {
		const std::string treeName = tree_name(a.hierarchy.empty() ? defaultTreeId : a.hierarchy);
		HTreeClass *tree = treeName.empty() ? nullptr : Get_HTree(treeName.c_str());
		if (!tree) {
			WWDEBUG_SAY(("W3X: '%s': animation %s skipped, hierarchy '%s' not found.", file.File_Name(), a.id.c_str(), treeName.c_str()));
			continue;
		}
		const std::string name = treeName + "." + W3x_Asset_Name(a.id, containerId, stem);
		if (HAnimManager.Peek_Anim(name.c_str()) != nullptr) {
			WWDEBUG_SAY(("W3X: '%s': animation %s already exists, keeping the first.", file.File_Name(), name.c_str()));
			continue;
		}
		HRawAnimClass *anim = W3DNEW HRawAnimClass;
		if (anim->Load_W3X(a, name.c_str(), treeName.c_str(), tree))
			HAnimManager.Add_Anim(anim);
		else
			WWDEBUG_SAY(("W3X: '%s': animation %s not loaded.", file.File_Name(), name.c_str()));
		anim->Release_Ref();
	}

	return true;
}

RenderObjClass *W3x_Create_Box(const W3xBox &box, const char *name)
{
	// Same fields as the converter's Write_Box, so the box state is what loading its W3D gives.
	W3dBoxStruct def;
	memset(&def, 0, sizeof(def));
	def.Version = W3D_BOX_CURRENT_VERSION;
	def.Attributes = W3D_BOX_ATTRIBUTE_ORIENTED | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PHYSICAL | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PROJECTILE;
	def.Color.Set((uint8)255, (uint8)255, (uint8)255);
	def.Center = { box.center.X, box.center.Y, box.center.Z };
	def.Extent = { box.extent.X, box.extent.Y, box.extent.Z };

	OBBoxRenderObjClass *obj = NEW_REF(OBBoxRenderObjClass, (def));
	obj->Set_Name(name); // the W3D name field would clip it
	return obj;
}
