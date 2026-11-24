#include "Request.hpp"

#include <cmath>
#include <cstring>
#include <exception>

#include <nlohmann/json.hpp>

#include "Base64.hpp"

namespace Borpa { namespace Sim {

using nlohmann::json;

bool is_valid_job_id(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

namespace {

// Decodes base64 into a vector of 4-byte values. The page writes typed arrays in the platform's
// byte order, which is little-endian on every target Borpa supports, as is this side.
template<class T> bool decode_array(const json& j, const char* key, std::vector<T>& out, std::string& error)
{
    static_assert(sizeof(T) == 4, "4-byte elements only");
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        error = std::string("The geometry has no \"") + key + "\" array.";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!base64_decode(it->get<std::string>(), bytes) || bytes.size() % 4 != 0) {
        error = std::string("The geometry's \"") + key + "\" array is not valid base64 of 4-byte values.";
        return false;
    }
    out.resize(bytes.size() / 4);
    if (!bytes.empty())
        std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

bool read_faces(const json& j, std::vector<uint32_t>& out, const char* what, std::string& error)
{
    auto it = j.find("faces");
    if (it == j.end() || !it->is_array()) {
        error = std::string("A ") + what + " has no face list.";
        return false;
    }
    out.clear();
    for (const json& f : *it) {
        if (!f.is_number_unsigned()) {
            error = std::string("A ") + what + " lists a face that is not a face number.";
            return false;
        }
        out.push_back(f.get<uint32_t>());
    }
    return true;
}

bool finite(double v) { return std::isfinite(v); }

} // namespace

bool parse_job_request(const std::string& text, JobRequest& out, std::string& error)
{
    out = JobRequest{};
    try {
        const json msg = json::parse(text);
        if (!msg.is_object()) {
            error = "The request is not a JSON object.";
            return false;
        }
        out.job = msg.value("job", std::string());
        if (!is_valid_job_id(out.job)) {
            error = "The job id is missing or has characters other than letters, digits, '_' and '-'.";
            return false;
        }

        const json& study = msg.at("study");
        if (study.value("version", 0) != 1) {
            error = "This version of Borpa reads study version 1 only.";
            return false;
        }
        if (study.value("kind", std::string()) != "static") {
            error = "This version of Borpa runs static stress studies only.";
            return false;
        }
        StaticStudy& s = out.study;
        s.object_id    = study.value("object", std::string());

        const json& m              = study.at("material");
        s.material.name            = m.value("name", std::string());
        s.material.youngs_modulus  = m.value("E", 0.0);
        s.material.poisson_ratio   = m.value("nu", 0.0);
        s.material.density         = m.value("density", 0.0);

        for (const json& f : study.value("fixtures", json::array())) {
            Fixture fx;
            if (!read_faces(f, fx.faces, "fixture", error))
                return false;
            s.fixtures.push_back(std::move(fx));
        }
        for (const json& l : study.value("loads", json::array())) {
            if (l.value("type", std::string()) != "force") {
                error = "This version of Borpa applies forces only.";
                return false;
            }
            ForceLoad load;
            if (!read_faces(l, load.faces, "force", error))
                return false;
            const json& v = l.at("vector");
            if (!v.is_array() || v.size() != 3) {
                error = "A force has no x, y, z vector.";
                return false;
            }
            for (int k = 0; k < 3; ++k)
                load.vector[k] = v[k].get<double>();
            s.loads.push_back(std::move(load));
        }
        if (auto mesh = study.find("mesh"); mesh != study.end() && mesh->is_object()) {
            auto size = mesh->find("size");
            if (size != mesh->end() && !size->is_null())
                s.mesh_size = size->get<double>();
            for (const json& r : mesh->value("refine", json::array())) {
                Refinement ref;
                if (!read_faces(r, ref.faces, "mesh refinement", error))
                    return false;
                ref.size = r.at("size").get<double>();
                s.refine.push_back(std::move(ref));
            }
        }

        const json& objects = msg.at("geometry").at("objects");
        if (!objects.is_array() || objects.size() != 1) {
            error = "A study in this version of Borpa holds exactly one Object.";
            return false;
        }
        const json&      o = objects[0];
        SurfaceGeometry& g = out.geometry;
        g.object_id        = o.value("id", std::string());
        for (const json& f : o.at("faces"))
            g.faces.push_back(f.get<std::string>());
        if (!decode_array(o, "positions", g.positions, error) || !decode_array(o, "indices", g.indices, error) ||
            !decode_array(o, "faceIndex", g.face_index, error))
            return false;
    } catch (const std::exception& e) {
        error = std::string("The request could not be read: ") + e.what();
        return false;
    }
    return validate_job_request(out, error);
}

bool validate_job_request(const JobRequest& r, std::string& error)
{
    const SurfaceGeometry& g = r.geometry;
    const StaticStudy&     s = r.study;

    if (!is_valid_job_id(r.job)) {
        error = "The job id is missing or has characters other than letters, digits, '_' and '-'.";
        return false;
    }
    if (s.object_id.empty() || s.object_id != g.object_id) {
        error = "The study and the geometry name different Objects.";
        return false;
    }
    if (g.positions.size() % 3 != 0 || g.indices.size() % 3 != 0 || g.triangle_count() == 0) {
        error = "The Object's surface has no triangles.";
        return false;
    }
    if (g.face_index.size() != g.triangle_count()) {
        error = "The Object's surface gives a face for some triangles and not others.";
        return false;
    }
    for (float v : g.positions)
        if (!std::isfinite(v)) {
            error = "The Object's surface has a point that is not a number.";
            return false;
        }
    for (uint32_t i : g.indices)
        if (i >= g.vertex_count()) {
            error = "The Object's surface refers to a point it does not have.";
            return false;
        }
    std::vector<bool> face_used(g.faces.size(), false);
    for (uint32_t f : g.face_index) {
        if (f >= g.faces.size()) {
            error = "The Object's surface refers to a face it does not list.";
            return false;
        }
        face_used[f] = true;
    }

    auto check_faces = [&](const std::vector<uint32_t>& faces, const char* what) {
        if (faces.empty()) {
            error = std::string("A ") + what + " has no faces.";
            return false;
        }
        for (uint32_t f : faces)
            if (f >= g.faces.size() || !face_used[f]) {
                error = std::string("A ") + what + " is on a face the Object no longer has.";
                return false;
            }
        return true;
    };

    const Material& m = s.material;
    if (!finite(m.youngs_modulus) || m.youngs_modulus <= 0) {
        error = "The material's stiffness (Young's modulus) must be above zero.";
        return false;
    }
    if (!finite(m.poisson_ratio) || m.poisson_ratio < 0 || m.poisson_ratio >= 0.5) {
        error = "The material's Poisson's ratio must be at least 0 and below 0.5.";
        return false;
    }
    if (s.fixtures.empty()) {
        error = "Fix at least one face, or the part is free to fly away.";
        return false;
    }
    for (const Fixture& f : s.fixtures)
        if (!check_faces(f.faces, "fixture"))
            return false;
    if (s.loads.empty()) {
        error = "Add at least one force.";
        return false;
    }
    bool any_force = false;
    for (const ForceLoad& l : s.loads) {
        if (!check_faces(l.faces, "force"))
            return false;
        for (double v : l.vector) {
            if (!finite(v)) {
                error = "A force has a component that is not a number.";
                return false;
            }
            any_force = any_force || v != 0;
        }
    }
    if (!any_force) {
        error = "Every force is zero.";
        return false;
    }
    if (!finite(s.mesh_size) || s.mesh_size < 0) {
        error = "The mesh size must be above zero, or automatic.";
        return false;
    }
    for (const Refinement& ref : s.refine) {
        if (!check_faces(ref.faces, "mesh refinement"))
            return false;
        if (!finite(ref.size) || ref.size <= 0) {
            error = "A mesh refinement's size must be above zero.";
            return false;
        }
    }
    return true;
}

}} // namespace Borpa::Sim
