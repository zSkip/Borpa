#pragma once

#include <string>
#include <vector>

#include "Mesh.hpp"
#include "Request.hpp"

namespace Borpa { namespace Sim {

// Default element size for a surface: a fortieth of the bounding-box diagonal, but no more than
// half the box's smallest side, so thin parts get two elements through their thickness; and never
// so small that the mesh grows past about 40,000 tetrahedra. Finer meshes come from refinement on
// picked faces.
double automatic_mesh_size(const SurfaceGeometry& geometry);

// Writes `part.stl` (ASCII, one solid per Hew face that has triangles) and `part.geo` into
// `dir`. The .geo builds one Gmsh surface per solid with CreateTopology, so surfaces keep the
// Hew faces' boundaries; each surface's physical tag is its Hew face index + 1, which is how the
// mesh's boundary triangles map back to Hew faces. Returns false if a file cannot be written.
bool write_gmsh_input(const std::string& dir, const SurfaceGeometry& geometry, const StaticStudy& study,
                      double element_size, std::string& error);

// Command-line arguments for gmsh.exe, given the files write_gmsh_input wrote.
std::vector<std::string> gmsh_arguments(const std::string& dir);

// Reads a Gmsh 2.2 ASCII mesh: linear tetrahedra (type 4) and boundary triangles (type 2),
// whose physical tag minus one is the Hew face index. Nodes no tetrahedron uses are dropped.
bool read_gmsh_msh22(const std::string& path, size_t face_count, TetMesh& out, std::string& error);

}} // namespace Borpa::Sim
