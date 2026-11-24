#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Borpa { namespace Sim {

using Vec3 = std::array<double, 3>;

// A linear tetrahedral mesh as the mesher returns it, plus the mesher's boundary triangles,
// each tagged with the Hew face (index into SurfaceGeometry::faces) it lies on.
struct TetMesh
{
    std::vector<Vec3>                    nodes;
    std::vector<std::array<uint32_t, 4>> tets;
    std::vector<std::array<uint32_t, 3>> boundary;
    std::vector<uint32_t>                boundary_face;
};

// Straight-sided quadratic tetrahedra in CalculiX's C3D10 node order: corners 0-3, then the
// mid-edge nodes of edges (0,1), (1,2), (2,0), (0,3), (1,3), (2,3). Boundary triangles are
// oriented outward and hold corners a, b, c then the mid-edge nodes of a-b, b-c, c-a.
// Nodes [0, corner_count) are the corner nodes of the linear mesh, in the same order.
struct QuadraticMesh
{
    std::vector<Vec3>                     nodes;
    std::vector<std::array<uint32_t, 10>> tets;
    std::vector<std::array<uint32_t, 6>>  boundary;
    std::vector<uint32_t>                 boundary_face;
    uint32_t                              corner_count{ 0 };
};

// Orients every tetrahedron to positive volume, checks that the boundary triangles are exactly
// the faces used by one tetrahedron, orients them outward, and adds the mid-edge nodes.
// Returns false and says why when the boundary and the volume do not match.
bool promote_to_quadratic(const TetMesh& linear, QuadraticMesh& out, std::string& error);

double triangle_area(const Vec3& a, const Vec3& b, const Vec3& c);

}} // namespace Borpa::Sim
