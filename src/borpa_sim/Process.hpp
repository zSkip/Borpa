#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Borpa { namespace Sim {

// One solver or mesher run. Strings are UTF-8.
struct ProcessSpec
{
    std::string              exe;
    std::vector<std::string> args;
    std::string              cwd;
    // PATH for the child is these folders followed by the system folders, nothing else, so a
    // solver can never load a DLL from another program's folder (solver-bakeoff.md section 13).
    std::vector<std::string>                         path_dirs;
    std::vector<std::pair<std::string, std::string>> env;
    // Standard output and error, merged, are written here as they arrive.
    std::string log_path;
    // The run fails as stalled when it has printed nothing and used no CPU time for this long,
    // and as timed out when it has run this long in all.
    double stall_seconds{ 120 };
    double wall_seconds{ 1800 };
};

enum class ProcessEnd
{
    Exited,
    FailedToStart,
    Stalled,
    TimedOut,
    Cancelled,
};

struct ProcessOutcome
{
    ProcessEnd  end{ ProcessEnd::FailedToStart };
    int         exit_code{ -1 }; // when end == Exited
    std::string error;           // when end == FailedToStart
    double      wall_seconds{ 0 };
};

// Called with each complete line of output, without its line ending, on a reader thread.
using LineCallback = std::function<void(const std::string& line)>;

// Runs the program to completion. On Windows the child and everything it starts run in a Job
// Object, so a stall, a timeout, `cancel` or the run ending kills the whole tree; no console
// window is shown. Exit code 0 does not mean the run worked: callers check its output.
ProcessOutcome run_process(const ProcessSpec& spec, const std::atomic<bool>& cancel, const LineCallback& on_line);

// The last `max_bytes` of a file, starting at a line boundary when one is in range.
std::string tail_of_file(const std::string& path, size_t max_bytes);

}} // namespace Borpa::Sim
