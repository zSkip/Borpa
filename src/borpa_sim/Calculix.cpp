#include "Calculix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

#include "Paths.hpp"

namespace Borpa { namespace Sim {

namespace {

void write_node_list(std::ofstream& f, const std::vector<uint32_t>& nodes)
{
    for (size_t i = 0; i < nodes.size(); ++i) {
        f << nodes[i] + 1;
        f << (i + 1 == nodes.size() || (i + 1) % 12 == 0 ? "\n" : ",");
    }
}

} // namespace

std::vector<std::string> calculix_arguments() { return { "part" }; }

std::vector<uint32_t> fixture_nodes(const QuadraticMesh& mesh, const StaticStudy& study)
{
    std::set<uint32_t> faces;
    for (const Fixture& fx : study.fixtures)
        faces.insert(fx.faces.begin(), fx.faces.end());
    std::set<uint32_t> nodes;
    for (size_t t = 0; t < mesh.boundary.size(); ++t)
        if (faces.count(mesh.boundary_face[t]))
            nodes.insert(mesh.boundary[t].begin(), mesh.boundary[t].end());
    return std::vector<uint32_t>(nodes.begin(), nodes.end());
}

bool write_calculix_static(const std::string& dir, const QuadraticMesh& mesh, const StaticStudy& study,
                           std::string& error)
{
    std::ofstream f(path_from_utf8(dir) / "part.inp", std::ios::binary);
    if (!f) {
        error = "Could not write the solver's input file.";
        return false;
    }
    char buf[192];
    f << "** Written by Borpa: linear static stress\n*NODE, NSET=NALL\n";
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Vec3& p = mesh.nodes[i];
        std::snprintf(buf, sizeof(buf), "%zu,%.12g,%.12g,%.12g\n", i + 1, p[0], p[1], p[2]);
        f << buf;
    }
    f << "*ELEMENT, TYPE=C3D10, ELSET=EALL\n";
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
        f << e + 1;
        for (uint32_t n : mesh.tets[e])
            f << "," << n + 1;
        f << "\n";
    }

    // Boundary triangles of each Hew face.
    std::map<uint32_t, std::vector<size_t>> tris_of_face;
    for (size_t t = 0; t < mesh.boundary.size(); ++t)
        tris_of_face[mesh.boundary_face[t]].push_back(t);

    const std::vector<uint32_t> fixed = fixture_nodes(mesh, study);
    if (fixed.empty()) {
        error = "The fixed faces have no mesh on them.";
        return false;
    }
    f << "*NSET, NSET=FIXED\n";
    write_node_list(f, fixed);

    std::map<uint32_t, Vec3> loads;
    for (const ForceLoad& load : study.loads) {
        double area = 0;
        for (uint32_t face : load.faces)
            for (size_t t : tris_of_face[face]) {
                const auto& b = mesh.boundary[t];
                area += triangle_area(mesh.nodes[b[0]], mesh.nodes[b[1]], mesh.nodes[b[2]]);
            }
        if (!(area > 0)) {
            error = "A force is on faces with no area.";
            return false;
        }
        for (uint32_t face : load.faces)
            for (size_t t : tris_of_face[face]) {
                const auto& b     = mesh.boundary[t];
                double      share = triangle_area(mesh.nodes[b[0]], mesh.nodes[b[1]], mesh.nodes[b[2]]) / area / 3.0;
                for (int m = 3; m < 6; ++m) {
                    Vec3& v = loads[b[m]];
                    for (int k = 0; k < 3; ++k)
                        v[k] += share * load.vector[k];
                }
            }
    }

    const Material& m = study.material;
    std::snprintf(buf, sizeof(buf), "*MATERIAL, NAME=MAT\n*ELASTIC\n%.9g,%.9g\n", m.youngs_modulus, m.poisson_ratio);
    f << buf;
    f << "*SOLID SECTION, ELSET=EALL, MATERIAL=MAT\n"
         "*STEP\n*STATIC\n*BOUNDARY\nFIXED,1,3,0.\n*CLOAD\n";
    for (const auto& [node, v] : loads)
        for (int k = 0; k < 3; ++k)
            if (v[k] != 0) {
                std::snprintf(buf, sizeof(buf), "%u,%d,%.12g\n", node + 1, k + 1, v[k]);
                f << buf;
            }
    f << "*NODE FILE\nU,RF\n*EL FILE\nS\n*END STEP\n";
    if (!f) {
        error = "Could not write the solver's input file.";
        return false;
    }
    return true;
}

double von_mises(const std::array<double, 6>& s)
{
    double a = s[0] - s[1], b = s[1] - s[2], c = s[2] - s[0];
    return std::sqrt(0.5 * (a * a + b * b + c * c) + 3.0 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
}

bool read_calculix_static(const std::string& dir, size_t node_count, StaticResult& out, std::string& error)
{
    out = StaticResult{};
    std::ifstream in(path_from_utf8(dir) / "part.frd", std::ios::binary);
    if (!in) {
        error = "The solver wrote no result file.";
        return false;
    }
    out.displacement.assign(node_count, { NAN, NAN, NAN });
    out.stress.assign(node_count, { NAN, NAN, NAN, NAN, NAN, NAN });
    out.force.assign(node_count, { 0, 0, 0 });

    // The .frd file is fixed-width text: a block starts with " -4  NAME", each value line is
    // " -1", the node number in 10 columns, then values in 12 columns each; " -3" ends a block.
    // Later steps overwrite earlier ones.
    enum class Block { None, Disp, Stress, Force } block = Block::None;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 3, " -4") == 0) {
            std::string name = line.size() > 5 ? line.substr(5, 8) : std::string();
            name.erase(name.find_last_not_of(' ') + 1);
            block = name == "DISP" ? Block::Disp : name == "STRESS" ? Block::Stress : name == "FORC" ? Block::Force : Block::None;
        } else if (line.compare(0, 3, " -3") == 0) {
            block = Block::None;
        } else if (block != Block::None && line.compare(0, 3, " -1") == 0 && line.size() >= 13) {
            long node = std::strtol(line.substr(3, 10).c_str(), nullptr, 10);
            if (node < 1 || size_t(node) > node_count)
                continue;
            double v[6] = { 0, 0, 0, 0, 0, 0 };
            int    n    = 0;
            for (size_t pos = 13; pos + 1 < line.size() + 1 && n < 6; pos += 12) {
                std::string chunk = line.substr(pos, 12);
                if (chunk.find_first_not_of(' ') == std::string::npos)
                    break;
                v[n++] = std::strtod(chunk.c_str(), nullptr);
            }
            size_t i = size_t(node - 1);
            if (block == Block::Disp && n >= 3)
                out.displacement[i] = { v[0], v[1], v[2] };
            else if (block == Block::Stress && n >= 6)
                out.stress[i] = { v[0], v[1], v[2], v[3], v[4], v[5] };
            else if (block == Block::Force && n >= 3)
                out.force[i] = { v[0], v[1], v[2] };
        }
    }
    for (size_t i = 0; i < node_count; ++i) {
        for (double d : out.displacement[i])
            if (!std::isfinite(d)) {
                error = "The solver's result file has no displacement for some of the mesh.";
                return false;
            }
        for (double s : out.stress[i])
            if (!std::isfinite(s)) {
                error = "The solver's result file has no stress for some of the mesh.";
                return false;
            }
    }
    return true;
}

}} // namespace Borpa::Sim
