#pragma once

#include <array>
#include <string>
#include <vector>

#include "Mesh.hpp"
#include "Request.hpp"

namespace Borpa { namespace Sim {

// Writes `part.inp` into `dir`: C3D10 elements, every node of a fixture's faces held in x, y and
// z, each force spread over its faces by area as consistent nodal loads (a quadratic triangle
// under uniform traction puts a third of its share on each mid-edge node and none on its
// corners), and requests displacement, reaction force and stress output.
bool write_calculix_static(const std::string& dir, const QuadraticMesh& mesh, const StaticStudy& study,
                           std::string& error);

// Every node on a fixture's faces (corners and mid-edge nodes), sorted.
std::vector<uint32_t> fixture_nodes(const QuadraticMesh& mesh, const StaticStudy& study);

// Command-line arguments for ccx: the job name without its extension, run in `dir`.
std::vector<std::string> calculix_arguments();

struct StaticResult
{
    std::vector<Vec3>                  displacement; // per mesh node, mm
    std::vector<std::array<double, 6>> stress;       // per mesh node: xx, yy, zz, xy, yz, zx (MPa)
    // Per mesh node (N). CalculiX reports the external force at every node: the reaction where
    // a node is held, the applied load where one is applied, zero elsewhere.
    std::vector<Vec3>                  force;
};

// Reads `part.frd`. Fails unless every node has a finite displacement and stress.
bool read_calculix_static(const std::string& dir, size_t node_count, StaticResult& out, std::string& error);

double von_mises(const std::array<double, 6>& s);

}} // namespace Borpa::Sim
