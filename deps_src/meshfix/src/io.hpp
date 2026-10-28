// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "core.hpp"

namespace meshfix {

enum class IoStatus { Ok, Io, Format };

// Reads .stl / .obj by extension. The vertices are not merged: an STL comes as a soup.
IoStatus read_mesh_file(const std::string &path, Mesh &m);
IoStatus write_mesh_file(const Mesh &m, const std::string &path);

} // namespace meshfix
