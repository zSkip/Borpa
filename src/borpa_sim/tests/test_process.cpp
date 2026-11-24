#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Pipeline.hpp"
#include "Process.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Borpa::Sim;
namespace fs = std::filesystem;

namespace {

struct Run
{
    ProcessOutcome           outcome;
    std::vector<std::string> lines;
};

Run run_child(std::vector<std::string> args, double stall = 120, double wall = 60, std::atomic<bool>* cancel = nullptr,
              std::vector<std::pair<std::string, std::string>> env = {})
{
    static std::atomic<bool> never{ false };
    std::string              dir = make_job_dir(SIM_TEST_WORK, "proc");
    ProcessSpec              spec;
    spec.exe           = SIM_TEST_CHILD;
    spec.args          = std::move(args);
    spec.cwd           = dir;
    spec.path_dirs     = { dir };
    spec.env           = std::move(env);
    spec.log_path      = (fs::u8path(dir) / "child.log").u8string();
    spec.stall_seconds = stall;
    spec.wall_seconds  = wall;
    Run        r;
    std::mutex m;
    r.outcome = run_process(spec, cancel ? *cancel : never, [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(m);
        r.lines.push_back(line);
    });
    return r;
}

#ifdef _WIN32
bool process_alive(unsigned long pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h)
        return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}
#endif

} // namespace

TEST_CASE("exit codes come back", "[sim][process]")
{
    Run ok = run_child({ "exit0" });
    CHECK(ok.outcome.end == ProcessEnd::Exited);
    CHECK(ok.outcome.exit_code == 0);

    Run bad = run_child({ "fail" });
    CHECK(bad.outcome.end == ProcessEnd::Exited);
    CHECK(bad.outcome.exit_code == 3);
    REQUIRE(bad.lines.size() == 1);
    CHECK(bad.lines[0] == "Error   : boom");
}

TEST_CASE("output is split into lines and logged", "[sim][process]")
{
    Run r = run_child({ "lines" });
    CHECK(r.lines == std::vector<std::string>{ "a", "b", "c" });
    std::string log = tail_of_file((fs::u8path(SIM_TEST_WORK) / "proc" / "child.log").u8string(), 100);
    CHECK(log == "a\r\nb\nc");
}

TEST_CASE("the child sees only its own folder and the system folders on PATH", "[sim][process]")
{
    Run r = run_child({ "env" }, 120, 60, nullptr, { { "OMP_NUM_THREADS", "1" } });
    REQUIRE(r.lines.size() == 2);
    std::string path = r.lines[0];
    CHECK(path.rfind("PATH=" + (fs::u8path(SIM_TEST_WORK) / "proc").u8string() + ";", 0) == 0);
    CHECK(path.find("Git") == std::string::npos);
    CHECK(r.lines[1] == "OMP_NUM_THREADS=1");
}

TEST_CASE("a silent idle child is stopped as stalled", "[sim][process]")
{
    auto t0 = std::chrono::steady_clock::now();
    Run  r  = run_child({ "hang" }, 1.5);
    CHECK(r.outcome.end == ProcessEnd::Stalled);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));

    Run talk = run_child({ "talk-then-hang" }, 1.5);
    CHECK(talk.outcome.end == ProcessEnd::Stalled);
    CHECK(talk.lines.size() == 3);
}

TEST_CASE("a silent child that is working is not stalled", "[sim][process]")
{
    Run r = run_child({ "busy", "3" }, 1.5);
    CHECK(r.outcome.end == ProcessEnd::Exited);
    CHECK(r.outcome.exit_code == 0);
}

TEST_CASE("the wall-clock limit stops a run", "[sim][process]")
{
    Run r = run_child({ "busy", "30" }, 120, 1.5);
    CHECK(r.outcome.end == ProcessEnd::TimedOut);
    CHECK(r.outcome.wall_seconds < 10);
}

TEST_CASE("cancelling kills the child and its children", "[sim][process]")
{
    std::string       pid_file = (fs::u8path(SIM_TEST_WORK) / "grandchild.pid").u8string();
    std::error_code   ec;
    fs::remove(fs::u8path(pid_file), ec);
    std::atomic<bool> cancel{ false };
    std::thread       canceller([&] {
        for (int i = 0; i < 100 && !fs::exists(fs::u8path(pid_file)); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel = true;
    });
    Run r = run_child({ "grandchild", pid_file }, 120, 60, &cancel);
    canceller.join();
    CHECK(r.outcome.end == ProcessEnd::Cancelled);
#ifdef _WIN32
    unsigned long pid = 0;
    std::ifstream(fs::u8path(pid_file)) >> pid;
    REQUIRE(pid != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(process_alive(pid));
#endif
}

TEST_CASE("a missing program fails to start", "[sim][process]")
{
    static std::atomic<bool> never{ false };
    ProcessSpec              spec;
    spec.exe = (fs::u8path(SIM_TEST_WORK) / "no-such-program.exe").u8string();
    spec.cwd = SIM_TEST_WORK;
    ProcessOutcome o = run_process(spec, never, nullptr);
    CHECK(o.end == ProcessEnd::FailedToStart);
    CHECK_FALSE(o.error.empty());
}
