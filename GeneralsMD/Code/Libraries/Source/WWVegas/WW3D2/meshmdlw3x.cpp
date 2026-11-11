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

// EastWind native W3X mesh builder. MeshModelClass::Load_W3X fills the model from a parsed W3xMesh in the
// same order and with the same engine calls as Load_W3D (meshmdlio.cpp) makes for the W3D that
// w3xconvert.cpp writes, plus the RA3 material extras (second texture, UV scroll and flipbook mappers).

#include "meshmdl.h"
#include "mapper.h"
#include "matinfo.h"
#include "vertmaterial.h"
#include "shader.h"
#include "texture.h"
#include "w3d_file.h"
#include "w3d_util.h"
#include "w3xparse.h"
#include "assetmgr.h"
#include "dx9wrapper.h"
#include "WWDebug/wwdebug.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// The last constant of that name, whatever its kind (the parser keeps them all in file order).
const W3xConstant *Find_Constant(const W3xMaterial &m, const char *name)
{
	const W3xConstant *found = nullptr;
	for (const W3xConstant &c : m.constants)
		if (_stricmp(c.name.c_str(), name) == 0)
			found = &c;
	return found;
}

// The byte round trip the converter's W3D colours go through.
float Byte_Color(float f)
{
	return (float)(f <= 0.0f ? 0 : f >= 1.0f ? 255 : (int)(f * 255.0f + 0.5f)) / 255.0f;
}

// A flipbook or a scale and scroll mapper for the stage, or null. The caller owns the returned reference.
TextureMapperClass *Make_Mapper(const W3xMaterial &m, int stage, const char *id)
{
	char name[64];
	snprintf(name, sizeof(name), "TexCoordMapper_%d", stage);
	const W3xConstant *mode = Find_Constant(m, name);
	const int mapper = mode ? mode->asInt : 0;

	if (stage == 0) {
		const W3xConstant *anim = Find_Constant(m, "TextureAnimation_FPS_NumPerRow_LastFrame_FrameOffset_0");
		if (anim && anim->values.size() >= 4) {
			const float fps = anim->values[0];
			const int per_row = (int)(anim->values[1] + 0.5f);
			// Mapper 2 is RA3's video mapping; "0 1 1 0" is its no-animation default.
			if (per_row >= 2 && (mapper == 2 || fps != 0.0f)) {
				if ((per_row & (per_row - 1)) == 0) {
					unsigned grid_log2 = 0;
					while ((1 << grid_log2) < per_row)
						++grid_log2;
					const unsigned last = (unsigned)std::max(0.0f, anim->values[2] + 0.5f);
					const unsigned offset = (unsigned)std::max(0.0f, anim->values[3] + 0.5f);
					return NEW_REF(GridTextureMapperClass, (fps, grid_log2, last, offset, 0));
				}
				// shortcut: GridTextureMapperClass needs a power of two grid, so the flipbook is not played; upgrade with Phase 2 F7.
				WWDEBUG_SAY(("W3X mesh %s: flipbook with %d frames per row is not a power of two, not played", id, per_row));
			}
		}
	}

	snprintf(name, sizeof(name), "TexCoordTransform_%d", stage);
	const W3xConstant *transform = Find_Constant(m, name);
	if (mapper == 1 && transform && transform->values.size() >= 4) {
		const Vector2 scale(transform->values[0], transform->values[1]);
		const Vector2 scroll(transform->values[2], transform->values[3]);
		if (scroll.X == 0.0f && scroll.Y == 0.0f) {
			if (scale.X == 1.0f && scale.Y == 1.0f)
				return nullptr;
			return NEW_REF(ScaleTextureMapperClass, (scale, stage));
		}
		// The mapper negates its rate ("legacy"), so negate here to scroll by +zw per second like RA3.
		return NEW_REF(LinearOffsetTextureMapperClass, (Vector2(-scroll.X, -scroll.Y), Vector2(0.0f, 0.0f), false, scale, stage));
	}
	return nullptr;
}
} // namespace

std::string W3x_Texture_File(const std::string &id, const std::map<std::string, std::string> &textureFiles)
{
	std::string key = id;
	for (char &c : key)
		c = (char)tolower((unsigned char)c);
	const auto found = textureFiles.find(key);
	if (found == textureFiles.end() || found->second.empty())
		return id + ".tga";
	// The engine looks textures up by file name alone.
	return found->second.substr(found->second.find_last_of("/\\") + 1);
}

ShaderClass::DetailColorFuncType W3x_Detail_Color_Func(int secondaryBlendMode)
{
	switch (secondaryBlendMode) {
		case 0: return ShaderClass::DETAILCOLOR_SCALE;
		case 1: return ShaderClass::DETAILCOLOR_SCALE2X;
		default: return ShaderClass::DETAILCOLOR_DETAILBLEND; // 2; 3 (specular alpha) has no match and uses it too
	}
}

uint32 W3x_Mesh_Attributes(const W3xMesh &mesh, bool twoSided)
{
	uint32 attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_NORMAL;
	if (mesh.geometryType == W3X_GEOMETRY_SKIN)
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN;
	else if (mesh.geometryType == W3X_GEOMETRY_CAMERA_ALIGNED)
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ALIGNED;
	else if (mesh.geometryType == W3X_GEOMETRY_CAMERA_ORIENTED)
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ORIENTED;
	if (mesh.castShadow)
		attributes |= W3D_MESH_FLAG_CAST_SHADOW;
	if (mesh.hidden)
		attributes |= W3D_MESH_FLAG_HIDDEN;
	if (twoSided)
		attributes |= W3D_MESH_FLAG_TWO_SIDED;
	return attributes;
}

bool MeshModelClass::Load_W3X(const W3xMesh &mesh, const char *name, const W3xHierarchy *tree,
	const std::map<std::string, std::string> &textureFiles)
{
	const char *id = mesh.id.c_str();

	// Everything that can fail comes before Reset, so a refused mesh leaves the model as it was.
	W3xMeshVertices verts;
	std::string error;
	if (!W3x_Single_Bone_Vertices(mesh, tree, verts, error)) {
		WWDEBUG_SAY(("W3X mesh %s: %s", id, error.c_str()));
		return false;
	}
	const size_t count = verts.positions.size();
	// TriIndex is 16 bit.
	if (count > 65535 || mesh.triangles.empty()) {
		WWDEBUG_SAY(("W3X mesh %s: %d vertices and %d triangles cannot be built (limit 65535 vertices)", id, (int)count, (int)mesh.triangles.size()));
		return false;
	}
	if (verts.blended)
		WWDEBUG_SAY(("W3X mesh %s: %d vertices are blended between bones and follow only their heaviest bone", id, verts.blended));

	const bool skin = mesh.geometryType == W3X_GEOMETRY_SKIN;
	std::vector<std::string> warnings;

	const bool has_uv = !mesh.uvSets.empty() && mesh.uvSets[0].size() == count;
	if (!mesh.uvSets.empty() && !has_uv)
		warnings.push_back("its TexCoords do not match its vertices and are dropped");

	W3xShading shading;
	W3x_Map_Shader(mesh, has_uv, shading, warnings);

	const W3xMaterial &mat = mesh.material;
	const bool default_w3d = _stricmp(mat.shaderName.c_str(), "DefaultW3D.fx") == 0;

	const W3xConstant *num_textures = Find_Constant(mat, "NumTextures");
	const W3xConstant *texture_1 = Find_Constant(mat, "Texture_1");
	const bool wants_second = !shading.textureId.empty() && num_textures && num_textures->asInt >= 2
		&& texture_1 && texture_1->kind == W3xConstant::Texture && !texture_1->text.empty();
	const bool has_second = wants_second && mesh.uvSets.size() >= 2 && mesh.uvSets[1].size() == count;
	if (wants_second && !has_second)
		warnings.push_back("Texture_1 is dropped because the mesh has no second TexCoords set");

	for (const std::string &w : warnings) {
		(void)w; // WWDEBUG_SAY is empty in Release
		WWDEBUG_SAY(("W3X mesh %s: %s", id, w.c_str()));
	}

	// Same conversion Load_W3D applies to the shader chunk.
	ShaderClass shader;
	W3dUtilityClass::Convert_Shader(shading.shader, &shader);
	if (has_second) {
		const W3xConstant *blend = Find_Constant(mat, "SecondaryTextureBlendMode");
		shader.Set_Post_Detail_Color_Func(W3x_Detail_Color_Func(blend ? blend->asInt : 0));
	}

	/*
	** The header: Reset zeroes the flags, so everything else comes after it
	*/
	Reset((int)mesh.triangles.size(), (int)count, 1);
	W3dAttributes = W3x_Mesh_Attributes(mesh, shading.twoSided);
	SortLevel = (char)std::min(std::max(mesh.sortLevel, 0), (int)MAX_SORT_LEVEL);
	Set_Name(name);

	BoundBoxMin = verts.min;
	BoundBoxMax = verts.max;
	BoundSphereCenter = verts.center;
	BoundSphereRadius = verts.radius;

	switch (mesh.geometryType) {
		case W3X_GEOMETRY_CAMERA_ALIGNED:
			Set_Flag(ALIGNED, true);
			break;
		case W3X_GEOMETRY_CAMERA_ORIENTED:
			Set_Flag(ORIENTED, true);
			break;
		case W3X_GEOMETRY_SKIN:
			Set_Flag(SKIN, true);
			Set_Flag(ALLOW_NPATCHES, true);
			break;
		default:
			break;
	}
	if (shading.twoSided)
		Set_Flag(TWO_SIDED, true);
	if (mesh.castShadow)
		Set_Flag(CAST_SHADOW, true);

	/*
	** Geometry. Planes are rebuilt by the engine and PolySurfaceType is zero (SURFACE_TYPE_DEFAULT) from Reset.
	*/
	Vector3 *vertex = Vertex->Get_Array();
	Vector3 *normal = get_vert_normals();
	for (size_t i = 0; i < count; ++i) {
		vertex[i] = verts.positions[i];
		normal[i] = verts.normals[i];
	}
	TriIndex *poly = get_polys();
	for (size_t i = 0; i < mesh.triangles.size(); ++i) {
		poly[i].I = (unsigned short)mesh.triangles[i].v[0];
		poly[i].J = (unsigned short)mesh.triangles[i].v[1];
		poly[i].K = (unsigned short)mesh.triangles[i].v[2];
	}
	if (skin) {
		// The engine has one bone per vertex; W3x_Single_Bone_Vertices chose the heaviest. shortcut: two-bone blending is Phase 2 F2.
		uint16 *links = get_bone_links(true);
		for (size_t i = 0; i < count; ++i)
			links[i] = verts.bones[i];
	}

	/*
	** Materials: one pass, one vertex material, one shader, one or two textures
	*/
	Set_Pass_Count(1);

	VertexMaterialClass *vmat = NEW_REF(VertexMaterialClass, ());
	vmat->Parse_W3dVertexMaterialStruct(shading.material);
	DefMatDesc->Set_Single_Material(vmat, 0);
	DefMatDesc->Set_Single_Shader(shader, 0);

	// Turn on sorting if pass 0 blends and does not alpha test (same rule as read_shader_ids).
	if (shader.Get_Dst_Blend_Func() != ShaderClass::DSTBLEND_ZERO && shader.Get_Alpha_Test() == ShaderClass::ALPHATEST_DISABLE
		&& SortLevel == SORT_LEVEL_NONE)
		Set_Flag(SORT, true);

	// Only DefaultW3D.fx stores real colours; the other shaders keep (1,1,1,0) there.
	if (default_w3d && mesh.vertexColors.size() == count) {
		unsigned *dcg = DefMatDesc->Get_Color_Array(0, true);
		for (size_t i = 0; i < count; ++i) {
			const Vector4 &c = mesh.vertexColors[i];
			dcg[i] = DX9Wrapper::Convert_Color(Vector4(Byte_Color(c.X), Byte_Color(c.Y), Byte_Color(c.Z), Byte_Color(c.W)));
		}
		DefMatDesc->Set_DCG_Source(0, VertexMaterialClass::COLOR1);
	}

	TextureClass *texture_0 = nullptr, *texture_1_ptr = nullptr;
	WW3DAssetManager *assets = WW3DAssetManager::Get_Instance();
	if (!shading.textureId.empty()) {
		texture_0 = assets->Get_Texture(W3x_Texture_File(shading.textureId, textureFiles).c_str());
		DefMatDesc->Set_Single_Texture(texture_0, 0, 0);
		// Both W3X and W3D store V upwards; the engine flips it on load.
		std::vector<Vector2> uvs(count);
		for (size_t i = 0; i < count; ++i)
			uvs[i] = Vector2(mesh.uvSets[0][i].X, 1.0f - mesh.uvSets[0][i].Y);
		DefMatDesc->Install_UV_Array(0, 0, uvs.data(), (int)count);

		if (TextureMapperClass *mapper = Make_Mapper(mat, 0, id)) {
			vmat->Set_Mapper(mapper, 0);
			mapper->Release_Ref();
		}
	}
	if (has_second) {
		texture_1_ptr = assets->Get_Texture(W3x_Texture_File(texture_1->text, textureFiles).c_str());
		DefMatDesc->Set_Single_Texture(texture_1_ptr, 0, 1);
		std::vector<Vector2> uvs(count);
		for (size_t i = 0; i < count; ++i)
			uvs[i] = Vector2(mesh.uvSets[1][i].X, 1.0f - mesh.uvSets[1][i].Y);
		DefMatDesc->Install_UV_Array(0, 1, uvs.data(), (int)count);

		if (TextureMapperClass *mapper = Make_Mapper(mat, 1, id)) {
			vmat->Set_Mapper(mapper, 1);
			mapper->Release_Ref();
		}
	}

	/*
	** What install_materials does: finish the materials, then hand the refs to the MatInfo
	*/
	DefMatDesc->Post_Load_Process(true, this);
	if (texture_0)
		MatInfo->Add_Texture(texture_0);
	if (texture_1_ptr)
		MatInfo->Add_Texture(texture_1_ptr);
	MatInfo->Add_Vertex_Material(vmat);

	// The matdesc and MatInfo hold their own references now.
	if (texture_0)
		texture_0->Release_Ref();
	if (texture_1_ptr)
		texture_1_ptr->Release_Ref();
	vmat->Release_Ref();

	post_process();
	return true;
}
