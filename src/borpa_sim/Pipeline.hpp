#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "Request.hpp"

namespace Borpa { namespace Sim {

// The bundled programs. Each lives in its own folder under the solvers folder, with its DLLs
// beside it: `gmsh/gmsh.exe` and `calculix/ccx_i4_static.exe` (no `.exe` off Windows).
struct SolverBundle
{
    std::string gmsh;     // empty when not found
    std::string calculix; // empty when not found

    bool has_mesher() const { return !gmsh.empty(); }
    bool has_structural() const { return !gmsh.empty() && !calculix.empty(); }
};

// Looks in `solvers_dir`, or in the folder named by the BORPA_SOLVERS_DIR environment variable
// when that is set (for development).
SolverBundle find_solvers(const std::string& solvers_dir);

enum class Stage
{
    Mesh,
    Solve,
    Read,
};

const char* stage_name(Stage stage);

struct Progress
{
    Stage       stage{ Stage::Mesh };
    double      fraction{ -1 }; // 0..1 within the stage, or -1 when unknown
    std::string message;
};

struct JobContext
{
    SolverBundle             solvers;
    std::string              job_dir; // exists and is empty
    const std::atomic<bool>* cancel{ nullptr };
    // Called from the job's thread and from process reader threads, never at the same time.
    std::function<void(const Progress&)> on_progress;
    double                               stall_seconds{ 120 };
    double                               wall_seconds{ 1800 };
};

struct JobOutcome
{
    bool ok{ false };
    // On failure: solver-missing, bad-study, mesh-failed, solve-failed, no-result, timeout,
    // stalled or cancelled.
    std::string code;
    std::string message;
    std::string log_tail;
    std::string result_path;  // result.bsr in the job folder, on success
    std::string summary_json; // nodes, elements, maxDisplacement, maxVonMises, reactionForce, wallSeconds
};

// Runs a linear static study: mesh with Gmsh, promote to quadratic tetrahedra, write and run
// CalculiX on one thread, read its results and write result.bsr. Checks each stage's output
// rather than trusting exit codes. Blocking; run it off the GUI thread.
JobOutcome run_static_job(const JobRequest& request, const JobContext& context);

// Creates `<root>/<job>` empty (removing an earlier folder of that name) and returns its path,
// or an empty string on failure.
std::string make_job_dir(const std::string& root, const std::string& job);

// Removes job folders under `root` last written more than `days` days ago.
void remove_old_job_dirs(const std::string& root, int days);

}} // namespace Borpa::Sim
