#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Calculix.hpp"
#include "Mesh.hpp"

namespace Borpa { namespace Sim {

struct ResultField
{
    std::string        name;
    int                components{ 1 };
    std::string        unit;
    double             min{ 0 }; // of the magnitude, for vector fields
    double             max{ 0 };
    std::vector<float> values; // components per surface node
};

// What the page draws: the outer surface of the mesh (corner nodes only), each triangle tagged
// with its Hew face index, and fields on its nodes. `summary_json` is a JSON object written into
// the file's header as "summary".
struct SurfaceResult
{
    std::vector<float>       positions;
    std::vector<uint32_t>    triangles;
    std::vector<uint32_t>    face_index;
    std::vector<ResultField> fields;
    std::string              summary_json{ "{}" };
};

// Surface of a static result: fields "displacement" (3 components, mm) and "vonMises" (MPa).
SurfaceResult make_static_surface_result(const QuadraticMesh& mesh, const StaticResult& result);

// BSR1 layout (little-endian): "BSR1", u32 header length, UTF-8 JSON header padded with spaces so
// the arrays that follow start on a 4-byte boundary, then positions (f32 x 3n), triangles
// (u32 x 3t), face index (u32 x t), and each field's values (f32 x n x components) in the
// header's order. The header holds "nodes", "triangles", "fields" and "summary".
bool write_result_file(const std::string& path, const SurfaceResult& result, std::string& error);
bool read_result_file(const std::string& path, SurfaceResult& out, std::string& error);

}} // namespace Borpa::Sim
