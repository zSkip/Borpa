#include "Pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <nlohmann/json.hpp>

#include "Calculix.hpp"
#include "Gmsh.hpp"
#include "Mesh.hpp"
#include "Paths.hpp"
#include "Process.hpp"
#include "ResultFile.hpp"

namespace Borpa { namespace Sim {

namespace fs = std::filesystem;

#ifdef _WIN32
static const char* const k_exe = ".exe";
#else
static const char* const k_exe = "";
#endif

SolverBundle find_solvers(const std::string& solvers_dir)
{
    std::string dir = solvers_dir;
    if (const char* env = std::getenv("BORPA_SOLVERS_DIR"); env && *env)
        dir = env;
    SolverBundle b;
    if (dir.empty())
        return b;
    std::error_code ec;
    fs::path        gmsh = path_from_utf8(dir) / "gmsh" / (std::string("gmsh") + k_exe);
    fs::path        ccx  = path_from_utf8(dir) / "calculix" / (std::string("ccx_i4_static") + k_exe);
    if (fs::is_regular_file(gmsh, ec))
        b.gmsh = path_to_utf8(gmsh);
    if (fs::is_regular_file(ccx, ec))
        b.calculix = path_to_utf8(ccx);
    return b;
}

const char* stage_name(Stage s)
{
    switch (s) {
    case Stage::Mesh: return "mesh";
    case Stage::Solve: return "solve";
    case Stage::Read: return "read";
    }
    return "mesh";
}

namespace {

struct Runner
{
    const JobContext& ctx;
    JobOutcome&       out;

    void progress(Stage s, double fraction, const std::string& message) const
    {
        if (ctx.on_progress)
            ctx.on_progress(Progress{ s, fraction, message });
    }
    bool cancelled() const { return ctx.cancel && ctx.cancel->load(); }
    bool fail(const char* code, const std::string& message, const std::string& log = std::string())
    {
        out.ok      = false;
        out.code    = code;
        out.message = message;
        if (!log.empty())
            out.log_tail = tail_of_file(log, 4000);
        return false;
    }

    // Runs one program. `first_error` collects the first line the program marked as an error.
    bool run(const std::string& exe, const std::vector<std::string>& args, const char* log_name,
             std::vector<std::pair<std::string, std::string>> env, const char* failed_code, const std::string& what,
             const LineCallback& on_line, const std::string& first_error)
    {
        static const std::atomic<bool> never{ false };
        ProcessSpec                    spec;
        spec.exe           = exe;
        spec.args          = args;
        spec.cwd           = ctx.job_dir;
        spec.path_dirs     = { path_to_utf8(path_from_utf8(exe).parent_path()) };
        spec.env           = std::move(env);
        spec.log_path      = path_to_utf8(path_from_utf8(ctx.job_dir) / log_name);
        spec.stall_seconds = ctx.stall_seconds;
        spec.wall_seconds  = ctx.wall_seconds;
        ProcessOutcome p   = run_process(spec, ctx.cancel ? *ctx.cancel : never, on_line);
        switch (p.end) {
        case ProcessEnd::FailedToStart: return fail(failed_code, what + " could not start. " + p.error);
        case ProcessEnd::Cancelled: return fail("cancelled", "Cancelled.");
        case ProcessEnd::Stalled:
            return fail("stalled", what + " stopped responding: no output and no work for " +
                                       std::to_string(int(ctx.stall_seconds)) + " seconds.", spec.log_path);
        case ProcessEnd::TimedOut:
            return fail("timeout", what + " ran longer than the time limit.", spec.log_path);
        case ProcessEnd::Exited: break;
        }
        if (p.exit_code != 0 || !first_error.empty()) {
            std::string msg = what + " failed";
            msg += first_error.empty() ? " with exit code " + std::to_string(p.exit_code) + "." : ": " + first_error;
            return fail(failed_code, msg, spec.log_path);
        }
        return true;
    }
};

std::string trimmed(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

JobOutcome run_static_job(const JobRequest& request, const JobContext& ctx)
{
    JobOutcome out;
    Runner     r{ ctx, out };
    const auto start = std::chrono::steady_clock::now();

    std::string error;
    if (!validate_job_request(request, error)) {
        r.fail("bad-study", error);
        return out;
    }
    if (!ctx.solvers.has_structural()) {
        r.fail("solver-missing", "The mesher or the stress solver is not installed with Borpa.");
        return out;
    }

    // Mesh.
    r.progress(Stage::Mesh, 0, "Preparing the mesh");
    const double size = request.study.mesh_size > 0 ? request.study.mesh_size : automatic_mesh_size(request.geometry);
    if (!write_gmsh_input(ctx.job_dir, request.geometry, request.study, size, error)) {
        r.fail("mesh-failed", error);
        return out;
    }
    std::string gmsh_error;
    auto        gmsh_line = [&](const std::string& line) {
        if (line.rfind("Error", 0) == 0) {
            if (gmsh_error.empty())
                gmsh_error = trimmed(line.substr(line.find(':') + 1));
        } else if (line.find("Meshing 1D") != std::string::npos)
            r.progress(Stage::Mesh, 0.05, "Meshing edges");
        else if (line.find("Meshing 2D") != std::string::npos)
            r.progress(Stage::Mesh, 0.15, "Meshing faces");
        else if (line.find("Meshing 3D") != std::string::npos)
            r.progress(Stage::Mesh, 0.4, "Meshing the volume");
        else if (line.find("Optimizing mesh (Netgen)") != std::string::npos)
            r.progress(Stage::Mesh, 0.75, "Improving the mesh");
    };
    if (!r.run(ctx.solvers.gmsh, gmsh_arguments(ctx.job_dir), "mesh.log", {}, "mesh-failed", "The mesher", gmsh_line,
               gmsh_error) ||
        r.cancelled()) {
        if (r.cancelled())
            r.fail("cancelled", "Cancelled.");
        return out;
    }
    const std::string mesh_log = path_to_utf8(path_from_utf8(ctx.job_dir) / "mesh.log");
    TetMesh           linear;
    QuadraticMesh     mesh;
    if (!read_gmsh_msh22(path_to_utf8(path_from_utf8(ctx.job_dir) / "part.msh"), request.geometry.faces.size(), linear,
                         error) ||
        !promote_to_quadratic(linear, mesh, error)) {
        r.fail("mesh-failed", error, mesh_log);
        return out;
    }

    // Solve.
    r.progress(Stage::Solve, 0, "Starting the solver");
    if (!write_calculix_static(ctx.job_dir, mesh, request.study, error)) {
        r.fail("solve-failed", error);
        return out;
    }
    std::string ccx_error;
    auto        ccx_line = [&](const std::string& line) {
        if (line.find("*ERROR") != std::string::npos) {
            if (ccx_error.empty())
                ccx_error = trimmed(line);
        } else if (line.find("Determining the structure") != std::string::npos)
            r.progress(Stage::Solve, 0.1, "Setting up the equations");
        else if (line.find("Factoring the system") != std::string::npos)
            r.progress(Stage::Solve, 0.3, "Solving the equations");
        // CalculiX prints nothing distinct after the solve ("stress calculation" lines come
        // before it), and its output arrives in buffered chunks, so the rest of the stage has no
        // finer progress.
    };
    // One thread only: the multi-threaded SPOOLES path in these Windows builds returns wrong
    // results with exit code 0 when the machine is busy (solver-bakeoff.md section 5.3).
    if (!r.run(ctx.solvers.calculix, calculix_arguments(), "solve.log",
               { { "OMP_NUM_THREADS", "1" }, { "CCX_NPROC_EQUATION_SOLVER", "1" }, { "CCX_NPROC_STRESS", "1" } },
               "solve-failed", "The solver", ccx_line, ccx_error)) {
        return out;
    }
    const std::string solve_log = path_to_utf8(path_from_utf8(ctx.job_dir) / "solve.log");

    // Read.
    r.progress(Stage::Read, 0, "Reading the results");
    StaticResult result;
    if (!read_calculix_static(ctx.job_dir, mesh.nodes.size(), result, error)) {
        r.fail("no-result", error, solve_log);
        return out;
    }

    // The reactions must balance the applied forces. A solver that returns a wrong answer with
    // exit code 0 fails this check.
    Vec3   applied{ 0, 0, 0 }, reaction{ 0, 0, 0 };
    double max_disp = 0, max_vm = 0;
    for (const ForceLoad& l : request.study.loads)
        for (int k = 0; k < 3; ++k)
            applied[k] += l.vector[k];
    for (uint32_t n : fixture_nodes(mesh, request.study))
        for (int k = 0; k < 3; ++k)
            reaction[k] += result.force[n][k];
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Vec3& d = result.displacement[i];
        max_disp = std::max(max_disp, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        max_vm   = std::max(max_vm, von_mises(result.stress[i]));
    }
    const double load_size = std::sqrt(applied[0] * applied[0] + applied[1] * applied[1] + applied[2] * applied[2]);
    double       imbalance = 0;
    for (int k = 0; k < 3; ++k)
        imbalance += (reaction[k] + applied[k]) * (reaction[k] + applied[k]);
    imbalance = std::sqrt(imbalance);
    if (!(imbalance <= 1e-3 * load_size)) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "The solver's result does not balance: the supports carry %.6g N against %.6g N applied.",
                      std::sqrt(reaction[0] * reaction[0] + reaction[1] * reaction[1] + reaction[2] * reaction[2]),
                      load_size);
        r.fail("solve-failed", buf, solve_log);
        return out;
    }

    SurfaceResult  surface = make_static_surface_result(mesh, result);
    nlohmann::json summary = {
        { "nodes", mesh.nodes.size() },
        { "elements", mesh.tets.size() },
        { "meshSize", size },
        { "maxDisplacement", max_disp },
        { "maxVonMises", max_vm },
        { "reactionForce", { reaction[0], reaction[1], reaction[2] } },
        { "wallSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() },
    };
    surface.summary_json = summary.dump();
    const std::string path = path_to_utf8(path_from_utf8(ctx.job_dir) / "result.bsr");
    if (!write_result_file(path, surface, error)) {
        r.fail("no-result", error);
        return out;
    }
    r.progress(Stage::Read, 1, "Done");
    out.ok           = true;
    out.result_path  = path;
    out.summary_json = surface.summary_json;
    return out;
}

std::string make_job_dir(const std::string& root, const std::string& job)
{
    if (!is_valid_job_id(job))
        return {};
    std::error_code ec;
    fs::path        dir = path_from_utf8(root) / job;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec))
        return {};
    return path_to_utf8(dir);
}

void remove_old_job_dirs(const std::string& root, int days)
{
    std::error_code ec;
    const auto      cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * days);
    for (fs::directory_iterator it(path_from_utf8(root), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && is_valid_job_id(path_to_utf8(it->path().filename())) &&
            fs::last_write_time(it->path(), e2) < cutoff && !e2)
            fs::remove_all(it->path(), e2);
    }
}

}} // namespace Borpa::Sim
