// End-to-end runs with the real mesher and solver against closed-form answers
// (solver-bakeoff.md section 5.1). Skipped unless BORPA_SOLVERS_DIR names a solvers folder.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "Pipeline.hpp"
#include "ResultFile.hpp"
#include "test_geometry.hpp"

using namespace Borpa::Sim;
namespace fs = std::filesystem;

namespace {

SolverBundle solvers_or_skip()
{
    SolverBundle b = find_solvers("");
    if (!b.has_structural())
        SKIP("BORPA_SOLVERS_DIR does not name a folder with gmsh/gmsh.exe and calculix/ccx_i4_static.exe");
    return b;
}

JobOutcome run(const JobRequest& req, SurfaceResult& surface)
{
    std::atomic<bool> cancel{ false };
    JobContext        ctx;
    ctx.solvers = solvers_or_skip();
    ctx.job_dir = make_job_dir(SIM_TEST_WORK, req.job);
    ctx.cancel  = &cancel;
    JobOutcome out = run_static_job(req, ctx);
    INFO(out.code << ": " << out.message << "\n" << out.log_tail);
    REQUIRE(out.ok);
    std::string error;
    REQUIRE(read_result_file(out.result_path, surface, error));
    std::printf("%s: %s\n", req.job.c_str(), out.summary_json.c_str());
    return out;
}

} // namespace

TEST_CASE("cantilever tip deflection matches beam theory", "[sim][benchmark]")
{
    JobRequest req;
    req.job                  = "bench_cantilever";
    req.geometry             = Test::box(100, 10, 10);
    req.study.object_id      = "1";
    req.study.material       = { "test", 2000, 0.3, 0 };
    req.study.fixtures       = { { { 0 } } };
    req.study.loads          = { { { 1 }, { 0, 0, -10 } } };
    req.study.mesh_size      = 2.5;
    SurfaceResult surface;
    JobOutcome    out = run(req, surface);

    // Mean vertical displacement of the loaded end. Timoshenko: 2.0156 mm; the bake-off got
    // 0.74 % less, the difference between a clamped 3-D block and beam theory.
    const ResultField* disp = nullptr;
    for (const auto& f : surface.fields)
        if (f.name == "displacement")
            disp = &f;
    REQUIRE(disp);
    double sum = 0;
    int    n   = 0;
    for (size_t i = 0; i < surface.positions.size() / 3; ++i)
        if (std::abs(surface.positions[3 * i] - 100.0f) < 1e-4f) {
            sum += disp->values[3 * i + 2];
            ++n;
        }
    REQUIRE(n > 0);
    double tip = -sum / n;
    INFO("tip deflection " << tip);
    CHECK(std::abs(tip - 2.0156) / 2.0156 < 0.015);

    // Supports carry the load.
    auto summary = nlohmann::json::parse(out.summary_json);
    CHECK(std::abs(summary["reactionForce"][2].get<double>() - 10.0) < 0.01);
}

TEST_CASE("plate with a hole shows the stress concentration", "[sim][benchmark]")
{
    JobRequest req;
    req.job             = "bench_plate";
    req.geometry        = Test::plate_with_hole(200, 100, 5, 5, 64);
    req.study.object_id = "1";
    req.study.material  = { "test", 2000, 0.3, 0 };
    req.study.fixtures  = { { { 2 } } };                 // x = 0
    req.study.loads     = { { { 3 }, { 5000, 0, 0 } } }; // x = 200: 10 MPa over 100 x 5 mm
    req.study.mesh_size = 5;                             // as in the bake-off
    Refinement hole;
    for (uint32_t f = 6; f < req.geometry.faces.size(); ++f)
        hole.faces.push_back(f);
    hole.size = 0.5;
    req.study.refine.push_back(hole);
    SurfaceResult surface;
    run(req, surface);

    // Peak at the hole edge. Plane-stress reference 30.25 MPa; through the thickness the peak runs
    // a few percent higher (bake-off: 31.55 MPa with Gmsh and CalculiX).
    const ResultField* vm = nullptr;
    for (const auto& f : surface.fields)
        if (f.name == "vonMises")
            vm = &f;
    REQUIRE(vm);
    double peak = 0;
    for (size_t i = 0; i < surface.positions.size() / 3; ++i) {
        double dx = surface.positions[3 * i] - 100.0, dy = surface.positions[3 * i + 1] - 50.0;
        if (std::hypot(dx, dy) < 6.0)
            peak = std::max(peak, double(vm->values[i]));
    }
    INFO("peak von Mises at the hole " << peak);
    CHECK(peak > 28.0);
    CHECK(peak < 33.5);
}
