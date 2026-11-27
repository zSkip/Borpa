#include "HewPlate.hpp"

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <miniz.h>

#include <algorithm>
#include <cstdlib>
#include <numeric>

namespace Slic3r { namespace GUI { namespace HewPlate {

namespace {

std::string xml_unescape(const std::string& s)
{
    static const std::pair<const char*, char> entities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&')
            for (const auto& [entity, c] : entities)
                if (s.compare(i, std::char_traits<char>::length(entity), entity) == 0) {
                    out += c;
                    i += std::char_traits<char>::length(entity);
                    replaced = true;
                    break;
                }
        if (!replaced)
            out += s[i++];
    }
    return out;
}

// Reads attribute `name` of the tag starting at `tag` (up to its closing ">") into `value`.
bool attribute(const std::string& xml, size_t tag, const char* name, std::string& value)
{
    const size_t end = xml.find('>', tag);
    if (end == std::string::npos)
        return false;
    const std::string key = std::string(" ") + name + "=\"";
    const size_t      at  = xml.find(key, tag);
    if (at == std::string::npos || at > end)
        return false;
    const size_t from = at + key.size();
    const size_t to   = xml.find('"', from);
    if (to == std::string::npos || to > end)
        return false;
    value = xml_unescape(xml.substr(from, to - from));
    return true;
}

// Each tag `<prefix ...>` of `xml`, in document order.
template<class F> void each_tag(const std::string& xml, const char* prefix, F&& f)
{
    const std::string open = std::string("<") + prefix + " ";
    for (size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + open.size()))
        f(at);
}

// An object as one mesh in bed coordinates (its first instance).
indexed_triangle_set world_mesh(const ModelObject& object)
{
    indexed_triangle_set out;
    if (object.instances.empty())
        return out;
    const Transform3d& instance = object.instances.front()->get_matrix();
    for (const ModelVolume* volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        indexed_triangle_set its = volume->mesh().its;
        const Transform3d    m   = instance * volume->get_matrix();
        for (stl_vertex& v : its.vertices)
            v = (m * v.cast<double>()).cast<float>();
        its_merge(out, its);
    }
    return out;
}

bool touches(const indexed_triangle_set& a, const indexed_triangle_set& b, const AABBTreeIndirect::Tree3f& b_tree, float tolerance)
{
    for (const stl_vertex& v : a.vertices) {
        float r2 = tolerance * tolerance;
        if (AABBTreeIndirect::is_any_triangle_in_radius(b.vertices, b.indices, b_tree, v, r2))
            return true;
    }
    return false;
}

} // namespace

std::vector<std::string> item_material_names(const std::string& path_3mf)
{
    std::string xml;
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        if (!open_zip_reader(&zip, path_3mf))
            return {};
        size_t size = 0;
        void*  data = mz_zip_reader_extract_file_to_heap(&zip, "3D/3dmodel.model", &size, 0);
        if (data != nullptr) {
            xml.assign(static_cast<const char*>(data), size);
            mz_free(data);
        }
        close_zip_reader(&zip);
    }
    if (xml.empty())
        return {};

    // Hew writes one <basematerials> group; its <base> entries in order are the pindex values.
    std::vector<std::string> bases;
    each_tag(xml, "base", [&](size_t at) {
        std::string name;
        attribute(xml, at, "name", name);
        bases.push_back(name);
    });
    std::map<std::string, std::string> material_of_object;
    each_tag(xml, "object", [&](size_t at) {
        std::string id, pindex;
        if (attribute(xml, at, "id", id) && attribute(xml, at, "pindex", pindex)) {
            const long i = std::strtol(pindex.c_str(), nullptr, 10);
            if (i >= 0 && size_t(i) < bases.size())
                material_of_object[id] = bases[size_t(i)];
        }
    });
    std::vector<std::string> out;
    each_tag(xml, "item", [&](size_t at) {
        std::string id;
        attribute(xml, at, "objectid", id);
        const auto it = material_of_object.find(id);
        out.push_back(it == material_of_object.end() ? std::string() : it->second);
    });
    return out;
}

std::vector<std::vector<size_t>> touching_groups(const Model& model, double tolerance)
{
    const size_t n = model.objects.size();
    std::vector<size_t> parent(n);
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto find = [&parent](size_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };

    std::vector<indexed_triangle_set> meshes;
    std::vector<BoundingBoxf3>        boxes;
    meshes.reserve(n);
    for (const ModelObject* object : model.objects) {
        meshes.push_back(world_mesh(*object));
        BoundingBoxf3 box = bounding_box(meshes.back());
        if (box.defined)
            box.offset(tolerance);
        boxes.push_back(box);
    }
    std::vector<AABBTreeIndirect::Tree3f> trees(n);
    std::vector<bool>                     built(n, false);
    auto tree = [&](size_t i) -> const AABBTreeIndirect::Tree3f& {
        if (!built[i]) {
            trees[i] = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(meshes[i].vertices, meshes[i].indices);
            built[i] = true;
        }
        return trees[i];
    };

    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (find(i) == find(j) || !boxes[i].defined || !boxes[j].defined || !boxes[i].overlap(boxes[j]))
                continue;
            if (touches(meshes[i], meshes[j], tree(j), float(tolerance)) || touches(meshes[j], meshes[i], tree(i), float(tolerance)))
                parent[find(j)] = find(i);
        }

    std::map<size_t, std::vector<size_t>> by_root;
    for (size_t i = 0; i < n; ++i)
        by_root[find(i)].push_back(i);
    std::vector<std::vector<size_t>> groups;
    for (auto& [root, members] : by_root)
        groups.push_back(std::move(members));
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
    return groups;
}

SlotPlan plan_slots(const std::vector<std::string>& needed,
                    const std::vector<std::string>& current,
                    size_t                          physical,
                    size_t                          max_slots,
                    size_t                          printer_holds,
                    const std::vector<bool>&        known)
{
    SlotPlan plan;
    size_t   next = physical; // slots 1..physical exist; a new one becomes physical + 1
    for (size_t i = 0; i < needed.size(); ++i) {
        const std::string& preset = needed[i];
        if (plan.slot_of.count(preset) != 0)
            continue;
        const auto in_use = std::find(current.begin(), current.begin() + std::min(physical, current.size()), preset);
        int        slot   = 1;
        if (in_use != current.begin() + std::min(physical, current.size())) {
            slot = int(in_use - current.begin()) + 1;
        } else if (i >= known.size() || !known[i]) {
            plan.unknown.push_back(preset);
        } else if (next < max_slots) {
            slot = int(++next);
            plan.added.push_back(preset);
        } else {
            plan.overflow.push_back(preset);
        }
        plan.slot_of[preset] = slot;
        if (size_t(slot) > printer_holds)
            plan.overflow.push_back(preset);
    }
    return plan;
}

}}} // namespace Slic3r::GUI::HewPlate
