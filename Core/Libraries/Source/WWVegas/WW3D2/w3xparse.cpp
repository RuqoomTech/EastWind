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

#include "w3xparse.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
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

std::string Lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
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

bool Parse_Hierarchy(const pugi::xml_node &node, W3xHierarchy &h)
{
	h.id = node.attribute("id").value();
	for (pugi::xml_node p : node.children("Pivot")) {
		W3xPivot pivot;
		pivot.name = p.attribute("Name").value();
		pivot.parent = p.attribute("Parent").as_int(-1);
		pivot.translation = Read_Vec(p.child("Translation"));
		pivot.rotation = Read_Quat(p.child("Rotation"));
		h.pivots.push_back(pivot);
	}
	return true;
}

bool Parse_Mesh(const pugi::xml_node &node, W3xMesh &m, std::string &error)
{
	m.id = node.attribute("id").value();
	const char *id = m.id.c_str();

	std::vector<std::vector<Vector3>> verts, norms;
	std::vector<std::vector<uint16>> bones;
	std::vector<std::vector<float>> weights;
	for (pugi::xml_node b : node.children("Vertices")) {
		verts.emplace_back();
		for (pugi::xml_node v : b.children("V"))
			verts.back().push_back(Read_Vec(v));
	}
	for (pugi::xml_node b : node.children("Normals")) {
		norms.emplace_back();
		for (pugi::xml_node n : b.children("N"))
			norms.back().push_back(Read_Vec(n));
	}
	for (pugi::xml_node b : node.children("BoneInfluences")) {
		bones.emplace_back();
		weights.emplace_back();
		for (pugi::xml_node i : b.children("I")) {
			// Saturates: a hierarchy never has 65535 bones, so the bone range check still catches it.
			bones.back().push_back((uint16)std::min<unsigned>(i.attribute("Bone").as_uint(), 0xFFFF));
			weights.back().push_back(i.attribute("Weight").as_float());
		}
	}
	if (verts.empty() || verts[0].empty()) {
		error = Format("mesh %s has no vertices", id);
		return false;
	}
	if (norms.empty()) {
		error = Format("mesh %s has no normals", id);
		return false;
	}

	const size_t count = verts[0].size();
	for (const auto &b : verts)
		if (b.size() != count) {
			error = Format("mesh %s: its Vertices blocks differ in length", id);
			return false;
		}
	for (const auto &b : norms)
		if (b.size() != count) {
			error = Format("mesh %s: its Normals do not match its vertices", id);
			return false;
		}
	for (const auto &b : bones)
		if (b.size() != count) {
			error = Format("mesh %s: its BoneInfluences do not match its vertices", id);
			return false;
		}

	const char *type = node.attribute("GeometryType").value();
	if (!*type || Same(type, "Normal"))
		m.geometryType = W3X_GEOMETRY_NORMAL;
	else if (Same(type, "Skin"))
		m.geometryType = W3X_GEOMETRY_SKIN;
	else if (Same(type, "CameraAligned"))
		m.geometryType = W3X_GEOMETRY_CAMERA_ALIGNED;
	else if (Same(type, "CameraOriented"))
		m.geometryType = W3X_GEOMETRY_CAMERA_ORIENTED;
	else {
		error = Format("mesh %s has unknown GeometryType %s", id, type);
		return false;
	}
	if (m.geometryType == W3X_GEOMETRY_SKIN && bones.empty()) {
		error = Format("skin mesh %s has no BoneInfluences", id);
		return false;
	}

	m.blocks.resize(std::max(std::max(verts.size(), norms.size()), bones.size()));
	for (size_t k = 0; k < m.blocks.size(); ++k) {
		if (k < verts.size())
			m.blocks[k].positions.swap(verts[k]);
		if (k < norms.size())
			m.blocks[k].normals.swap(norms[k]);
		if (k < bones.size()) {
			m.blocks[k].bones.swap(bones[k]);
			m.blocks[k].weights.swap(weights[k]);
		}
	}

	m.castShadow = node.attribute("CastShadow").as_bool();
	m.hidden = node.attribute("Hidden").as_bool();
	m.sortLevel = node.attribute("SortLevel").as_int();

	if (pugi::xml_node box = node.child("BoundingBox")) {
		m.hasBounds = true;
		m.boundsMin = Read_Vec(box.child("Min"));
		m.boundsMax = Read_Vec(box.child("Max"));
	}

	for (pugi::xml_node set : node.children("TexCoords")) {
		m.uvSets.emplace_back();
		for (pugi::xml_node t : set.children("T"))
			m.uvSets.back().push_back(Vector2(t.attribute("X").as_float(), t.attribute("Y").as_float()));
	}

	for (pugi::xml_node c : node.child("VertexColors").children("C"))
		m.vertexColors.push_back(Vector4(c.attribute("R").as_float(1.0f), c.attribute("G").as_float(1.0f),
			c.attribute("B").as_float(1.0f), c.attribute("A").as_float(1.0f)));

	for (pugi::xml_node t : node.child("Triangles").children("T")) {
		W3xTriangle tri;
		memset(&tri, 0, sizeof(tri));
		int corners = 0;
		bool bad = false;
		for (pugi::xml_node v : t.children("V")) {
			const unsigned index = v.text().as_uint();
			// as_uint reads "-1" and "abc" as 0.
			bad |= corners >= 3 || index >= count || !isdigit((unsigned char)*v.text().get());
			if (corners < 3)
				tri.v[corners] = index;
			++corners;
		}
		if (bad || corners != 3) {
			error = Format("mesh %s: triangle %d has bad vertex indices", id, (int)m.triangles.size());
			return false;
		}
		m.triangles.push_back(tri);
	}
	if (m.triangles.empty()) {
		error = Format("mesh %s has no triangles", id);
		return false;
	}

	pugi::xml_node fx = node.child("FXShader");
	W3xMaterial &mat = m.material;
	mat.shaderName = fx.attribute("ShaderName").value();
	mat.techniqueIndex = fx.attribute("TechniqueIndex").as_int();
	for (pugi::xml_node c : fx.child("Constants").children()) {
		W3xConstant k;
		k.kind = Same(c.name(), "Texture") ? W3xConstant::Texture
			: Same(c.name(), "Float") ? W3xConstant::Float
			: Same(c.name(), "Int") ? W3xConstant::Int
			: Same(c.name(), "Bool") ? W3xConstant::Bool
			: W3xConstant::Other;
		k.name = c.attribute("Name").value();
		const pugi::xml_text value = c.child("Value").text();
		k.text = value.get();
		k.asInt = value.as_int();
		k.asBool = value.as_bool();
		if (k.kind != W3xConstant::Texture)
			for (pugi::xml_node v : c.children("Value"))
				k.values.push_back(v.text().as_float());

		if (k.kind == W3xConstant::Texture)
			mat.textures[k.name] = k.text;
		else if (k.kind == W3xConstant::Float)
			mat.floats[k.name] = k.values;
		else if (k.kind == W3xConstant::Int)
			mat.ints[k.name] = k.asInt;
		else if (k.kind == W3xConstant::Bool)
			mat.bools[k.name] = k.asBool;
		mat.constants.push_back(k);
	}
	return true;
}

void Parse_Animation(const pugi::xml_node &node, W3xAnimation &a, std::vector<std::string> &warnings)
{
	a.id = node.attribute("id").value();
	a.hierarchy = node.attribute("Hierarchy").value();
	a.numFrames = node.attribute("NumFrames").as_uint();
	a.frameRate = node.attribute("FrameRate").as_uint(30);

	for (pugi::xml_node c : node.child("Channels").children()) {
		const bool quat = Same(c.name(), "ChannelQuaternion");
		const bool scalar = Same(c.name(), "ChannelScalar");
		const char *type = c.attribute("Type").value();
		W3xChannel ch;
		if (quat && Same(type, "Orientation"))
			ch.type = W3X_CHANNEL_ORIENTATION;
		else if (scalar && Same(type, "XTranslation"))
			ch.type = W3X_CHANNEL_X_TRANSLATION;
		else if (scalar && Same(type, "YTranslation"))
			ch.type = W3X_CHANNEL_Y_TRANSLATION;
		else if (scalar && Same(type, "ZTranslation"))
			ch.type = W3X_CHANNEL_Z_TRANSLATION;
		else if (scalar && Same(type, "Visibility"))
			ch.type = W3X_CHANNEL_VISIBILITY;
		else {
			warnings.push_back(Format("animation channel %s %s is not converted", c.name(), type));
			continue;
		}

		ch.pivot = c.attribute("Pivot").as_uint();
		ch.firstFrame = c.attribute("FirstFrame").as_uint();
		for (pugi::xml_node f : c.children("Frame")) {
			if (quat) {
				const Quaternion q = Read_Quat(f);
				ch.values.insert(ch.values.end(), { q.X, q.Y, q.Z, q.W });
			} else
				ch.values.push_back(f.text().as_float());
			ch.binaryMove.push_back(f.attribute("BinaryMove").as_bool());
		}
		a.channels.push_back(ch);
	}
}

// "ART:Dir/File.w3x" gives "File.w3x": the name an include is looked up by.
std::string Include_File(std::string source)
{
	return source.substr(source.find_last_of("/\\") + 1);
}
} // namespace

bool W3x_Parse(const char *data, size_t size, W3xFile &out, std::string &error, std::vector<std::string> &warnings)
{
	out = W3xFile();
	pugi::xml_document doc;
	const pugi::xml_parse_result parsed = doc.load_buffer(data, size);
	if (!parsed) {
		error = Format("XML error at byte %d: %s", (int)parsed.offset, parsed.description());
		return false;
	}
	const pugi::xml_node root = doc.document_element();
	if (!Same(root.name(), "AssetDeclaration")) {
		error = Format("<%s> is not a W3X AssetDeclaration", root.name());
		return false;
	}

	for (pugi::xml_node inc : root.child("Includes").children("Include")) {
		std::string source = inc.attribute("source").value();
		if (_strnicmp(source.c_str(), "ART:", 4) == 0)
			source = source.substr(4);
		// Only .w3x includes hold hierarchies; the .xml ones declare textures.
		const std::string file = Include_File(source);
		if (file.size() >= 4 && _stricmp(file.c_str() + file.size() - 4, ".w3x") == 0)
			out.w3xIncludes.push_back(source);
		else
			out.xmlIncludes.push_back(source);
	}

	for (pugi::xml_node n : root.children()) {
		if (Same(n.name(), "W3DHierarchy")) {
			out.hierarchies.emplace_back();
			Parse_Hierarchy(n, out.hierarchies.back());
		} else if (Same(n.name(), "W3DMesh")) {
			out.meshes.emplace_back();
			if (!Parse_Mesh(n, out.meshes.back(), error))
				return false;
		} else if (Same(n.name(), "W3DCollisionBox")) {
			W3xBox box;
			box.id = n.attribute("id").value();
			box.center = Read_Vec(n.child("Center"));
			box.extent = Read_Vec(n.child("Extent"));
			out.boxes.push_back(box);
		} else if (Same(n.name(), "W3DContainer")) {
			W3xContainer c;
			c.id = n.attribute("id").value();
			c.hierarchy = n.attribute("Hierarchy").value();
			for (pugi::xml_node s : n.children("SubObject")) {
				const pugi::xml_node ref = s.child("RenderObject").first_child();
				W3xSubObject sub;
				sub.subObjectId = s.attribute("SubObjectID").value();
				sub.boneIndex = s.attribute("BoneIndex").as_uint();
				sub.renderObjectId = ref.text().get();
				sub.isBox = Same(ref.name(), "CollisionBox");
				c.subObjects.push_back(sub);
			}
			out.containers.push_back(c);
		} else if (Same(n.name(), "W3DAnimation")) {
			out.animations.emplace_back();
			Parse_Animation(n, out.animations.back(), warnings);
		} else if (!Same(n.name(), "Includes"))
			warnings.push_back(Format("<%s> is not converted", n.name()));
	}
	return true;
}

bool W3x_Parse_Texture_Declarations(const char *data, size_t size, std::map<std::string, std::string> &idToFile)
{
	pugi::xml_document doc;
	if (!doc.load_buffer(data, size))
		return false;
	const pugi::xml_node root = doc.document_element();
	if (!Same(root.name(), "AssetDeclaration"))
		return false;
	for (pugi::xml_node t : root.children("Texture"))
		idToFile[Lower(t.attribute("id").value())] = t.attribute("File").value();
	return true;
}

void W3x_Bind_Pose(const W3xHierarchy &tree, std::vector<Vector3> &translation, std::vector<Quaternion> &rotation)
{
	translation.assign(tree.pivots.size(), Vector3(0, 0, 0));
	rotation.assign(tree.pivots.size(), Quaternion(true));
	for (size_t i = 1; i < tree.pivots.size(); ++i) {
		const int parent = tree.pivots[i].parent;
		if (parent < 0 || (size_t)parent >= i)
			continue;
		translation[i] = translation[parent] + rotation[parent].Rotate_Vector(tree.pivots[i].translation);
		rotation[i] = rotation[parent] * tree.pivots[i].rotation;
	}
}

// The blocks that have everything a vertex needs; a skin needs bones too.
static size_t Usable_Blocks(const W3xMesh &mesh)
{
	const bool skin = mesh.geometryType == W3X_GEOMETRY_SKIN;
	size_t n = 0;
	while (n < mesh.blocks.size() && !mesh.blocks[n].positions.empty() && !mesh.blocks[n].normals.empty()
		&& (!skin || !mesh.blocks[n].bones.empty()))
		++n;
	return skin ? n : std::min<size_t>(n, 1);
}

size_t W3x_Heaviest_Block(const W3xMesh &mesh, size_t vertex, bool *blended)
{
	const size_t usable = Usable_Blocks(mesh);
	size_t k = 0;
	for (size_t b = 1; b < usable; ++b)
		if (mesh.blocks[b].weights[vertex] > mesh.blocks[k].weights[vertex])
			k = b;
	if (blended) {
		*blended = false;
		for (size_t b = 0; b < mesh.blocks.size(); ++b)
			if (b != k && !mesh.blocks[b].bones.empty() && mesh.blocks[b].weights[vertex] > 1e-4f
				&& mesh.blocks[b].bones[vertex] != mesh.blocks[k].bones[vertex]) {
				*blended = true;
				break;
			}
	}
	return k;
}

bool W3x_Single_Bone_Vertices(const W3xMesh &mesh, const W3xHierarchy *tree, W3xMeshVertices &out, std::string &error)
{
	const char *id = mesh.id.c_str();
	const bool skin = mesh.geometryType == W3X_GEOMETRY_SKIN;
	const size_t count = mesh.blocks.empty() ? 0 : mesh.blocks[0].positions.size();
	if (Usable_Blocks(mesh) == 0 || count == 0) {
		error = Format("mesh %s has no vertices or normals", id);
		return false;
	}

	std::vector<Vector3> bind_t;
	std::vector<Quaternion> bind_q;
	if (skin) {
		if (!tree || tree->pivots.empty()) {
			error = Format("skin mesh %s has no hierarchy", id);
			return false;
		}
		W3x_Bind_Pose(*tree, bind_t, bind_q);
	}

	out = W3xMeshVertices();
	out.positions.resize(count);
	out.normals.resize(count);
	if (skin)
		out.bones.resize(count);
	std::vector<Vector3> model_pos(count);
	for (size_t i = 0; i < count; ++i) {
		bool blended = false;
		const size_t k = skin ? W3x_Heaviest_Block(mesh, i, &blended) : 0;
		out.blended += blended;
		out.positions[i] = mesh.blocks[k].positions[i];
		out.normals[i] = mesh.blocks[k].normals[i];
		model_pos[i] = out.positions[i];
		if (skin) {
			const unsigned bone = mesh.blocks[k].bones[i];
			if (bone >= tree->pivots.size()) {
				error = Format("skin mesh %s: vertex %d follows bone %u, the hierarchy has %d", id, (int)i, bone, (int)tree->pivots.size());
				return false;
			}
			out.bones[i] = (uint16)bone;
			model_pos[i] = bind_t[bone] + bind_q[bone].Rotate_Vector(out.positions[i]);
		}
	}

	out.min = out.max = model_pos[0];
	for (const Vector3 &p : model_pos) {
		out.min.Update_Min(p);
		out.max.Update_Max(p);
	}
	out.center = (out.min + out.max) * 0.5f;
	float radius2 = 0.0f;
	for (const Vector3 &p : model_pos)
		radius2 = std::max(radius2, (p - out.center).Length2());
	out.radius = sqrtf(radius2);
	return true;
}

void W3x_Map_Shader(const W3xMesh &mesh, bool hasUvs, W3xShading &out, std::vector<std::string> &warnings)
{
	const char *id = mesh.id.c_str();
	std::string diffuse, normal_map, spec_map;
	bool alpha_test = false, two_sided = false, depth_write = true;
	int blend = 0;
	for (const W3xConstant &c : mesh.material.constants) {
		const char *cname = c.name.c_str();
		if (c.kind == W3xConstant::Texture) {
			if (Same(cname, "DiffuseTexture") || Same(cname, "Texture_0"))
				diffuse = c.text;
			else if (Same(cname, "NormalMap"))
				normal_map = c.text;
			else if (Same(cname, "SpecMap"))
				spec_map = c.text;
			else if (!c.text.empty() && !Same(c.text.c_str(), diffuse.c_str()))
				warnings.push_back(Format("mesh %s: texture %s (%s) is not converted", id, cname, c.text.c_str()));
		} else if (Same(cname, "AlphaTestEnable"))
			alpha_test = c.asBool;
		else if (Same(cname, "CullingEnable"))
			two_sided = !c.asBool;
		else if (Same(cname, "DepthWriteEnable"))
			depth_write = c.asBool;
		else if (Same(cname, "BlendMode"))
			blend = c.asInt;
		else if (Same(cname, "AlphaBlendingEnable") && c.asBool)
			blend = 1;
	}
	if (!normal_map.empty() && !Same(normal_map.c_str(), (diffuse + "_NRM").c_str()))
		warnings.push_back(Format("mesh %s: NormalMap %s is not named %s_NRM, so the engine will not find it", id, normal_map.c_str(), diffuse.c_str()));
	if (!spec_map.empty() && !Same(spec_map.c_str(), (diffuse + "_SPM").c_str()))
		warnings.push_back(Format("mesh %s: SpecMap %s is not named %s_SPM, so the engine will not find it", id, spec_map.c_str(), diffuse.c_str()));
	if (!diffuse.empty() && !hasUvs) {
		warnings.push_back(Format("mesh %s: texture %s is dropped because the mesh has no TexCoords", id, diffuse.c_str()));
		diffuse.clear();
	}

	W3d_Shader_Reset(&out.shader);
	out.shader.Texturing = diffuse.empty() ? W3DSHADER_TEXTURING_DISABLE : W3DSHADER_TEXTURING_ENABLE;
	out.shader.AlphaTest = alpha_test ? W3DSHADER_ALPHATEST_ENABLE : W3DSHADER_ALPHATEST_DISABLE;
	out.shader.DepthMask = depth_write ? W3DSHADER_DEPTHMASK_WRITE_ENABLE : W3DSHADER_DEPTHMASK_WRITE_DISABLE;
	// DefaultW3D.fx BlendMode: 0 opaque, 1 alpha blend, 2 additive.
	if (blend == 1) {
		out.shader.SrcBlend = W3DSHADER_SRCBLENDFUNC_SRC_ALPHA;
		out.shader.DestBlend = W3DSHADER_DESTBLENDFUNC_ONE_MINUS_SRC_ALPHA;
	} else if (blend == 2) {
		out.shader.SrcBlend = W3DSHADER_SRCBLENDFUNC_ONE;
		out.shader.DestBlend = W3DSHADER_DESTBLENDFUNC_ONE;
	} else if (blend != 0)
		warnings.push_back(Format("mesh %s: unknown BlendMode %d is drawn opaque", id, blend));

	W3dVertexMaterialStruct &material = out.material;
	memset(&material, 0, sizeof(material));
	W3d_Vertex_Material_Reset(&material);
	// Only the DefaultW3D.fx colours are fixed-function material colours; other shaders keep the reset values.
	if (Same(mesh.material.shaderName.c_str(), "DefaultW3D.fx")) {
		for (const W3xConstant &c : mesh.material.constants) {
			if (c.kind != W3xConstant::Float)
				continue;
			const char *cname = c.name.c_str();
			float v[3] = { 0, 0, 0 };
			const int n = (int)std::min<size_t>(c.values.size(), 3);
			for (int i = 0; i < n; ++i)
				v[i] = c.values[i];
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

	out.textureId = diffuse;
	out.twoSided = two_sided;
}
