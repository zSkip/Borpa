#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Borpa { namespace Sim {

// One Object's closed surface as the page sends it: welded vertices in world millimetres,
// triangles, and for each triangle the index of the Hew face it belongs to. Hew face ids are
// u64; they travel as decimal strings and triangles refer to them by index, so this side never
// handles 64-bit ids.
struct SurfaceGeometry
{
    std::string              object_id;
    std::vector<std::string> faces;
    std::vector<float>       positions;  // x, y, z per vertex
    std::vector<uint32_t>    indices;    // three per triangle
    std::vector<uint32_t>    face_index; // one per triangle, into `faces`

    size_t vertex_count() const { return positions.size() / 3; }
    size_t triangle_count() const { return indices.size() / 3; }
};

struct Material
{
    std::string name;
    double      youngs_modulus{ 0 }; // MPa
    double      poisson_ratio{ 0 };
    double      density{ 0 };        // tonne/mm³; carried for later studies, unused by static
};

// Faces are indices into SurfaceGeometry::faces.
struct Fixture
{
    std::vector<uint32_t> faces;
};

// A total force in newtons spread over its faces by area.
struct ForceLoad
{
    std::vector<uint32_t>  faces;
    std::array<double, 3>  vector{ { 0, 0, 0 } };
};

struct Refinement
{
    std::vector<uint32_t> faces;
    double                size{ 0 }; // mm
};

// Linear static stress on one Object. Units: mm, N, MPa, tonne/mm³.
struct StaticStudy
{
    std::string             object_id;
    Material                material;
    std::vector<Fixture>    fixtures;
    std::vector<ForceLoad>  loads;
    double                  mesh_size{ 0 }; // 0 = automatic
    std::vector<Refinement> refine;
};

struct JobRequest
{
    std::string     job;
    StaticStudy     study;
    SurfaceGeometry geometry;
};

// Job ids name a folder and a URL path segment: [A-Za-z0-9_-]{1,64}.
bool is_valid_job_id(const std::string& id);

// Parses the body of a `sim.run` message (docs: brief-s1-sim-foundation.md section 2) and checks
// it is a study this version can run. On failure returns false and says why in `error`, in words
// fit to show the user.
bool parse_job_request(const std::string& json, JobRequest& out, std::string& error);

// Checks a request built in code the same way parse_job_request checks a parsed one.
bool validate_job_request(const JobRequest& request, std::string& error);

}} // namespace Borpa::Sim
