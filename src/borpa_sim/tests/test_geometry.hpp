#pragma once

// Test shapes built the way Hew hands them over: welded vertices, planar faces, each face a
// group of triangles with its own face index.

#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "Request.hpp"

namespace Borpa { namespace Sim { namespace Test {

struct Builder
{
    SurfaceGeometry                               g;
    std::map<std::tuple<float, float, float>, uint32_t> index;

    uint32_t vertex(double x, double y, double z)
    {
        auto key = std::make_tuple(float(x), float(y), float(z));
        auto it  = index.find(key);
        if (it != index.end())
            return it->second;
        uint32_t i = uint32_t(g.vertex_count());
        g.positions.insert(g.positions.end(), { float(x), float(y), float(z) });
        index.emplace(key, i);
        return i;
    }
    uint32_t face()
    {
        g.faces.push_back(std::to_string(1000 + g.faces.size()));
        return uint32_t(g.faces.size() - 1);
    }
    void tri(uint32_t face, uint32_t a, uint32_t b, uint32_t c)
    {
        g.indices.insert(g.indices.end(), { a, b, c });
        g.face_index.push_back(face);
    }
};

// Box [0,L] x [0,W] x [0,H]. Faces in order: x=0, x=L, y=0, y=W, z=0, z=H. Outward winding.
inline SurfaceGeometry box(double L, double W, double H)
{
    Builder b;
    b.g.object_id = "1";
    auto v        = [&](int i, int j, int k) { return b.vertex(i * L, j * W, k * H); };
    auto quad     = [&](uint32_t f, uint32_t p, uint32_t q, uint32_t r, uint32_t s) {
        b.tri(f, p, q, r);
        b.tri(f, p, r, s);
    };
    quad(b.face(), v(0, 0, 0), v(0, 0, 1), v(0, 1, 1), v(0, 1, 0));
    quad(b.face(), v(1, 0, 0), v(1, 1, 0), v(1, 1, 1), v(1, 0, 1));
    quad(b.face(), v(0, 0, 0), v(1, 0, 0), v(1, 0, 1), v(0, 0, 1));
    quad(b.face(), v(0, 1, 0), v(0, 1, 1), v(1, 1, 1), v(1, 1, 0));
    quad(b.face(), v(0, 0, 0), v(0, 1, 0), v(1, 1, 0), v(1, 0, 0));
    quad(b.face(), v(0, 0, 1), v(1, 0, 1), v(1, 1, 1), v(0, 1, 1));
    return b.g;
}

// Plate [0,L] x [0,W] x [0,T] with a through hole of radius R at its centre, the hole wall made
// of `segments` planar faces. Faces in order: bottom, top, x=0, x=L, y=0, y=W, then the hole wall.
inline SurfaceGeometry plate_with_hole(double L, double W, double T, double R, int segments)
{
    Builder b;
    b.g.object_id = "1";
    const double cx = L / 2, cy = W / 2;
    std::vector<std::pair<double, double>> ring, corners{ { 0, 0 }, { L, 0 }, { L, W }, { 0, W } };
    for (int i = 0; i < segments; ++i) {
        double a = 2 * 3.14159265358979323846 * i / segments;
        ring.push_back({ cx + R * std::cos(a), cy + R * std::sin(a) });
    }
    auto nearest_corner = [&](double x, double y) {
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (std::hypot(corners[k].first - x, corners[k].second - y) <
                std::hypot(corners[best].first - x, corners[best].second - y))
                best = k;
        return best;
    };
    auto nearest_ring = [&](double x, double y) {
        int best = 0;
        for (int k = 1; k < segments; ++k)
            if (std::hypot(ring[k].first - x, ring[k].second - y) < std::hypot(ring[best].first - x, ring[best].second - y))
                best = k;
        return best;
    };
    // Each cap: a fan from every ring edge to its nearest corner, plus one triangle per rectangle
    // edge to the ring point nearest its middle. Winding is fixed up from the triangle's normal.
    for (int capi = 0; capi < 2; ++capi) {
        const double z  = capi ? T : 0;
        const bool   up = capi == 1;
        uint32_t     f  = b.face();
        auto add = [&](std::pair<double, double> p, std::pair<double, double> q, std::pair<double, double> r) {
            double cross = (q.first - p.first) * (r.second - p.second) - (q.second - p.second) * (r.first - p.first);
            if ((cross > 0) != up)
                std::swap(q, r);
            b.tri(f, b.vertex(p.first, p.second, z), b.vertex(q.first, q.second, z), b.vertex(r.first, r.second, z));
        };
        for (int i = 0; i < segments; ++i) {
            auto p = ring[i], q = ring[(i + 1) % segments];
            add(q, p, corners[nearest_corner((p.first + q.first) / 2, (p.second + q.second) / 2)]);
        }
        for (int k = 0; k < 4; ++k) {
            auto c0 = corners[k], c1 = corners[(k + 1) % 4];
            add(c0, c1, ring[nearest_ring((c0.first + c1.first) / 2, (c0.second + c1.second) / 2)]);
        }
    }
    auto wall = [&](std::pair<double, double> p, std::pair<double, double> q) {
        uint32_t f  = b.face();
        uint32_t a  = b.vertex(p.first, p.second, 0), bb = b.vertex(q.first, q.second, 0);
        uint32_t c  = b.vertex(q.first, q.second, T), d = b.vertex(p.first, p.second, T);
        b.tri(f, a, bb, c);
        b.tri(f, a, c, d);
    };
    wall({ 0, W }, { 0, 0 });
    wall({ L, 0 }, { L, W });
    wall({ 0, 0 }, { L, 0 });
    wall({ L, W }, { 0, W });
    for (int i = 0; i < segments; ++i)
        wall(ring[(i + 1) % segments], ring[i]);
    return b.g;
}

}}} // namespace Borpa::Sim::Test
