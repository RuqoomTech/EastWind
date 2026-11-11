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

#include "w3xconvert.h"
#include "w3xparse.h"

#include "w3d_file.h"
#include "WWMath/quat.h"
#include "WWMath/vector3.h"

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <set>

namespace
{
const size_t MAX_NAME = W3D_NAME_LEN - 1;

// Vertex arrays are written straight from Vector3 arrays.
static_assert(sizeof(Vector3) == sizeof(W3dVectorStruct), "Vector3 must match W3dVectorStruct");

W3dVectorStruct W3d_Vec(const Vector3 &v) { return { v.X, v.Y, v.Z }; }

uint8 Color_Byte(float f) { return (uint8)(f <= 0.0f ? 0 : f >= 1.0f ? 255 : (int)(f * 255.0f + 0.5f)); }

bool Same(const char *a, const char *b) { return _stricmp(a, b) == 0; }
bool Same(const std::string &a, const std::string &b) { return Same(a.c_str(), b.c_str()); }

std::string Lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

void Copy_Name(char *dst, size_t size, const std::string &s)
{
	memset(dst, 0, size);
	memcpy(dst, s.data(), s.size() < size ? s.size() : size - 1);
}

std::string Format(const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

// The part of an asset id after its last dot: "CONTAINER.MESH" gives "MESH".
std::string Sub_Name(const std::string &id)
{
	const size_t dot = id.rfind('.');
	return dot == std::string::npos ? id : id.substr(dot + 1);
}

// Writes W3D chunks into memory. A chunk's size excludes its 8-byte header, and its top bit marks a
// chunk that holds chunks (WWLib chunkio.h).
class ChunkWriter
{
public:
	void Begin(uint32 id)
	{
		if (!Open.empty())
			Open.back().second = true;
		Open.push_back({ Bytes.size(), false });
		const uint32 header[2] = { id, 0 };
		Put(header, sizeof(header));
	}

	void End()
	{
		const size_t start = Open.back().first;
		const uint32 size = uint32(Bytes.size() - start - 8) | (Open.back().second ? 0x80000000u : 0u);
		Open.pop_back();
		memcpy(&Bytes[start + 4], &size, 4);
	}

	void Put(const void *data, size_t size)
	{
		const unsigned char *p = static_cast<const unsigned char *>(data);
		Bytes.insert(Bytes.end(), p, p + size);
	}

	template <class T> void Chunk(uint32 id, const T &value) { Chunk(id, &value, sizeof(T)); }

	void Chunk(uint32 id, const void *data, size_t size)
	{
		Begin(id);
		Put(data, size);
		End();
	}

	std::vector<unsigned char> Bytes;

private:
	std::vector<std::pair<size_t, bool>> Open;
};

class Converter
{
public:
	Converter(W3xConvertResult &result, const W3xIncludeReader &read_include) : Result(result), Read_Include(read_include) {}

	bool Run(const std::string &xml, const std::string &name);

private:
	bool Fail(const std::string &error)
	{
		Result.Error = error;
		return false;
	}

	void Warn(const std::string &warning) { Result.Warnings.push_back(warning); }

	std::vector<std::string> Short_Names(const std::vector<std::string> &names, const char *what);
	const W3xHierarchy *Find_Hierarchy(const std::string &id);
	bool Write_Hierarchy(const W3xHierarchy &tree);
	bool Write_Mesh(const W3xMesh &mesh, const std::string &mesh_name);
	void Write_Box(const W3xBox &box, const std::string &box_name);
	bool Write_Container(const W3xContainer &container, const std::map<std::string, std::string> &sub_names);
	bool Write_Animation(const W3xAnimation &anim);

	size_t Pivot_Count() const { return Tree ? Tree->pivots.size() : 0; }

	W3xConvertResult &Result;
	const W3xIncludeReader &Read_Include;
	W3xFile File;
	std::list<W3xFile> Includes; // keeps the hierarchy found in an include alive
	const W3xHierarchy *Tree = nullptr;
	ChunkWriter W;
	std::string Model;
};

// Gives every name a W3D name of at most 15 characters, unique ignoring case. The first use of a name
// that fits is kept; longer names are cut, and a cut or a repeated name that clashes ends in ~N instead.
std::vector<std::string> Converter::Short_Names(const std::vector<std::string> &names, const char *what)
{
	std::set<std::string> used;
	std::vector<bool> keep(names.size());
	for (size_t i = 0; i < names.size(); ++i)
		keep[i] = names[i].size() <= MAX_NAME && used.insert(Lower(names[i])).second;

	std::vector<std::string> out(names);
	for (size_t i = 0; i < out.size(); ++i) {
		if (keep[i])
			continue;
		std::string &n = out[i];
		std::string s = n.substr(0, MAX_NAME);
		for (int k = 1; used.count(Lower(s)); ++k) {
			const std::string tail = "~" + std::to_string(k);
			s = n.substr(0, MAX_NAME - tail.size()) + tail;
		}
		used.insert(Lower(s));
		Warn(Format("%s %s is renamed %s: %s", what, n.c_str(), s.c_str(),
			n.size() > MAX_NAME ? "W3D names hold 15 characters" : "another one has that name"));
		n = s;
	}
	return out;
}

const W3xHierarchy *Converter::Find_Hierarchy(const std::string &id)
{
	for (const W3xHierarchy &h : File.hierarchies)
		if (Same(h.id, id))
			return &h;

	// Only .w3x includes hold hierarchies; the .xml ones declare textures.
	for (const std::string &source : File.w3xIncludes) {
		const std::string file = source.substr(source.find_last_of("/\\") + 1);
		std::string text;
		if (!Read_Include || !Read_Include(file, text)) {
			Warn(Format("cannot read include %s", file.c_str()));
			continue;
		}
		Includes.emplace_back();
		std::string error;
		std::vector<std::string> ignored; // the include's other content is not ours to warn about
		if (!W3x_Parse(text.data(), text.size(), Includes.back(), error, ignored)) {
			Includes.pop_back();
			Warn(Format("include %s is not valid: %s", file.c_str(), error.c_str()));
			continue;
		}
		for (const W3xHierarchy &h : Includes.back().hierarchies)
			if (Same(h.id, id))
				return &h;
	}
	Fail(Format("hierarchy %s is not in this file or in its .w3x includes", id.c_str()));
	return nullptr;
}

bool Converter::Write_Hierarchy(const W3xHierarchy &tree)
{
	const char *id = tree.id.c_str();
	std::vector<std::string> names;
	for (int index = 0; index < (int)tree.pivots.size(); ++index) {
		const W3xPivot &pivot = tree.pivots[index];
		// HTreeClass::Base_Update needs pivot 0 as the only root and every parent before its children.
		if ((index == 0) != (pivot.parent < 0) || pivot.parent >= index)
			return Fail(Format("pivot %d (%s) of hierarchy %s has parent %d: W3D needs pivot 0 as the only root and parents before their children",
				index, pivot.name.c_str(), id, pivot.parent));

		// The engine puts pivot 0 at the model's origin and ignores its own transform.
		if (index == 0 && (pivot.translation.Length2() > 1e-8f || fabsf(fabsf(pivot.rotation.W) - 1.0f) > 1e-4f))
			Warn(Format("root pivot %s of hierarchy %s is not at the origin; W3D ignores its transform", pivot.name.c_str(), id));
		names.push_back(pivot.name);
	}
	if (tree.pivots.empty())
		return Fail(Format("hierarchy %s has no pivots", id));
	Tree = &tree;

	names = Short_Names(names, "pivot");

	W.Begin(W3D_CHUNK_HIERARCHY);
	W3dHierarchyStruct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_HTREE_VERSION;
	Copy_Name(header.Name, sizeof(header.Name), Model);
	header.NumPivots = (uint32)tree.pivots.size();
	W.Chunk(W3D_CHUNK_HIERARCHY_HEADER, header);

	W.Begin(W3D_CHUNK_PIVOTS);
	for (size_t i = 0; i < tree.pivots.size(); ++i) {
		const W3xPivot &pivot = tree.pivots[i];
		W3dPivotStruct p;
		memset(&p, 0, sizeof(p));
		Copy_Name(p.Name, sizeof(p.Name), names[i]);
		p.ParentIdx = pivot.parent < 0 ? 0xFFFFFFFF : (uint32)pivot.parent;
		p.Translation = W3d_Vec(pivot.translation);
		p.Rotation.Q[0] = pivot.rotation.X;
		p.Rotation.Q[1] = pivot.rotation.Y;
		p.Rotation.Q[2] = pivot.rotation.Z;
		p.Rotation.Q[3] = pivot.rotation.W;
		W.Put(&p, sizeof(p));
	}
	W.End();
	W.End();
	return true;
}

bool Converter::Write_Mesh(const W3xMesh &mesh, const std::string &mesh_name)
{
	const char *id = mesh.id.c_str();

	uint32 attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_NORMAL;
	if (mesh.geometryType == W3X_GEOMETRY_SKIN)
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN;
	else if (mesh.geometryType == W3X_GEOMETRY_CAMERA_ALIGNED)
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ALIGNED;
	else if (mesh.geometryType == W3X_GEOMETRY_CAMERA_ORIENTED)
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ORIENTED;
	const bool skin = mesh.geometryType == W3X_GEOMETRY_SKIN;

	// A W3D skin vertex follows one bone, so it keeps the block with the heaviest weight.
	W3xMeshVertices vertices;
	std::string error;
	if (!W3x_Single_Bone_Vertices(mesh, Tree, vertices, error))
		return Fail(error);
	const size_t count = vertices.positions.size();
	const std::vector<Vector3> &pos = vertices.positions, &nrm = vertices.normals;
	std::vector<W3dVertInfStruct> links(skin ? count : 0);
	for (size_t i = 0; i < links.size(); ++i) {
		memset(&links[i], 0, sizeof(links[i]));
		links[i].BoneIdx = vertices.bones[i];
	}
	if (vertices.blended)
		Warn(Format("skin mesh %s: %d vertices were blended between bones and now follow only their heaviest bone", id, vertices.blended));

	const Vector3 &lo = vertices.min, &hi = vertices.max, &center = vertices.center;

	// The W3X box comes from the exporter; a mismatch means the vertices are not in the space assumed above.
	if (mesh.hasBounds) {
		const Vector3 d0 = mesh.boundsMin - lo, d1 = mesh.boundsMax - hi;
		const float tolerance = 0.01f * (hi - lo).Length() + 0.01f;
		if (std::max(std::max(fabsf(d0.X), fabsf(d0.Y)), fabsf(d0.Z)) > tolerance || std::max(std::max(fabsf(d1.X), fabsf(d1.Y)), fabsf(d1.Z)) > tolerance)
			Warn(Format("mesh %s: its BoundingBox does not match its vertices", id));
	}

	// No triangle planes are written: the engine rebuilds them from the vertices (OPTIMIZE_PLANEEQ_RAM).
	std::vector<W3dTriStruct> tris;
	for (const W3xTriangle &t : mesh.triangles) {
		W3dTriStruct tri;
		memset(&tri, 0, sizeof(tri));
		for (int corner = 0; corner < 3; ++corner)
			tri.Vindex[corner] = t.v[corner];
		tri.Attributes = SURFACE_TYPE_DEFAULT;
		tris.push_back(tri);
	}

	// Only the first UV set: the second one needs a second texture stage that W3D files here do not carry.
	std::vector<W3dTexCoordStruct> uvs;
	if (!mesh.uvSets.empty())
		for (const Vector2 &t : mesh.uvSets[0])
			uvs.push_back({ t.X, t.Y });
	if (!uvs.empty() && uvs.size() != count) {
		Warn(Format("mesh %s: its TexCoords do not match its vertices and are dropped", id));
		uvs.clear();
	}

	std::vector<W3dRGBAStruct> colors;
	for (const Vector4 &c : mesh.vertexColors)
		colors.push_back({ Color_Byte(c.X), Color_Byte(c.Y), Color_Byte(c.Z), Color_Byte(c.W) });
	if (!colors.empty() && colors.size() != count) {
		Warn(Format("mesh %s: its VertexColors do not match its vertices and are dropped", id));
		colors.clear();
	}

	// The engine has no FX shaders: keep the diffuse texture and the alpha test, blend and culling.
	W3xShading shading;
	W3x_Map_Shader(mesh, !uvs.empty(), shading, Result.Warnings);
	const std::string &diffuse = shading.textureId;
	const W3dShaderStruct &shader = shading.shader;
	const W3dVertexMaterialStruct &material = shading.material;

	if (mesh.castShadow)
		attributes |= W3D_MESH_FLAG_CAST_SHADOW;
	if (mesh.hidden)
		attributes |= W3D_MESH_FLAG_HIDDEN;
	if (shading.twoSided)
		attributes |= W3D_MESH_FLAG_TWO_SIDED;

	W3dMeshHeader3Struct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_MESH_VERSION;
	header.Attributes = attributes;
	Copy_Name(header.MeshName, sizeof(header.MeshName), mesh_name);
	Copy_Name(header.ContainerName, sizeof(header.ContainerName), Model);
	header.NumTris = (uint32)tris.size();
	header.NumVertices = (uint32)count;
	header.NumMaterials = 1;
	header.SortLevel = mesh.sortLevel;
	header.VertexChannels = W3D_VERTEX_CHANNEL_LOCATION | W3D_VERTEX_CHANNEL_NORMAL;
	if (!uvs.empty())
		header.VertexChannels |= W3D_VERTEX_CHANNEL_TEXCOORD;
	if (!colors.empty())
		header.VertexChannels |= W3D_VERTEX_CHANNEL_COLOR;
	if (skin)
		header.VertexChannels |= W3D_VERTEX_CHANNEL_BONEID;
	header.FaceChannels = W3D_FACE_CHANNEL_FACE;
	header.Min = W3d_Vec(lo);
	header.Max = W3d_Vec(hi);
	header.SphCenter = W3d_Vec(center);
	header.SphRadius = vertices.radius;

	// Chunk order matters: the header first, and the materials before the pass that uses them.
	W.Begin(W3D_CHUNK_MESH);
	W.Chunk(W3D_CHUNK_MESH_HEADER3, header);
	W.Chunk(W3D_CHUNK_VERTICES, pos.data(), pos.size() * sizeof(Vector3));
	W.Chunk(W3D_CHUNK_VERTEX_NORMALS, nrm.data(), nrm.size() * sizeof(Vector3));
	W.Chunk(W3D_CHUNK_TRIANGLES, tris.data(), tris.size() * sizeof(W3dTriStruct));
	if (skin)
		W.Chunk(W3D_CHUNK_VERTEX_INFLUENCES, links.data(), links.size() * sizeof(W3dVertInfStruct));

	const W3dMaterialInfoStruct info = { 1, 1, 1, diffuse.empty() ? 0u : 1u };
	W.Chunk(W3D_CHUNK_MATERIAL_INFO, info);
	W.Begin(W3D_CHUNK_VERTEX_MATERIALS);
	W.Begin(W3D_CHUNK_VERTEX_MATERIAL);
	W.Chunk(W3D_CHUNK_VERTEX_MATERIAL_INFO, material);
	W.End();
	W.End();
	W.Begin(W3D_CHUNK_SHADERS);
	W.Put(&shader, sizeof(shader));
	W.End();
	if (!diffuse.empty()) {
		// shortcut: the texture file is <id>.tga; the File of the Texture declared in an .xml include is not read.
		const std::string file = diffuse + ".tga";
		W.Begin(W3D_CHUNK_TEXTURES);
		W.Begin(W3D_CHUNK_TEXTURE);
		W.Chunk(W3D_CHUNK_TEXTURE_NAME, file.c_str(), file.size() + 1);
		W.End();
		W.End();
	}

	const uint32 zero = 0;
	W.Begin(W3D_CHUNK_MATERIAL_PASS);
	W.Chunk(W3D_CHUNK_VERTEX_MATERIAL_IDS, zero);
	W.Chunk(W3D_CHUNK_SHADER_IDS, zero);
	if (!colors.empty())
		W.Chunk(W3D_CHUNK_DCG, colors.data(), colors.size() * sizeof(W3dRGBAStruct));
	if (!diffuse.empty()) {
		W.Begin(W3D_CHUNK_TEXTURE_STAGE);
		W.Chunk(W3D_CHUNK_TEXTURE_IDS, zero);
		// Both W3X and W3D store V upwards; the engine flips it on load.
		W.Chunk(W3D_CHUNK_STAGE_TEXCOORDS, uvs.data(), uvs.size() * sizeof(W3dTexCoordStruct));
		W.End();
	}
	W.End();
	W.End();
	return true;
}

void Converter::Write_Box(const W3xBox &box, const std::string &box_name)
{
	W3dBoxStruct b;
	memset(&b, 0, sizeof(b));
	b.Version = W3D_BOX_CURRENT_VERSION;
	b.Attributes = W3D_BOX_ATTRIBUTE_ORIENTED | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PHYSICAL | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PROJECTILE;
	Copy_Name(b.Name, sizeof(b.Name), Model + "." + box_name);
	b.Color.Set((uint8)255, (uint8)255, (uint8)255);
	b.Center = W3d_Vec(box.center);
	b.Extent = W3d_Vec(box.extent);
	W.Chunk(W3D_CHUNK_BOX, b);
}

bool Converter::Write_Container(const W3xContainer &container, const std::map<std::string, std::string> &sub_names)
{
	std::vector<W3dHLodSubObjectStruct> subs;
	for (const W3xSubObject &s : container.subObjects) {
		const auto found = sub_names.find(Lower(s.renderObjectId));
		// The engine skips a missing sub-object without a word, so a bad reference fails here.
		if (found == sub_names.end())
			return Fail(Format("container sub-object %s names %s, which is not in this file", s.subObjectId.c_str(), s.renderObjectId.c_str()));
		W3dHLodSubObjectStruct sub;
		memset(&sub, 0, sizeof(sub));
		sub.BoneIndex = s.boneIndex;
		if (sub.BoneIndex >= std::max<size_t>(Pivot_Count(), 1))
			return Fail(Format("container sub-object %s is on bone %u, the hierarchy has %d", s.renderObjectId.c_str(), sub.BoneIndex, (int)Pivot_Count()));
		Copy_Name(sub.Name, sizeof(sub.Name), Model + "." + found->second);
		subs.push_back(sub);
	}

	W.Begin(W3D_CHUNK_HLOD);
	W3dHLodHeaderStruct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_HLOD_VERSION;
	header.LodCount = 1;
	Copy_Name(header.Name, sizeof(header.Name), Model);
	if (Tree)
		Copy_Name(header.HierarchyName, sizeof(header.HierarchyName), Model);
	W.Chunk(W3D_CHUNK_HLOD_HEADER, header);
	W.Begin(W3D_CHUNK_HLOD_LOD_ARRAY);
	const W3dHLodArrayHeaderStruct array = { (uint32)subs.size(), FLT_MAX };
	W.Chunk(W3D_CHUNK_HLOD_SUB_OBJECT_ARRAY_HEADER, array);
	for (const W3dHLodSubObjectStruct &sub : subs)
		W.Chunk(W3D_CHUNK_HLOD_SUB_OBJECT, sub);
	W.End();
	W.End();
	return true;
}

bool Converter::Write_Animation(const W3xAnimation &anim)
{
	W.Begin(W3D_CHUNK_ANIMATION);
	W3dAnimHeaderStruct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_HANIM_VERSION;
	Copy_Name(header.Name, sizeof(header.Name), Model);
	Copy_Name(header.HierarchyName, sizeof(header.HierarchyName), Model);
	header.NumFrames = anim.numFrames;
	header.FrameRate = anim.frameRate;
	if (header.NumFrames == 0)
		return Fail(Format("animation %s has no frames", anim.id.c_str()));
	W.Chunk(W3D_CHUNK_ANIMATION_HEADER, header);

	// W3X keys are what HTreeClass::Anim_Update expects: translations are offsets in the pivot's base
	// frame and rotations multiply its base rotation, so they are copied unchanged.
	int binary_moves = 0;
	for (const W3xChannel &c : anim.channels) {
		const bool quat = c.type == W3X_CHANNEL_ORIENTATION;
		uint16 flags;
		if (quat)
			flags = ANIM_CHANNEL_Q;
		else if (c.type == W3X_CHANNEL_X_TRANSLATION)
			flags = ANIM_CHANNEL_X;
		else if (c.type == W3X_CHANNEL_Y_TRANSLATION)
			flags = ANIM_CHANNEL_Y;
		else if (c.type == W3X_CHANNEL_Z_TRANSLATION)
			flags = ANIM_CHANNEL_Z;
		else {
			// W3D has no visibility channel here; the native loader reads it as a bit channel.
			Warn("animation channel ChannelScalar Visibility is not converted");
			continue;
		}

		const unsigned pivot = c.pivot;
		if (pivot >= Pivot_Count()) {
			Warn(Format("animation channel on pivot %u is dropped: the hierarchy has %d pivots", pivot, (int)Pivot_Count()));
			continue;
		}

		for (bool move : c.binaryMove)
			binary_moves += move;
		const unsigned len = quat ? 4 : 1;
		const unsigned first = c.firstFrame;
		const unsigned frames = (unsigned)c.values.size() / len;
		if (frames == 0)
			continue;
		if (first + frames - 1 > 0xFFFF)
			return Fail(Format("animation channel on pivot %u ends after frame 65535", pivot));

		const uint16 channel[6] = { (uint16)first, (uint16)(first + frames - 1), (uint16)len, flags, (uint16)pivot, 0 };
		W.Begin(W3D_CHUNK_ANIMATION_CHANNEL);
		W.Put(channel, sizeof(channel));
		W.Put(c.values.data(), c.values.size() * sizeof(float));
		W.End();
	}
	if (binary_moves)
		Warn(Format("%d BinaryMove keys are interpolated: W3D channels have no step keys", binary_moves));
	W.End();
	return true;
}

bool Converter::Run(const std::string &xml, const std::string &name)
{
	std::string error;
	if (!W3x_Parse(xml.data(), xml.size(), File, error, Result.Warnings))
		return Fail(error);
	const std::vector<W3xMesh> &meshes = File.meshes;
	const std::vector<W3xBox> &boxes = File.boxes;
	const std::vector<W3xContainer> &containers = File.containers;
	const std::vector<W3xAnimation> &anims = File.animations;
	// One file holds one model, named after it, so the engine can load it by name.
	if (containers.size() > 1 || anims.size() > 1)
		return Fail("the file holds more than one container or animation; W3D files here hold one of each");

	std::string id;
	if (!containers.empty())
		id = containers[0].id;
	else if (!anims.empty())
		id = anims[0].id;
	else if (!File.hierarchies.empty())
		id = File.hierarchies[0].id;
	else if (!meshes.empty()) {
		id = meshes[0].id;
		id = id.substr(0, id.rfind('.'));
	} else
		return Fail("the file holds no hierarchy, mesh, container or animation");

	Model = name.empty() ? id : name;
	if (Model.empty() || Model.size() > MAX_NAME || Model.find('.') != std::string::npos)
		return Fail(Format("model name %s does not fit W3D: it needs 1 to 15 characters and no dot; give a short model name", Model.c_str()));
	Result.Name = Model;

	std::string hierarchy = !containers.empty() ? containers[0].hierarchy : "";
	if (!anims.empty()) {
		const std::string &anim_hierarchy = anims[0].hierarchy;
		if (!hierarchy.empty() && !Same(hierarchy, anim_hierarchy))
			return Fail(Format("the container uses hierarchy %s and the animation %s", hierarchy.c_str(), anim_hierarchy.c_str()));
		hierarchy = anim_hierarchy;
	}
	if (hierarchy.empty() && !File.hierarchies.empty())
		hierarchy = File.hierarchies[0].id;
	// HRawAnimClass::Load_W3D fails without the HTree it names.
	if (hierarchy.empty() && !anims.empty())
		return Fail(Format("animation %s names no hierarchy", anims[0].id.c_str()));

	if (!hierarchy.empty()) {
		const W3xHierarchy *tree = Find_Hierarchy(hierarchy);
		if (!tree || !Write_Hierarchy(*tree))
			return false;
	}

	std::vector<std::string> sub_ids, sub_names;
	for (const W3xMesh &m : meshes)
		sub_ids.push_back(m.id);
	for (const W3xBox &b : boxes)
		sub_ids.push_back(b.id);
	for (const std::string &s : sub_ids)
		sub_names.push_back(Sub_Name(s));
	sub_names = Short_Names(sub_names, "mesh");
	std::map<std::string, std::string> by_id;
	for (size_t i = 0; i < sub_ids.size(); ++i)
		if (!by_id.emplace(Lower(sub_ids[i]), sub_names[i]).second)
			return Fail(Format("two meshes or boxes have id %s", sub_ids[i].c_str()));

	for (size_t i = 0; i < meshes.size(); ++i)
		if (!Write_Mesh(meshes[i], sub_names[i]))
			return false;
	for (size_t i = 0; i < boxes.size(); ++i)
		Write_Box(boxes[i], sub_names[meshes.size() + i]);
	if (!containers.empty() && !Write_Container(containers[0], by_id))
		return false;
	if (!anims.empty() && !Write_Animation(anims[0]))
		return false;

	Result.W3d.swap(W.Bytes);
	return true;
}
} // namespace

bool Convert_W3X(const std::string &xml, const std::string &name, const W3xIncludeReader &read_include,
	W3xConvertResult &result)
{
	result = W3xConvertResult();
	Converter converter(result, read_include);
	if (converter.Run(xml, name))
		return true;
	result.W3d.clear();
	return false;
}
