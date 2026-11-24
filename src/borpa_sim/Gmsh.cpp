#include "Gmsh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "Paths.hpp"

namespace Borpa { namespace Sim {

namespace fs = std::filesystem;

double automatic_mesh_size(const SurfaceGeometry& g)
{
    double lo[3] = { HUGE_VAL, HUGE_VAL, HUGE_VAL }, hi[3] = { -HUGE_VAL, -HUGE_VAL, -HUGE_VAL };
    for (size_t i = 0; i < g.vertex_count(); ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], double(g.positions[3 * i + k]));
            hi[k] = std::max(hi[k], double(g.positions[3 * i + k]));
        }
    double d[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
    double diag = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    double thin = std::min({ d[0], d[1], d[2] });

    // Volume by the divergence theorem, for the element budget below.
    double volume = 0;
    for (size_t t = 0; t < g.triangle_count(); ++t) {
        const float* a = &g.positions[3 * size_t(g.indices[3 * t])];
        const float* b = &g.positions[3 * size_t(g.indices[3 * t + 1])];
        const float* c = &g.positions[3 * size_t(g.indices[3 * t + 2])];
        volume += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) -
                   double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
                   double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6.0;
    }
    volume = std::abs(volume);
    // Gmsh makes roughly 12 V / h³ tetrahedra at size h on these parts. Keeping that near 40,000
    // keeps a one-thread CalculiX solve to tens of seconds; a 5.6 times larger plate mesh took
    // over ten minutes.
    double budget = std::cbrt(12.0 * volume / 40000.0);
    double size   = std::max(std::min(diag / 40.0, thin / 2.0), budget);
    return std::clamp(size, 0.05, 50.0);
}

bool write_gmsh_input(const std::string& dir, const SurfaceGeometry& g, const StaticStudy& study, double size,
                      std::string& error)
{
    // Surfaces are numbered in the order of the solids in the STL, so remember which Hew face each
    // surface came from.
    std::vector<std::vector<uint32_t>> by_face(g.faces.size());
    for (uint32_t t = 0; t < g.triangle_count(); ++t)
        by_face[g.face_index[t]].push_back(t);
    std::vector<int> surface_of_face(g.faces.size(), 0);

    {
        std::ofstream stl(path_from_utf8(dir) / "part.stl", std::ios::binary);
        if (!stl) {
            error = "Could not write the mesher's input file.";
            return false;
        }
        char buf[160];
        int  surface = 0;
        for (size_t f = 0; f < by_face.size(); ++f) {
            if (by_face[f].empty())
                continue;
            surface_of_face[f] = ++surface;
            stl << "solid f" << f << "\n";
            for (uint32_t t : by_face[f]) {
                stl << "facet normal 0 0 0\n outer loop\n";
                for (int k = 0; k < 3; ++k) {
                    const float* p = &g.positions[3 * size_t(g.indices[3 * t + k])];
                    // %.9g round-trips a float, so shared vertices stay bit-identical in the file.
                    std::snprintf(buf, sizeof(buf), "  vertex %.9g %.9g %.9g\n", p[0], p[1], p[2]);
                    stl << buf;
                }
                stl << " endloop\nendfacet\n";
            }
            stl << "endsolid f" << f << "\n";
        }
        if (!stl) {
            error = "Could not write the mesher's input file.";
            return false;
        }
    }

    std::ofstream geo(path_from_utf8(dir) / "part.geo", std::ios::binary);
    if (!geo) {
        error = "Could not write the mesher's input file.";
        return false;
    }
    geo.precision(9);
    geo << "Merge \"part.stl\";\n"
           // Without CreateTopology the discrete surfaces have no boundary curves and Gmsh writes an
           // empty mesh with exit code 0.
           "CreateTopology;\n"
           "CreateGeometry;\n"
           "s() = Surface{:};\n"
           "Surface Loop(1) = {s()};\n"
           "Volume(1) = {1};\n";
    for (size_t f = 0; f < surface_of_face.size(); ++f)
        if (surface_of_face[f] > 0)
            geo << "Physical Surface(" << f + 1 << ") = {" << surface_of_face[f] << "};\n";
    geo << "Physical Volume(1) = {1};\n"
           // Curvature sizing is unusable on faceted input (solver-bakeoff.md section 4).
           "Mesh.MeshSizeFromCurvature = 0;\n"
           "Mesh.MeshSizeFromPoints = 0;\n"
           "Mesh.MeshSizeExtendFromBoundary = 1;\n"
           "Mesh.MeshSizeMax = "
        << size << ";\n";

    int field = 0;
    std::vector<int> thresholds;
    for (const Refinement& r : study.refine) {
        std::vector<int> surfaces;
        for (uint32_t f : r.faces)
            if (f < surface_of_face.size() && surface_of_face[f] > 0)
                surfaces.push_back(surface_of_face[f]);
        if (surfaces.empty())
            continue;
        const int dist = ++field;
        geo << "Field[" << dist << "] = Distance;\nField[" << dist << "].SurfacesList = {";
        for (size_t i = 0; i < surfaces.size(); ++i)
            geo << (i ? ", " : "") << surfaces[i];
        geo << "};\nField[" << dist << "].Sampling = 20;\n";
        const int thr = ++field;
        const double fine = std::min(r.size, size);
        geo << "Field[" << thr << "] = Threshold;\n"
            << "Field[" << thr << "].InField = " << dist << ";\n"
            << "Field[" << thr << "].SizeMin = " << fine << ";\n"
            << "Field[" << thr << "].SizeMax = " << size << ";\n"
            << "Field[" << thr << "].DistMin = " << fine << ";\n"
            << "Field[" << thr << "].DistMax = " << std::max(4.0 * size, 10.0 * fine) << ";\n";
        thresholds.push_back(thr);
    }
    if (!thresholds.empty()) {
        const int min_field = ++field;
        geo << "Field[" << min_field << "] = Min;\nField[" << min_field << "].FieldsList = {";
        for (size_t i = 0; i < thresholds.size(); ++i)
            geo << (i ? ", " : "") << thresholds[i];
        geo << "};\nBackground Field = " << min_field << ";\n";
    }
    geo << "Mesh.Algorithm3D = 1;\n"
           // Without Netgen's optimiser the smallest dihedral angle on faceted input falls below 1°.
           "Mesh.OptimizeNetgen = 1;\n"
           "Mesh.ElementOrder = 1;\n"
           "Mesh.MshFileVersion = 2.2;\n"
           "Mesh.Binary = 0;\n";
    if (!geo) {
        error = "Could not write the mesher's input file.";
        return false;
    }
    return true;
}

std::vector<std::string> gmsh_arguments(const std::string& dir)
{
    const std::string geo = path_to_utf8(path_from_utf8(dir) / "part.geo");
    const std::string msh = path_to_utf8(path_from_utf8(dir) / "part.msh");
    return { geo, "-3", "-format", "msh22", "-o", msh, "-nt", "1" };
}

bool read_gmsh_msh22(const std::string& path, size_t face_count, TetMesh& out, std::string& error)
{
    out = TetMesh{};
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) {
        error = "The mesher wrote no mesh file.";
        return false;
    }
    std::unordered_map<long long, Vec3> raw_nodes;
    std::vector<std::array<long long, 4>> raw_tets;
    std::vector<std::array<long long, 3>> raw_tris;
    std::vector<uint32_t>                 tri_face;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "$MeshFormat") {
            std::getline(in, line);
            if (line.rfind("2.2 0", 0) != 0) {
                error = "The mesh file is not Gmsh's 2.2 text format.";
                return false;
            }
        } else if (line == "$Nodes") {
            size_t n = 0;
            in >> n;
            raw_nodes.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                long long id;
                Vec3      p;
                if (!(in >> id >> p[0] >> p[1] >> p[2])) {
                    error = "The mesh file's node list is cut short.";
                    return false;
                }
                raw_nodes[id] = p;
            }
        } else if (line == "$Elements") {
            size_t n = 0;
            in >> n;
            std::getline(in, line);
            for (size_t i = 0; i < n; ++i) {
                if (!std::getline(in, line)) {
                    error = "The mesh file's element list is cut short.";
                    return false;
                }
                std::istringstream ls(line);
                long long          id, type, ntags;
                ls >> id >> type >> ntags;
                std::vector<long long> tags(size_t(std::max(0LL, ntags)));
                for (auto& t : tags)
                    ls >> t;
                if (type == 4) {
                    std::array<long long, 4> t{};
                    ls >> t[0] >> t[1] >> t[2] >> t[3];
                    raw_tets.push_back(t);
                } else if (type == 2) {
                    std::array<long long, 3> t{};
                    ls >> t[0] >> t[1] >> t[2];
                    long long phys = tags.empty() ? 0 : tags[0];
                    if (phys < 1 || size_t(phys) > face_count) {
                        error = "The mesh has a surface triangle that belongs to no face of the Object.";
                        return false;
                    }
                    raw_tris.push_back(t);
                    tri_face.push_back(uint32_t(phys - 1));
                }
                if (!ls) {
                    error = "The mesh file has an element it cannot read.";
                    return false;
                }
            }
        }
    }
    if (raw_tets.empty()) {
        error = "The mesher made no volume mesh. The Object may not be a closed solid.";
        return false;
    }

    std::unordered_map<long long, uint32_t> index;
    index.reserve(raw_nodes.size());
    auto node = [&](long long id, uint32_t& out_index) {
        auto it = index.find(id);
        if (it != index.end()) {
            out_index = it->second;
            return true;
        }
        auto p = raw_nodes.find(id);
        if (p == raw_nodes.end())
            return false;
        out_index = uint32_t(out.nodes.size());
        out.nodes.push_back(p->second);
        index.emplace(id, out_index);
        return true;
    };
    out.tets.reserve(raw_tets.size());
    for (const auto& t : raw_tets) {
        std::array<uint32_t, 4> q{};
        for (int k = 0; k < 4; ++k)
            if (!node(t[k], q[k])) {
                error = "A tetrahedron in the mesh file refers to a node it does not list.";
                return false;
            }
        out.tets.push_back(q);
    }
    const size_t volume_nodes = out.nodes.size();
    for (size_t i = 0; i < raw_tris.size(); ++i) {
        std::array<uint32_t, 3> q{};
        for (int k = 0; k < 3; ++k)
            if (!node(raw_tris[i][k], q[k]) || q[k] >= volume_nodes) {
                error = "A surface triangle in the mesh is not on the volume mesh.";
                return false;
            }
        out.boundary.push_back(q);
        out.boundary_face.push_back(tri_face[i]);
    }
    return true;
}

}} // namespace Borpa::Sim
