#include "Mesh.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Borpa { namespace Sim {

namespace {

constexpr int k_tet_edges[6][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 }, { 0, 3 }, { 1, 3 }, { 2, 3 } };
// Faces of a positively oriented tetrahedron, wound so their normals point out of it.
constexpr int k_tet_faces[4][3] = { { 0, 2, 1 }, { 0, 1, 3 }, { 1, 2, 3 }, { 2, 0, 3 } };

Vec3 sub(const Vec3& a, const Vec3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

uint64_t edge_key(uint32_t a, uint32_t b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(a) << 32) | b;
}

struct FaceKey
{
    uint32_t v[3];
    bool     operator==(const FaceKey& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2]; }
};
struct FaceKeyHash
{
    size_t operator()(const FaceKey& k) const
    {
        uint64_t h = k.v[0];
        h          = h * 0x9E3779B97F4A7C15ull ^ k.v[1];
        h          = h * 0x9E3779B97F4A7C15ull ^ k.v[2];
        return size_t(h ^ (h >> 29));
    }
};
FaceKey face_key(uint32_t a, uint32_t b, uint32_t c)
{
    FaceKey k{ { a, b, c } };
    std::sort(k.v, k.v + 3);
    return k;
}

} // namespace

double triangle_area(const Vec3& a, const Vec3& b, const Vec3& c)
{
    Vec3 n = cross(sub(b, a), sub(c, a));
    return 0.5 * std::sqrt(dot(n, n));
}

bool promote_to_quadratic(const TetMesh& in, QuadraticMesh& out, std::string& error)
{
    out = QuadraticMesh{};
    if (in.tets.empty()) {
        error = "The mesh has no tetrahedra.";
        return false;
    }
    if (in.boundary.size() != in.boundary_face.size()) {
        error = "The mesh's boundary triangles and their faces do not match up.";
        return false;
    }
    out.nodes        = in.nodes;
    out.corner_count = uint32_t(in.nodes.size());

    // Positive orientation, then the outward faces of every tetrahedron.
    std::vector<std::array<uint32_t, 4>> tets = in.tets;
    for (auto& t : tets) {
        for (uint32_t v : t)
            if (v >= in.nodes.size()) {
                error = "A tetrahedron refers to a node the mesh does not have.";
                return false;
            }
        const Vec3& p0 = in.nodes[t[0]];
        double      vol = dot(cross(sub(in.nodes[t[1]], p0), sub(in.nodes[t[2]], p0)), sub(in.nodes[t[3]], p0));
        if (vol < 0)
            std::swap(t[1], t[2]);
    }

    struct FaceUse
    {
        uint32_t tet;
        int      local;
        int      count;
    };
    std::unordered_map<FaceKey, FaceUse, FaceKeyHash> faces;
    faces.reserve(tets.size() * 3);
    for (uint32_t ti = 0; ti < tets.size(); ++ti)
        for (int f = 0; f < 4; ++f) {
            const auto& t   = tets[ti];
            FaceKey     key = face_key(t[k_tet_faces[f][0]], t[k_tet_faces[f][1]], t[k_tet_faces[f][2]]);
            auto [it, fresh] = faces.try_emplace(key, FaceUse{ ti, f, 0 });
            ++it->second.count;
            if (it->second.count > 2) {
                error = "Three or more tetrahedra share one face: the mesh is not a valid solid.";
                return false;
            }
        }

    // Mid-edge nodes.
    std::unordered_map<uint64_t, uint32_t> mid;
    mid.reserve(tets.size() * 2);
    auto mid_node = [&](uint32_t a, uint32_t b) {
        auto [it, fresh] = mid.try_emplace(edge_key(a, b), 0u);
        if (fresh) {
            const Vec3& pa = out.nodes[a];
            const Vec3& pb = out.nodes[b];
            it->second     = uint32_t(out.nodes.size());
            out.nodes.push_back({ (pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2, (pa[2] + pb[2]) / 2 });
        }
        return it->second;
    };
    out.tets.reserve(tets.size());
    for (const auto& t : tets) {
        std::array<uint32_t, 10> q{};
        for (int k = 0; k < 4; ++k)
            q[k] = t[k];
        for (int e = 0; e < 6; ++e)
            q[4 + e] = mid_node(t[k_tet_edges[e][0]], t[k_tet_edges[e][1]]);
        out.tets.push_back(q);
    }

    // Boundary: every mesher triangle must be a face used once; every face used once must be
    // covered by a mesher triangle, or some of the surface has no Hew face.
    size_t boundary_faces = 0;
    for (const auto& kv : faces)
        boundary_faces += kv.second.count == 1;
    if (boundary_faces != in.boundary.size()) {
        error = "The mesh's outer surface and the Object's faces do not match up.";
        return false;
    }
    out.boundary.reserve(in.boundary.size());
    out.boundary_face = in.boundary_face;
    for (const auto& tri : in.boundary) {
        auto it = faces.find(face_key(tri[0], tri[1], tri[2]));
        if (it == faces.end() || it->second.count != 1) {
            error = "A face of the Object lies inside the mesh: the Object is not one closed solid.";
            return false;
        }
        const auto& t = tets[it->second.tet];
        const int*  f = k_tet_faces[it->second.local];
        uint32_t    a = t[f[0]], b = t[f[1]], c = t[f[2]];
        out.boundary.push_back({ a, b, c, mid_node(a, b), mid_node(b, c), mid_node(c, a) });
    }
    return true;
}

}} // namespace Borpa::Sim
