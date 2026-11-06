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

#include "w3d_file.h"
#include "WWMath/quat.h"
#include "WWMath/vector3.h"

#include <pugixml.hpp>

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

Vector3 Read_Vec(const pugi::xml_node &n)
{
	return Vector3(n.attribute("X").as_float(), n.attribute("Y").as_float(), n.attribute("Z").as_float());
}

Quaternion Read_Quat(const pugi::xml_node &n)
{
	return Quaternion(n.attribute("X").as_float(), n.attribute("Y").as_float(), n.attribute("Z").as_float(),
		n.attribute("W").as_float(1.0f));
}

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

struct Pivot
{
	std::string Name;
	int Parent;
	Vector3 T;
	Quaternion Q;
	Vector3 WorldT; // the bind pose in model space
	Quaternion WorldQ;
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
	bool Find_Hierarchy(pugi::xml_node root, const std::string &id, pugi::xml_node &found);
	bool Write_Hierarchy(pugi::xml_node node);
	bool Write_Mesh(pugi::xml_node mesh, const std::string &mesh_name);
	void Write_Box(pugi::xml_node box, const std::string &box_name);
	bool Write_Container(pugi::xml_node container, const std::map<std::string, std::string> &sub_names);
	bool Write_Animation(pugi::xml_node anim);

	W3xConvertResult &Result;
	const W3xIncludeReader &Read_Include;
	std::list<pugi::xml_document> Includes;
	ChunkWriter W;
	std::string Model;
	std::vector<Pivot> Pivots;
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

bool Converter::Find_Hierarchy(pugi::xml_node root, const std::string &id, pugi::xml_node &found)
{
	for (pugi::xml_node h : root.children("W3DHierarchy")) {
		if (Same(h.attribute("id").value(), id.c_str())) {
			found = h;
			return true;
		}
	}

	for (pugi::xml_node inc : root.child("Includes").children("Include")) {
		std::string file = inc.attribute("source").value();
		if (_strnicmp(file.c_str(), "ART:", 4) == 0)
			file = file.substr(4);
		file = file.substr(file.find_last_of("/\\") + 1);
		// Only .w3x includes hold hierarchies; the .xml ones declare textures.
		if (file.size() < 4 || !Same(file.substr(file.size() - 4), ".w3x"))
			continue;

		std::string text;
		if (!Read_Include || !Read_Include(file, text)) {
			Warn(Format("cannot read include %s", file.c_str()));
			continue;
		}
		Includes.emplace_back();
		if (!Includes.back().load_buffer(text.data(), text.size())) {
			Warn(Format("include %s is not valid XML", file.c_str()));
			continue;
		}
		for (pugi::xml_node h : Includes.back().document_element().children("W3DHierarchy")) {
			if (Same(h.attribute("id").value(), id.c_str())) {
				found = h;
				return true;
			}
		}
	}
	return Fail(Format("hierarchy %s is not in this file or in its .w3x includes", id.c_str()));
}

bool Converter::Write_Hierarchy(pugi::xml_node node)
{
	const char *id = node.attribute("id").value();
	std::vector<std::string> names;
	for (pugi::xml_node p : node.children("Pivot")) {
		Pivot pivot;
		pivot.Name = p.attribute("Name").value();
		pivot.Parent = p.attribute("Parent").as_int(-1);
		pivot.T = Read_Vec(p.child("Translation"));
		pivot.Q = Read_Quat(p.child("Rotation"));

		const int index = (int)Pivots.size();
		// HTreeClass::Base_Update needs pivot 0 as the only root and every parent before its children.
		if ((index == 0) != (pivot.Parent < 0) || pivot.Parent >= index)
			return Fail(Format("pivot %d (%s) of hierarchy %s has parent %d: W3D needs pivot 0 as the only root and parents before their children",
				index, pivot.Name.c_str(), id, pivot.Parent));

		if (index == 0) {
			// The engine puts pivot 0 at the model's origin and ignores its own transform.
			if (pivot.T.Length2() > 1e-8f || fabsf(fabsf(pivot.Q.W) - 1.0f) > 1e-4f)
				Warn(Format("root pivot %s of hierarchy %s is not at the origin; W3D ignores its transform", pivot.Name.c_str(), id));
			pivot.WorldT.Set(0, 0, 0);
			pivot.WorldQ.Make_Identity();
		} else {
			const Pivot &parent = Pivots[pivot.Parent];
			pivot.WorldT = parent.WorldT + parent.WorldQ.Rotate_Vector(pivot.T);
			pivot.WorldQ = parent.WorldQ * pivot.Q;
		}
		Pivots.push_back(pivot);
		names.push_back(pivot.Name);
	}
	if (Pivots.empty())
		return Fail(Format("hierarchy %s has no pivots", id));

	names = Short_Names(names, "pivot");

	W.Begin(W3D_CHUNK_HIERARCHY);
	W3dHierarchyStruct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_HTREE_VERSION;
	Copy_Name(header.Name, sizeof(header.Name), Model);
	header.NumPivots = (uint32)Pivots.size();
	W.Chunk(W3D_CHUNK_HIERARCHY_HEADER, header);

	W.Begin(W3D_CHUNK_PIVOTS);
	for (size_t i = 0; i < Pivots.size(); ++i) {
		W3dPivotStruct p;
		memset(&p, 0, sizeof(p));
		Copy_Name(p.Name, sizeof(p.Name), names[i]);
		p.ParentIdx = Pivots[i].Parent < 0 ? 0xFFFFFFFF : (uint32)Pivots[i].Parent;
		p.Translation = W3d_Vec(Pivots[i].T);
		p.Rotation.Q[0] = Pivots[i].Q.X;
		p.Rotation.Q[1] = Pivots[i].Q.Y;
		p.Rotation.Q[2] = Pivots[i].Q.Z;
		p.Rotation.Q[3] = Pivots[i].Q.W;
		W.Put(&p, sizeof(p));
	}
	W.End();
	W.End();
	return true;
}

bool Converter::Write_Mesh(pugi::xml_node mesh, const std::string &mesh_name)
{
	const char *id = mesh.attribute("id").value();

	std::vector<std::vector<Vector3>> verts, norms;
	std::vector<std::vector<std::pair<unsigned, float>>> influences;
	for (pugi::xml_node b : mesh.children("Vertices")) {
		verts.emplace_back();
		for (pugi::xml_node v : b.children("V"))
			verts.back().push_back(Read_Vec(v));
	}
	for (pugi::xml_node b : mesh.children("Normals")) {
		norms.emplace_back();
		for (pugi::xml_node n : b.children("N"))
			norms.back().push_back(Read_Vec(n));
	}
	for (pugi::xml_node b : mesh.children("BoneInfluences")) {
		influences.emplace_back();
		for (pugi::xml_node i : b.children("I"))
			influences.back().push_back({ i.attribute("Bone").as_uint(), i.attribute("Weight").as_float() });
	}
	if (verts.empty() || verts[0].empty())
		return Fail(Format("mesh %s has no vertices", id));
	if (norms.empty())
		return Fail(Format("mesh %s has no normals", id));

	const size_t count = verts[0].size();
	for (const auto &b : verts)
		if (b.size() != count)
			return Fail(Format("mesh %s: its Vertices blocks differ in length", id));
	for (const auto &b : norms)
		if (b.size() != count)
			return Fail(Format("mesh %s: its Normals do not match its vertices", id));
	for (const auto &b : influences)
		if (b.size() != count)
			return Fail(Format("mesh %s: its BoneInfluences do not match its vertices", id));

	const char *type = mesh.attribute("GeometryType").value();
	uint32 attributes;
	if (!*type || Same(type, "Normal"))
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_NORMAL;
	else if (Same(type, "Skin"))
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN;
	else if (Same(type, "CameraAligned"))
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ALIGNED;
	else if (Same(type, "CameraOriented"))
		attributes = W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ORIENTED;
	else
		return Fail(Format("mesh %s has unknown GeometryType %s", id, type));
	const bool skin = attributes == W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN;

	// Vertex k of block b is expressed in the space of block b's bone. A W3D skin vertex follows one
	// bone, so it keeps the block with the heaviest weight, already in that bone's space.
	std::vector<Vector3> pos(count), nrm(count), model_pos(count);
	std::vector<W3dVertInfStruct> links(skin ? count : 0);
	if (skin) {
		if (influences.empty())
			return Fail(Format("skin mesh %s has no BoneInfluences", id));
		if (Pivots.empty())
			return Fail(Format("skin mesh %s has no hierarchy", id));
	}
	const size_t blocks = std::min(std::min(verts.size(), norms.size()), skin ? influences.size() : size_t(1));
	int blended = 0;
	for (size_t i = 0; i < count; ++i) {
		size_t k = 0;
		if (skin) {
			for (size_t b = 1; b < blocks; ++b)
				if (influences[b][i].second > influences[k][i].second)
					k = b;
			for (size_t b = 0; b < influences.size(); ++b)
				if (b != k && influences[b][i].second > 1e-4f && influences[b][i].first != influences[k][i].first) {
					++blended;
					break;
				}
		}
		pos[i] = verts[k][i];
		nrm[i] = norms[k][i];
		model_pos[i] = pos[i];
		if (skin) {
			const unsigned bone = influences[k][i].first;
			if (bone >= Pivots.size())
				return Fail(Format("skin mesh %s: vertex %d follows bone %u, the hierarchy has %d", id, (int)i, bone, (int)Pivots.size()));
			memset(&links[i], 0, sizeof(links[i]));
			links[i].BoneIdx = (uint16)bone;
			model_pos[i] = Pivots[bone].WorldT + Pivots[bone].WorldQ.Rotate_Vector(pos[i]);
		}
	}
	if (blended)
		Warn(Format("skin mesh %s: %d vertices were blended between bones and now follow only their heaviest bone", id, blended));

	// Bounds in mesh space; for a skin that is the model space of the bind pose.
	Vector3 lo = model_pos[0], hi = model_pos[0];
	for (const Vector3 &p : model_pos) {
		lo.Update_Min(p);
		hi.Update_Max(p);
	}
	const Vector3 center = (lo + hi) * 0.5f;
	float radius2 = 0.0f;
	for (const Vector3 &p : model_pos)
		radius2 = std::max(radius2, (p - center).Length2());

	// The W3X box comes from the exporter; a mismatch means the vertices are not in the space assumed above.
	if (pugi::xml_node box = mesh.child("BoundingBox")) {
		const Vector3 d0 = Read_Vec(box.child("Min")) - lo, d1 = Read_Vec(box.child("Max")) - hi;
		const float tolerance = 0.01f * (hi - lo).Length() + 0.01f;
		if (std::max(std::max(fabsf(d0.X), fabsf(d0.Y)), fabsf(d0.Z)) > tolerance || std::max(std::max(fabsf(d1.X), fabsf(d1.Y)), fabsf(d1.Z)) > tolerance)
			Warn(Format("mesh %s: its BoundingBox does not match its vertices", id));
	}

	// No triangle planes are written: the engine rebuilds them from the vertices (OPTIMIZE_PLANEEQ_RAM).
	std::vector<W3dTriStruct> tris;
	for (pugi::xml_node t : mesh.child("Triangles").children("T")) {
		W3dTriStruct tri;
		memset(&tri, 0, sizeof(tri));
		int corners = 0;
		bool bad = false;
		for (pugi::xml_node v : t.children("V")) {
			const unsigned index = v.text().as_uint();
			// as_uint reads "-1" and "abc" as 0.
			bad |= corners >= 3 || index >= count || !isdigit((unsigned char)*v.text().get());
			if (corners < 3)
				tri.Vindex[corners] = index;
			++corners;
		}
		if (bad || corners != 3)
			return Fail(Format("mesh %s: triangle %d has bad vertex indices", id, (int)tris.size()));
		tri.Attributes = SURFACE_TYPE_DEFAULT;
		tris.push_back(tri);
	}
	if (tris.empty())
		return Fail(Format("mesh %s has no triangles", id));

	std::vector<W3dTexCoordStruct> uvs;
	for (pugi::xml_node t : mesh.child("TexCoords").children("T"))
		uvs.push_back({ t.attribute("X").as_float(), t.attribute("Y").as_float() });
	if (!uvs.empty() && uvs.size() != count) {
		Warn(Format("mesh %s: its TexCoords do not match its vertices and are dropped", id));
		uvs.clear();
	}

	std::vector<W3dRGBAStruct> colors;
	for (pugi::xml_node c : mesh.child("VertexColors").children("C")) {
		auto byte = [&](const char *channel) { return Color_Byte(c.attribute(channel).as_float(1.0f)); };
		colors.push_back({ byte("R"), byte("G"), byte("B"), byte("A") });
	}
	if (!colors.empty() && colors.size() != count) {
		Warn(Format("mesh %s: its VertexColors do not match its vertices and are dropped", id));
		colors.clear();
	}

	// The engine has no FX shaders: keep the diffuse texture and the alpha test, blend and culling.
	// It finds the normal and spec maps itself, as <diffuse>_nrm and <diffuse>_spm.
	std::string diffuse, normal_map, spec_map;
	bool alpha_test = false, two_sided = false, depth_write = true;
	int blend = 0;
	pugi::xml_node fx = mesh.child("FXShader");
	for (pugi::xml_node c : fx.child("Constants").children()) {
		const char *cname = c.attribute("Name").value();
		const pugi::xml_text value = c.child("Value").text();
		if (Same(c.name(), "Texture")) {
			if (Same(cname, "DiffuseTexture") || Same(cname, "Texture_0"))
				diffuse = value.get();
			else if (Same(cname, "NormalMap"))
				normal_map = value.get();
			else if (Same(cname, "SpecMap"))
				spec_map = value.get();
			else if (*value.get() && !Same(value.get(), diffuse.c_str()))
				Warn(Format("mesh %s: texture %s (%s) is not converted", id, cname, value.get()));
		} else if (Same(cname, "AlphaTestEnable"))
			alpha_test = value.as_bool();
		else if (Same(cname, "CullingEnable"))
			two_sided = !value.as_bool();
		else if (Same(cname, "DepthWriteEnable"))
			depth_write = value.as_bool();
		else if (Same(cname, "BlendMode"))
			blend = value.as_int();
		else if (Same(cname, "AlphaBlendingEnable") && value.as_bool())
			blend = 1;
	}
	if (!normal_map.empty() && !Same(normal_map, diffuse + "_NRM"))
		Warn(Format("mesh %s: NormalMap %s is not named %s_NRM, so the engine will not find it", id, normal_map.c_str(), diffuse.c_str()));
	if (!spec_map.empty() && !Same(spec_map, diffuse + "_SPM"))
		Warn(Format("mesh %s: SpecMap %s is not named %s_SPM, so the engine will not find it", id, spec_map.c_str(), diffuse.c_str()));
	if (!diffuse.empty() && uvs.empty()) {
		Warn(Format("mesh %s: texture %s is dropped because the mesh has no TexCoords", id, diffuse.c_str()));
		diffuse.clear();
	}

	W3dShaderStruct shader;
	W3d_Shader_Reset(&shader);
	shader.Texturing = diffuse.empty() ? W3DSHADER_TEXTURING_DISABLE : W3DSHADER_TEXTURING_ENABLE;
	shader.AlphaTest = alpha_test ? W3DSHADER_ALPHATEST_ENABLE : W3DSHADER_ALPHATEST_DISABLE;
	shader.DepthMask = depth_write ? W3DSHADER_DEPTHMASK_WRITE_ENABLE : W3DSHADER_DEPTHMASK_WRITE_DISABLE;
	// DefaultW3D.fx BlendMode: 0 opaque, 1 alpha blend, 2 additive.
	if (blend == 1) {
		shader.SrcBlend = W3DSHADER_SRCBLENDFUNC_SRC_ALPHA;
		shader.DestBlend = W3DSHADER_DESTBLENDFUNC_ONE_MINUS_SRC_ALPHA;
	} else if (blend == 2) {
		shader.SrcBlend = W3DSHADER_SRCBLENDFUNC_ONE;
		shader.DestBlend = W3DSHADER_DESTBLENDFUNC_ONE;
	} else if (blend != 0)
		Warn(Format("mesh %s: unknown BlendMode %d is drawn opaque", id, blend));

	W3dVertexMaterialStruct material;
	memset(&material, 0, sizeof(material));
	W3d_Vertex_Material_Reset(&material);
	// Only the DefaultW3D.fx colours are fixed-function material colours; other shaders keep the reset values.
	if (Same(fx.attribute("ShaderName").value(), "DefaultW3D.fx")) {
		for (pugi::xml_node c : fx.child("Constants").children("Float")) {
			const char *cname = c.attribute("Name").value();
			float v[3] = { 0, 0, 0 };
			int n = 0;
			for (pugi::xml_node value : c.children("Value"))
				if (n < 3)
					v[n++] = value.text().as_float();
			W3dRGBStruct *color = Same(cname, "ColorAmbient") ? &material.Ambient
				: Same(cname, "ColorDiffuse") ? &material.Diffuse
				: Same(cname, "ColorSpecular") ? &material.Specular
				: Same(cname, "ColorEmissive") ? &material.Emissive
				: nullptr;
			// Set keeps the pad byte that the memset cleared.
			if (color && n == 3)
				color->Set(Color_Byte(v[0]), Color_Byte(v[1]), Color_Byte(v[2]));
			else if (Same(cname, "Shininess") && n == 1)
				material.Shininess = v[0];
			else if (Same(cname, "Opacity") && n == 1)
				material.Opacity = v[0];
		}
	}

	if (mesh.attribute("CastShadow").as_bool())
		attributes |= W3D_MESH_FLAG_CAST_SHADOW;
	if (mesh.attribute("Hidden").as_bool())
		attributes |= W3D_MESH_FLAG_HIDDEN;
	if (two_sided)
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
	header.SortLevel = mesh.attribute("SortLevel").as_int();
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
	header.SphRadius = sqrtf(radius2);

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

void Converter::Write_Box(pugi::xml_node box, const std::string &box_name)
{
	W3dBoxStruct b;
	memset(&b, 0, sizeof(b));
	b.Version = W3D_BOX_CURRENT_VERSION;
	b.Attributes = W3D_BOX_ATTRIBUTE_ORIENTED | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PHYSICAL | W3D_BOX_ATTRIBUTE_COLLISION_TYPE_PROJECTILE;
	Copy_Name(b.Name, sizeof(b.Name), Model + "." + box_name);
	b.Color.Set((uint8)255, (uint8)255, (uint8)255);
	b.Center = W3d_Vec(Read_Vec(box.child("Center")));
	b.Extent = W3d_Vec(Read_Vec(box.child("Extent")));
	W.Chunk(W3D_CHUNK_BOX, b);
}

bool Converter::Write_Container(pugi::xml_node container, const std::map<std::string, std::string> &sub_names)
{
	std::vector<W3dHLodSubObjectStruct> subs;
	for (pugi::xml_node s : container.children("SubObject")) {
		const pugi::xml_node ref = s.child("RenderObject").first_child();
		const auto found = sub_names.find(Lower(ref.text().get()));
		// The engine skips a missing sub-object without a word, so a bad reference fails here.
		if (found == sub_names.end())
			return Fail(Format("container sub-object %s names %s, which is not in this file", s.attribute("SubObjectID").value(), ref.text().get()));
		W3dHLodSubObjectStruct sub;
		memset(&sub, 0, sizeof(sub));
		sub.BoneIndex = s.attribute("BoneIndex").as_uint();
		if (sub.BoneIndex >= std::max<size_t>(Pivots.size(), 1))
			return Fail(Format("container sub-object %s is on bone %u, the hierarchy has %d", ref.text().get(), sub.BoneIndex, (int)Pivots.size()));
		Copy_Name(sub.Name, sizeof(sub.Name), Model + "." + found->second);
		subs.push_back(sub);
	}

	W.Begin(W3D_CHUNK_HLOD);
	W3dHLodHeaderStruct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_HLOD_VERSION;
	header.LodCount = 1;
	Copy_Name(header.Name, sizeof(header.Name), Model);
	if (!Pivots.empty())
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

bool Converter::Write_Animation(pugi::xml_node anim)
{
	W.Begin(W3D_CHUNK_ANIMATION);
	W3dAnimHeaderStruct header;
	memset(&header, 0, sizeof(header));
	header.Version = W3D_CURRENT_HANIM_VERSION;
	Copy_Name(header.Name, sizeof(header.Name), Model);
	Copy_Name(header.HierarchyName, sizeof(header.HierarchyName), Model);
	header.NumFrames = anim.attribute("NumFrames").as_uint();
	header.FrameRate = anim.attribute("FrameRate").as_uint(30);
	if (header.NumFrames == 0)
		return Fail(Format("animation %s has no frames", anim.attribute("id").value()));
	W.Chunk(W3D_CHUNK_ANIMATION_HEADER, header);

	// W3X keys are what HTreeClass::Anim_Update expects: translations are offsets in the pivot's base
	// frame and rotations multiply its base rotation, so they are copied unchanged.
	int binary_moves = 0;
	for (pugi::xml_node c : anim.child("Channels").children()) {
		const bool quat = Same(c.name(), "ChannelQuaternion");
		const char *type = c.attribute("Type").value();
		uint16 flags;
		if (quat && Same(type, "Orientation"))
			flags = ANIM_CHANNEL_Q;
		else if (!quat && Same(c.name(), "ChannelScalar") && Same(type, "XTranslation"))
			flags = ANIM_CHANNEL_X;
		else if (!quat && Same(c.name(), "ChannelScalar") && Same(type, "YTranslation"))
			flags = ANIM_CHANNEL_Y;
		else if (!quat && Same(c.name(), "ChannelScalar") && Same(type, "ZTranslation"))
			flags = ANIM_CHANNEL_Z;
		else {
			// shortcut: Visibility channels are dropped; no test art has one to check the bit channel against.
			Warn(Format("animation channel %s %s is not converted", c.name(), type));
			continue;
		}

		const unsigned pivot = c.attribute("Pivot").as_uint();
		if (pivot >= Pivots.size()) {
			Warn(Format("animation channel on pivot %u is dropped: the hierarchy has %d pivots", pivot, (int)Pivots.size()));
			continue;
		}

		std::vector<float> data;
		for (pugi::xml_node f : c.children("Frame")) {
			if (quat) {
				const Quaternion q = Read_Quat(f);
				data.insert(data.end(), { q.X, q.Y, q.Z, q.W });
			} else
				data.push_back(f.text().as_float());
			binary_moves += f.attribute("BinaryMove").as_bool();
		}
		const unsigned len = quat ? 4 : 1;
		const unsigned first = c.attribute("FirstFrame").as_uint();
		const unsigned frames = (unsigned)data.size() / len;
		if (frames == 0)
			continue;
		if (first + frames - 1 > 0xFFFF)
			return Fail(Format("animation channel on pivot %u ends after frame 65535", pivot));

		const uint16 channel[6] = { (uint16)first, (uint16)(first + frames - 1), (uint16)len, flags, (uint16)pivot, 0 };
		W.Begin(W3D_CHUNK_ANIMATION_CHANNEL);
		W.Put(channel, sizeof(channel));
		W.Put(data.data(), data.size() * sizeof(float));
		W.End();
	}
	if (binary_moves)
		Warn(Format("%d BinaryMove keys are interpolated: W3D channels have no step keys", binary_moves));
	W.End();
	return true;
}

bool Converter::Run(const std::string &xml, const std::string &name)
{
	pugi::xml_document doc;
	const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
	if (!parsed)
		return Fail(Format("XML error at byte %d: %s", (int)parsed.offset, parsed.description()));
	const pugi::xml_node root = doc.document_element();
	if (!Same(root.name(), "AssetDeclaration"))
		return Fail(Format("<%s> is not a W3X AssetDeclaration", root.name()));

	std::vector<pugi::xml_node> meshes, boxes, containers, anims;
	for (pugi::xml_node n : root.children()) {
		if (Same(n.name(), "W3DMesh"))
			meshes.push_back(n);
		else if (Same(n.name(), "W3DCollisionBox"))
			boxes.push_back(n);
		else if (Same(n.name(), "W3DContainer"))
			containers.push_back(n);
		else if (Same(n.name(), "W3DAnimation"))
			anims.push_back(n);
		else if (!Same(n.name(), "W3DHierarchy") && !Same(n.name(), "Includes"))
			Warn(Format("<%s> is not converted", n.name()));
	}
	// One file holds one model, named after it, so the engine can load it by name.
	if (containers.size() > 1 || anims.size() > 1)
		return Fail("the file holds more than one container or animation; W3D files here hold one of each");

	std::string id;
	if (!containers.empty())
		id = containers[0].attribute("id").value();
	else if (!anims.empty())
		id = anims[0].attribute("id").value();
	else if (root.child("W3DHierarchy"))
		id = root.child("W3DHierarchy").attribute("id").value();
	else if (!meshes.empty()) {
		id = meshes[0].attribute("id").value();
		id = id.substr(0, id.rfind('.'));
	} else
		return Fail("the file holds no hierarchy, mesh, container or animation");

	Model = name.empty() ? id : name;
	if (Model.empty() || Model.size() > MAX_NAME || Model.find('.') != std::string::npos)
		return Fail(Format("model name %s does not fit W3D: it needs 1 to 15 characters and no dot; give a short model name", Model.c_str()));
	Result.Name = Model;

	std::string hierarchy = !containers.empty() ? containers[0].attribute("Hierarchy").value() : "";
	if (!anims.empty()) {
		const std::string anim_hierarchy = anims[0].attribute("Hierarchy").value();
		if (!hierarchy.empty() && !Same(hierarchy, anim_hierarchy))
			return Fail(Format("the container uses hierarchy %s and the animation %s", hierarchy.c_str(), anim_hierarchy.c_str()));
		hierarchy = anim_hierarchy;
	}
	if (hierarchy.empty() && root.child("W3DHierarchy"))
		hierarchy = root.child("W3DHierarchy").attribute("id").value();
	// HRawAnimClass::Load_W3D fails without the HTree it names.
	if (hierarchy.empty() && !anims.empty())
		return Fail(Format("animation %s names no hierarchy", anims[0].attribute("id").value()));

	if (!hierarchy.empty()) {
		pugi::xml_node node;
		if (!Find_Hierarchy(root, hierarchy, node) || !Write_Hierarchy(node))
			return false;
	}

	std::vector<std::string> sub_ids, sub_names;
	for (const pugi::xml_node &n : meshes)
		sub_ids.push_back(n.attribute("id").value());
	for (const pugi::xml_node &n : boxes)
		sub_ids.push_back(n.attribute("id").value());
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
