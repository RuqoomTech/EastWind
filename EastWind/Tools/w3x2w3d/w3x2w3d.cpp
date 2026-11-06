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

// w3x2w3d <in.w3x> [-o <out dir>] [--name <short name>]
// Writes <out dir>/<model>.w3d, and replaces an existing one only when --name is given. Includes are
// read from the folder of the input file.

#include "WW3D2/w3xconvert.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static bool Read_File(const fs::path &path, std::string &text)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

int main(int argc, char **argv)
{
	fs::path input, out_dir = ".";
	std::string name;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-o" && i + 1 < argc)
			out_dir = argv[++i];
		else if (arg == "--name" && i + 1 < argc)
			name = argv[++i];
		else if (input.empty() && arg[0] != '-')
			input = arg;
		else {
			fprintf(stderr, "usage: w3x2w3d <in.w3x> [-o <out dir>] [--name <short name>]\n");
			return 2;
		}
	}
	if (input.empty()) {
		fprintf(stderr, "usage: w3x2w3d <in.w3x> [-o <out dir>] [--name <short name>]\n");
		return 2;
	}

	std::string xml;
	if (!Read_File(input, xml)) {
		fprintf(stderr, "error: cannot read %s\n", input.string().c_str());
		return 1;
	}

	// The file system ignores case, as the game's ART: paths do.
	const fs::path folder = input.parent_path();
	const W3xIncludeReader read_include = [&](const std::string &file, std::string &text) {
		return Read_File(folder / file, text);
	};

	W3xConvertResult result;
	const bool ok = Convert_W3X(xml, name, read_include, result);
	for (const std::string &w : result.Warnings)
		printf("warning: %s\n", w.c_str());
	if (!ok) {
		fprintf(stderr, "error: %s: %s\n", input.string().c_str(), result.Error.c_str());
		return 1;
	}

	std::error_code ec;
	fs::create_directories(out_dir, ec);
	const fs::path out = out_dir / (result.Name + ".w3d");
	// RA3 LOD files share their model's id, so a second file of a folder would replace the first.
	if (name.empty() && fs::exists(out)) {
		fprintf(stderr, "error: %s already exists; give --name to write %s under another name or to replace it\n",
			out.string().c_str(), input.string().c_str());
		return 1;
	}
	std::ofstream file(out, std::ios::binary);
	file.write(reinterpret_cast<const char *>(result.W3d.data()), (std::streamsize)result.W3d.size());
	file.close();
	if (!file) {
		fprintf(stderr, "error: cannot write %s\n", out.string().c_str());
		return 1;
	}
	printf("wrote %s (%d bytes)\n", out.string().c_str(), (int)result.W3d.size());
	return 0;
}
