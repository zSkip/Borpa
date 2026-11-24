#include "ResultFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Paths.hpp"

namespace Borpa { namespace Sim {

using nlohmann::json;

SurfaceResult make_static_surface_result(const QuadraticMesh& mesh, const StaticResult& r)
{
    SurfaceResult out;
    std::unordered_map<uint32_t, uint32_t> local;
    std::vector<uint32_t>                  nodes;
    auto index_of = [&](uint32_t n) {
        auto [it, fresh] = local.try_emplace(n, uint32_t(nodes.size()));
        if (fresh)
            nodes.push_back(n);
        return it->second;
    };
    out.triangles.reserve(mesh.boundary.size() * 3);
    for (size_t t = 0; t < mesh.boundary.size(); ++t) {
        for (int k = 0; k < 3; ++k)
            out.triangles.push_back(index_of(mesh.boundary[t][k]));
        out.face_index.push_back(mesh.boundary_face[t]);
    }

    ResultField disp{ "displacement", 3, "mm", HUGE_VAL, 0, {} };
    ResultField vm{ "vonMises", 1, "MPa", HUGE_VAL, 0, {} };
    out.positions.reserve(nodes.size() * 3);
    disp.values.reserve(nodes.size() * 3);
    vm.values.reserve(nodes.size());
    for (uint32_t n : nodes) {
        for (int k = 0; k < 3; ++k) {
            out.positions.push_back(float(mesh.nodes[n][k]));
            disp.values.push_back(float(r.displacement[n][k]));
        }
        const Vec3& d   = r.displacement[n];
        double      mag = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        disp.min        = std::min(disp.min, mag);
        disp.max        = std::max(disp.max, mag);
        double s        = von_mises(r.stress[n]);
        vm.values.push_back(float(s));
        vm.min = std::min(vm.min, s);
        vm.max = std::max(vm.max, s);
    }
    if (nodes.empty())
        disp.min = vm.min = 0;
    out.fields.push_back(std::move(disp));
    out.fields.push_back(std::move(vm));
    return out;
}

namespace {

template<class T> void append(std::string& buf, const std::vector<T>& v)
{
    static_assert(sizeof(T) == 4, "4-byte elements only");
    if (!v.empty())
        buf.append(reinterpret_cast<const char*>(v.data()), v.size() * 4);
}

template<class T> bool take(const std::string& buf, size_t& pos, size_t count, std::vector<T>& v)
{
    if (count > (buf.size() - pos) / 4)
        return false;
    v.resize(count);
    if (count)
        std::memcpy(v.data(), buf.data() + pos, count * 4);
    pos += count * 4;
    return true;
}

} // namespace

bool write_result_file(const std::string& path, const SurfaceResult& r, std::string& error)
{
    const size_t n = r.positions.size() / 3;
    json         header;
    header["nodes"]     = n;
    header["triangles"] = r.triangles.size() / 3;
    header["fields"]    = json::array();
    for (const ResultField& f : r.fields) {
        if (f.values.size() != n * size_t(f.components)) {
            error = "A result field does not match the result surface.";
            return false;
        }
        header["fields"].push_back(
            { { "name", f.name }, { "components", f.components }, { "unit", f.unit }, { "min", f.min }, { "max", f.max } });
    }
    try {
        header["summary"] = json::parse(r.summary_json);
    } catch (const std::exception&) {
        error = "The result summary is not valid JSON.";
        return false;
    }
    std::string text = header.dump();
    while ((8 + text.size()) % 4 != 0)
        text += ' ';

    std::string buf = "BSR1";
    uint32_t    len = uint32_t(text.size());
    buf.append(reinterpret_cast<const char*>(&len), 4);
    buf += text;
    append(buf, r.positions);
    append(buf, r.triangles);
    append(buf, r.face_index);
    for (const ResultField& f : r.fields)
        append(buf, f.values);

    std::ofstream out(path_from_utf8(path), std::ios::binary);
    out.write(buf.data(), std::streamsize(buf.size()));
    if (!out) {
        error = "Could not write the result file.";
        return false;
    }
    return true;
}

bool read_result_file(const std::string& path, SurfaceResult& out, std::string& error)
{
    out = SurfaceResult{};
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    std::string   buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (buf.size() < 8 || buf.compare(0, 4, "BSR1") != 0) {
        error = "Not a Borpa result file.";
        return false;
    }
    uint32_t len;
    std::memcpy(&len, buf.data() + 4, 4);
    if (len > buf.size() - 8) {
        error = "The result file is cut short.";
        return false;
    }
    try {
        json   header = json::parse(buf.substr(8, len));
        size_t n      = header.at("nodes").get<size_t>();
        size_t t      = header.at("triangles").get<size_t>();
        size_t pos    = 8 + len;
        out.summary_json = header.at("summary").dump();
        if (!take(buf, pos, n * 3, out.positions) || !take(buf, pos, t * 3, out.triangles) ||
            !take(buf, pos, t, out.face_index)) {
            error = "The result file is cut short.";
            return false;
        }
        for (const json& f : header.at("fields")) {
            ResultField field;
            field.name       = f.at("name").get<std::string>();
            field.components = f.at("components").get<int>();
            field.unit       = f.at("unit").get<std::string>();
            field.min        = f.at("min").get<double>();
            field.max        = f.at("max").get<double>();
            if (field.components < 1 || !take(buf, pos, n * size_t(field.components), field.values)) {
                error = "The result file is cut short.";
                return false;
            }
            out.fields.push_back(std::move(field));
        }
    } catch (const std::exception& e) {
        error = std::string("The result file's header could not be read: ") + e.what();
        return false;
    }
    return true;
}

}} // namespace Borpa::Sim
