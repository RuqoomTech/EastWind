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

// EastWind W3X parser: reads one RA3 W3X file (XML) into plain structs with full-length names and
// every value the engine can use. Two back ends read the result: the W3D converter (w3xconvert.cpp,
// which clips names to W3D's 15 characters) and the native W3X loader. Nothing here touches engine
// runtime classes, and pugixml stays in the .cpp.

#pragma once

#include "WW3D2/w3d_file.h"
#include "WWMath/quat.h"
#include "WWMath/vector2.h"
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"

#include <map>
#include <string>
#include <vector>

struct W3xPivot
{
	std::string name;
	int parent = -1;
	Vector3 translation = Vector3(0, 0, 0);
	Quaternion rotation = Quaternion(0, 0, 0, 1);
};

struct W3xHierarchy
{
	std::string id;
	std::vector<W3xPivot> pivots;
};

enum W3xGeometryType
{
	W3X_GEOMETRY_NORMAL,
	W3X_GEOMETRY_SKIN,
	W3X_GEOMETRY_CAMERA_ALIGNED,
	W3X_GEOMETRY_CAMERA_ORIENTED,
};

// Vertex k of a block is in the space of the bone that the same block's bones[k] names.
// A skin has one or two blocks; a vertex weighted on both has the same weight sum over them.
struct W3xVertexBlock
{
	std::vector<Vector3> positions;
	std::vector<Vector3> normals;
	std::vector<uint16> bones;  // skin only
	std::vector<float> weights; // skin only
};

struct W3xTriangle
{
	uint32 v[3];
};

// One FXShader constant as written, so a back end can apply them in file order.
struct W3xConstant
{
	enum Kind { Texture, Float, Int, Bool, Other };
	Kind kind = Other;
	std::string name;
	std::string text;          // the first Value as written (the texture id for a Texture)
	std::vector<float> values; // every Value as a float (not for a Texture)
	int asInt = 0;             // the first Value as an int
	bool asBool = false;       // the first Value as a bool
};

struct W3xMaterial
{
	std::string shaderName;
	int techniqueIndex = 0;
	std::vector<W3xConstant> constants; // all of them, in file order
	// The same constants by name, the last one winning.
	std::map<std::string, std::string> textures;
	std::map<std::string, std::vector<float>> floats;
	std::map<std::string, int> ints;
	std::map<std::string, bool> bools;
};

struct W3xMesh
{
	std::string id;
	W3xGeometryType geometryType = W3X_GEOMETRY_NORMAL;
	bool castShadow = false;
	bool hidden = false;
	int sortLevel = 0;
	std::vector<W3xVertexBlock> blocks;
	std::vector<std::vector<Vector2>> uvSets;
	std::vector<Vector4> vertexColors; // R, G, B, A in X, Y, Z, W; may be empty
	std::vector<W3xTriangle> triangles;
	W3xMaterial material;
	bool hasBounds = false; // the exporter's BoundingBox, kept to check the vertices against
	Vector3 boundsMin = Vector3(0, 0, 0);
	Vector3 boundsMax = Vector3(0, 0, 0);
};

struct W3xBox
{
	std::string id;
	Vector3 center = Vector3(0, 0, 0);
	Vector3 extent = Vector3(0, 0, 0);
};

struct W3xSubObject
{
	std::string subObjectId;
	uint32 boneIndex = 0;
	std::string renderObjectId; // the full id of the mesh or box
	bool isBox = false;
};

struct W3xContainer
{
	std::string id;
	std::string hierarchy;
	std::vector<W3xSubObject> subObjects;
};

enum W3xChannelType
{
	W3X_CHANNEL_X_TRANSLATION,
	W3X_CHANNEL_Y_TRANSLATION,
	W3X_CHANNEL_Z_TRANSLATION,
	W3X_CHANNEL_ORIENTATION,
	W3X_CHANNEL_VISIBILITY,
};

struct W3xChannel
{
	uint32 pivot = 0;
	W3xChannelType type = W3X_CHANNEL_X_TRANSLATION;
	uint32 firstFrame = 0;
	std::vector<float> values;      // one per frame; four per frame (X, Y, Z, W) for Orientation
	std::vector<bool> binaryMove;   // one per frame
};

struct W3xAnimation
{
	std::string id;
	std::string hierarchy;
	uint32 numFrames = 0;
	uint32 frameRate = 30;
	std::vector<W3xChannel> channels;
};

struct W3xFile
{
	std::vector<std::string> w3xIncludes; // the source as given, without the "ART:" prefix
	std::vector<std::string> xmlIncludes;
	std::vector<W3xHierarchy> hierarchies;
	std::vector<W3xMesh> meshes;
	std::vector<W3xBox> boxes;
	std::vector<W3xContainer> containers;
	std::vector<W3xAnimation> animations;
};

// Parses one W3X document. Warnings get what is skipped. False, with error set, when the file is
// not a W3X document or a mesh in it cannot be used.
bool W3x_Parse(const char *data, size_t size, W3xFile &out, std::string &error, std::vector<std::string> &warnings);

// Parses an .xml texture declaration file: texture id (lower case) to file name.
bool W3x_Parse_Texture_Declarations(const char *data, size_t size, std::map<std::string, std::string> &idToFile);

// The bind pose in model space. Pivot 0 is the model origin whatever its own transform says.
void W3x_Bind_Pose(const W3xHierarchy &tree, std::vector<Vector3> &translation, std::vector<Quaternion> &rotation);

// A skin vertex keeps the block with the heaviest weight (the first on a tie), so the engine's one bone
// per vertex gets the vertex in that bone's space. Blended says another block pulls it to another bone.
size_t W3x_Heaviest_Block(const W3xMesh &mesh, size_t vertex, bool *blended = nullptr);

// One block per vertex chosen by W3x_Heaviest_Block, and the bounds of the result in mesh space (the
// model space of the bind pose for a skin). The hierarchy may be null for a mesh that is not a skin.
struct W3xMeshVertices
{
	std::vector<Vector3> positions;
	std::vector<Vector3> normals;
	std::vector<uint16> bones; // skin only
	Vector3 min = Vector3(0, 0, 0);
	Vector3 max = Vector3(0, 0, 0);
	Vector3 center = Vector3(0, 0, 0);
	float radius = 0.0f;
	int blended = 0; // vertices that were pulled by more than one bone
};
bool W3x_Single_Bone_Vertices(const W3xMesh &mesh, const W3xHierarchy *tree, W3xMeshVertices &out, std::string &error);

// What the fixed-function pipeline takes from an FX shader: the diffuse texture, alpha test, blend,
// culling, depth write and, for DefaultW3D.fx, the material colours. The engine finds the normal and
// spec maps itself, as <diffuse>_nrm and <diffuse>_spm.
struct W3xShading
{
	W3dShaderStruct shader;
	W3dVertexMaterialStruct material;
	std::string textureId; // empty: untextured. The file is <id>.tga unless a declaration says otherwise.
	bool twoSided = false;
};
void W3x_Map_Shader(const W3xMesh &mesh, bool hasUvs, W3xShading &out, std::vector<std::string> &warnings);
