#include "Process.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

#include "Paths.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Borpa { namespace Sim {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// Splits output into lines, writes it to the log, and counts bytes so the watchdog can tell
// whether anything arrived. Used from the reader thread only, apart from `bytes`.
struct OutputSink
{
    std::ofstream         log;
    const LineCallback&   on_line;
    std::string           pending;
    std::atomic<uint64_t> bytes{ 0 };

    OutputSink(const std::string& log_path, const LineCallback& cb) : on_line(cb)
    {
        if (!log_path.empty())
            log.open(path_from_utf8(log_path), std::ios::binary | std::ios::trunc);
    }
    void feed(const char* data, size_t n)
    {
        if (log.is_open()) {
            log.write(data, std::streamsize(n));
            log.flush();
        }
        bytes += n;
        pending.append(data, n);
        size_t start = 0, nl;
        while ((nl = pending.find('\n', start)) != std::string::npos) {
            std::string line = pending.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (on_line)
                on_line(line);
            start = nl + 1;
        }
        pending.erase(0, start);
    }
    void finish()
    {
        if (!pending.empty() && on_line)
            on_line(pending);
        pending.clear();
    }
};

#ifdef _WIN32

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    int          n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Quotes one argument the way the Microsoft C runtime splits a command line.
std::wstring quote_argument(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return a;
    std::wstring out = L"\"";
    for (auto it = a.begin();; ++it) {
        size_t backslashes = 0;
        while (it != a.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == a.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

bool same_name(const std::wstring& entry, const std::wstring& name)
{
    size_t eq = entry.find(L'=', 1);
    if (eq == std::wstring::npos || eq != name.size())
        return false;
    return CompareStringOrdinal(entry.c_str(), int(eq), name.c_str(), int(name.size()), TRUE) == CSTR_EQUAL;
}

// The parent's environment with PATH and the spec's own variables replaced, sorted by name as
// CreateProcess expects, as a double-NUL-terminated block.
std::wstring environment_block(const ProcessSpec& spec)
{
    std::vector<std::wstring> entries;
    std::vector<std::wstring> replaced{ L"PATH" };
    for (const auto& kv : spec.env)
        replaced.push_back(widen(kv.first));
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring e(p);
            bool         drop = false;
            for (const auto& r : replaced)
                drop = drop || same_name(e, r);
            if (!drop)
                entries.push_back(std::move(e));
        }
        FreeEnvironmentStringsW(env);
    }
    std::wstring path;
    for (const auto& d : spec.path_dirs)
        path += widen(d) + L";";
    wchar_t sys[MAX_PATH], win[MAX_PATH];
    UINT    ns = GetSystemDirectoryW(sys, MAX_PATH);
    UINT    nw = GetWindowsDirectoryW(win, MAX_PATH);
    path += std::wstring(sys, ns) + L";" + std::wstring(win, nw);
    entries.push_back(L"PATH=" + path);
    for (const auto& kv : spec.env)
        entries.push_back(widen(kv.first) + L"=" + widen(kv.second));
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& e : entries) {
        block += e;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::string last_error_text(const char* what)
{
    DWORD   code = GetLastError();
    wchar_t buf[512];
    DWORD   n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' '))
        --n;
    int         len = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
    std::string msg(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, int(n), msg.data(), len, nullptr, nullptr);
    return std::string(what) + ": " + msg + " (" + std::to_string(code) + ")";
}

struct Handle
{
    HANDLE h{ nullptr };
    Handle() = default;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset()
    {
        if (h)
            CloseHandle(h);
        h = nullptr;
    }
    explicit operator bool() const { return h != nullptr; }
};

#endif

} // namespace

#ifdef _WIN32

ProcessOutcome run_process(const ProcessSpec& spec, const std::atomic<bool>& cancel, const LineCallback& on_line)
{
    ProcessOutcome out;
    const auto     start = Clock::now();

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        out.error = last_error_text("CreateJobObject");
        return out;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE              rd_raw = nullptr, wr_raw = nullptr;
    if (!CreatePipe(&rd_raw, &wr_raw, &sa, 0)) {
        out.error = last_error_text("CreatePipe");
        return out;
    }
    Handle rd(rd_raw), wr(wr_raw);
    SetHandleInformation(rd.h, HANDLE_FLAG_INHERIT, 0);
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (!nul) {
        out.error = last_error_text("Opening NUL");
        return out;
    }

    // Only the two handles meant for the child are inherited: a GUI process has many inheritable
    // handles, and a pipe end leaked into another child would keep this pipe open.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char>            attr_buf(attr_size);
    auto                         attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE                       inherit[2] = { wr.h, nul.h };
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        out.error = last_error_text("Preparing the child's handles");
        return out;
    }
    struct AttrGuard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attr_guard{ attrs };

    STARTUPINFOEXW si{};
    si.StartupInfo.cb         = sizeof(si);
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = nul.h;
    si.StartupInfo.hStdOutput = wr.h;
    si.StartupInfo.hStdError  = wr.h;
    si.lpAttributeList        = attrs;

    const std::wstring exe     = widen(spec.exe);
    std::wstring       cmdline = quote_argument(exe);
    for (const auto& a : spec.args)
        cmdline += L" " + quote_argument(widen(a));
    std::wstring       env = environment_block(spec);
    const std::wstring cwd = widen(spec.cwd);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                        env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi)) {
        out.error = last_error_text(("Starting " + spec.exe).c_str());
        return out;
    }
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) {
        out.error = last_error_text("AssignProcessToJobObject");
        TerminateProcess(process.h, 1);
        return out;
    }
    ResumeThread(thread.h);
    thread.reset();
    wr.reset();
    nul.reset();

    OutputSink  sink(spec.log_path, on_line);
    std::thread reader([&] {
        char  buf[8192];
        DWORD got = 0;
        while (ReadFile(rd.h, buf, sizeof(buf), &got, nullptr) && got > 0)
            sink.feed(buf, got);
        sink.finish();
    });

    out.end                 = ProcessEnd::Exited;
    auto     last_activity  = Clock::now();
    ULONG64  last_cpu       = 0;
    uint64_t last_bytes     = 0;
    for (;;) {
        if (WaitForSingleObject(process.h, 200) == WAIT_OBJECT_0)
            break;
        if (cancel.load()) {
            out.end = ProcessEnd::Cancelled;
            break;
        }
        if (seconds_since(start) > spec.wall_seconds) {
            out.end = ProcessEnd::TimedOut;
            break;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
        QueryInformationJobObject(job.h, JobObjectBasicAccountingInformation, &acc, sizeof(acc), nullptr);
        ULONG64  cpu   = ULONG64(acc.TotalUserTime.QuadPart) + ULONG64(acc.TotalKernelTime.QuadPart);
        uint64_t bytes = sink.bytes.load();
        if (cpu != last_cpu || bytes != last_bytes) {
            last_cpu      = cpu;
            last_bytes    = bytes;
            last_activity = Clock::now();
        } else if (seconds_since(last_activity) > spec.stall_seconds) {
            out.end = ProcessEnd::Stalled;
            break;
        }
    }
    if (out.end == ProcessEnd::Exited) {
        DWORD code = 0;
        GetExitCodeProcess(process.h, &code);
        out.exit_code = int(code);
    }
    // Ends the run, or whatever the child left behind: their ends of the pipe close with them.
    TerminateJobObject(job.h, 1);
    WaitForSingleObject(process.h, 5000);
    reader.join();
    out.wall_seconds = seconds_since(start);
    return out;
}

#else

ProcessOutcome run_process(const ProcessSpec& spec, const std::atomic<bool>& cancel, const LineCallback& on_line)
{
    // Untested: Borpa runs on Windows first. The watchdog here sees output only, since a child's
    // CPU time is not reported before it exits.
    ProcessOutcome out;
    const auto     start = Clock::now();
    if (access(spec.exe.c_str(), X_OK) != 0) {
        out.error = "Cannot run " + spec.exe;
        return out;
    }
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        std::string s(*e);
        std::string name = s.substr(0, s.find('='));
        bool        drop = name == "PATH";
        for (const auto& kv : spec.env)
            drop = drop || name == kv.first;
        if (!drop)
            env_strings.push_back(s);
    }
    std::string path;
    for (const auto& d : spec.path_dirs)
        path += d + ":";
    env_strings.push_back("PATH=" + path + "/usr/bin:/bin");
    for (const auto& kv : spec.env)
        env_strings.push_back(kv.first + "=" + kv.second);
    std::vector<char*> envp, argv;
    for (auto& s : env_strings)
        envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> args{ spec.exe };
    args.insert(args.end(), spec.args.begin(), spec.args.end());
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) {
        out.error = "pipe() failed";
        return out;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        out.error = "fork() failed";
        return out;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0)
            _exit(127);
        execve(spec.exe.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    OutputSink  sink(spec.log_path, on_line);
    std::thread reader([&] {
        char    buf[8192];
        ssize_t got;
        while ((got = read(fds[0], buf, sizeof(buf))) > 0)
            sink.feed(buf, size_t(got));
        sink.finish();
    });
    out.end            = ProcessEnd::Exited;
    auto     last_seen = Clock::now();
    uint64_t last_bytes = 0;
    int      status     = 0;
    for (;;) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (cancel.load())
            out.end = ProcessEnd::Cancelled;
        else if (seconds_since(start) > spec.wall_seconds)
            out.end = ProcessEnd::TimedOut;
        else if (sink.bytes.load() != last_bytes) {
            last_bytes = sink.bytes.load();
            last_seen  = Clock::now();
        } else if (seconds_since(last_seen) > spec.stall_seconds)
            out.end = ProcessEnd::Stalled;
        if (out.end != ProcessEnd::Exited) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
    }
    kill(-pid, SIGKILL);
    reader.join();
    close(fds[0]);
    out.wall_seconds = seconds_since(start);
    return out;
}

#endif

std::string tail_of_file(const std::string& path, size_t max_bytes)
{
    std::ifstream in(path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    std::streamoff size = in.tellg();
    std::streamoff from = size > std::streamoff(max_bytes) ? size - std::streamoff(max_bytes) : 0;
    in.seekg(from);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (from > 0) {
        size_t nl = text.find('\n');
        if (nl != std::string::npos && nl + 1 < text.size())
            text.erase(0, nl + 1);
    }
    return text;
}

}} // namespace Borpa::Sim
