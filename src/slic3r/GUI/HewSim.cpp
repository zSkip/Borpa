#include "HewSim.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include "Pipeline.hpp"
#include "Request.hpp"

namespace Slic3r { namespace GUI {

namespace {

// Job folders older than this are removed when the Sim tab first starts a job runner.
constexpr int JOB_FOLDER_DAYS = 7;

std::string bundled_solvers_dir()
{
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    exe.AppendDir("solvers");
    return exe.GetPath().ToUTF8().data();
}

// ASCII-only JSON, so the script stays valid whatever the messages hold.
wxString event_script(const nlohmann::json& event)
{
    return wxString::FromUTF8("(function(){var h=window.__borpaHew;if(h&&h.simEvent){h.simEvent(" +
                              event.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace) + ");}})();");
}

} // namespace

HewSim::HewSim(wxEvtHandler* owner, std::function<void(const wxString&)> run_script)
    : m_owner(owner)
    , m_run_script(std::move(run_script))
    , m_solvers_dir(bundled_solvers_dir())
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    Borpa::Sim::remove_old_job_dirs(jobs_root(), JOB_FOLDER_DAYS);
}

HewSim::~HewSim()
{
    *m_alive = false;
    stop();
}

// Paths are UTF-8 here, as borpa_sim takes them: a narrow boost path would use the ANSI code page
// and break on a user name outside it.
std::string HewSim::jobs_root()
{
    wxFileName dir(wxFileName::GetTempDir(), wxEmptyString);
    dir.AppendDir("borpa_sim");
    return dir.GetPath().ToUTF8().data();
}

std::string HewSim::served_file(const wxString& job, const wxString& name)
{
    const std::string id = job.ToUTF8().data();
    if (!Borpa::Sim::is_valid_job_id(id) || name != "result.bsr")
        return {};
    wxFileName file(wxString::FromUTF8(jobs_root()), "result.bsr");
    file.AppendDir(job);
    return file.GetFullPath().ToUTF8().data();
}

wxString HewSim::result_url(const std::string& job)
{
#ifdef __WIN32__
    return wxString::FromUTF8("https://hew.wxsite/sim/" + job + "/result.bsr");
#else
    return wxString::FromUTF8("hew://app/sim/" + job + "/result.bsr");
#endif
}

wxString HewSim::capabilities_script() const
{
    const Borpa::Sim::SolverBundle solvers = Borpa::Sim::find_solvers(m_solvers_dir);
    const nlohmann::json caps = { { "mesher", solvers.has_mesher() }, { "structural", solvers.has_structural() } };
    return wxString::FromUTF8("(function(){var h=window.__borpaHew;if(h&&h.setSimCapabilities){h.setSimCapabilities(" +
                              caps.dump() + ");}})();");
}

void HewSim::stop()
{
    if (m_cancel)
        *m_cancel = true;
    if (m_worker.joinable())
        m_worker.join();
    m_cancel.reset();
    m_job.clear();
}

void HewSim::post_event(const std::string& json)
{
    // CallAfter queues the call for the GUI thread and is safe to use from the worker.
    m_owner->CallAfter([alive = m_alive, run = m_run_script, script = wxString::FromUTF8(json)] {
        if (*alive)
            run(script);
    });
}

void HewSim::cancel(const std::string& job)
{
    if (!m_job.empty() && job == m_job) {
        BOOST_LOG_TRIVIAL(info) << "Sim: cancelling job " << job;
        stop();
    }
}

void HewSim::run(const std::string& message)
{
    stop();

    Borpa::Sim::JobRequest request;
    std::string            error;
    if (!Borpa::Sim::parse_job_request(message, request, error)) {
        std::string job;
        try {
            job = nlohmann::json::parse(message).value("job", std::string());
        } catch (const std::exception&) {}
        BOOST_LOG_TRIVIAL(warning) << "Sim: refused a study: " << error;
        const nlohmann::json ev = { { "job", job }, { "state", "failed" }, { "code", "bad-study" }, { "message", error }, { "logTail", "" } };
        post_event(event_script(ev).ToUTF8().data());
        return;
    }

    m_job    = request.job;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    BOOST_LOG_TRIVIAL(info) << "Sim: starting job " << m_job << ", " << request.geometry.triangle_count() << " triangles";

    m_worker = std::thread([this, request = std::move(request), cancel = m_cancel, solvers_dir = m_solvers_dir] {
        const std::string job = request.job;
        auto              send = [this](const nlohmann::json& ev) { post_event(event_script(ev).ToUTF8().data()); };

        Borpa::Sim::JobContext ctx;
        ctx.solvers = Borpa::Sim::find_solvers(solvers_dir);
        ctx.job_dir = Borpa::Sim::make_job_dir(jobs_root(), job);
        ctx.cancel  = cancel.get();
        if (ctx.job_dir.empty()) {
            send({ { "job", job }, { "state", "failed" }, { "code", "no-result" }, { "message", "Could not make the job folder." }, { "logTail", "" } });
            return;
        }

        // At most one progress event per stage step or per quarter second.
        using Clock = std::chrono::steady_clock;
        Borpa::Sim::Stage last_stage = Borpa::Sim::Stage::Read;
        double            last_fraction = -2;
        Clock::time_point last_sent;
        ctx.on_progress = [&](const Borpa::Sim::Progress& p) {
            const auto now = Clock::now();
            if (p.stage == last_stage && p.fraction == last_fraction && now - last_sent < std::chrono::milliseconds(250))
                return;
            last_stage    = p.stage;
            last_fraction = p.fraction;
            last_sent     = now;
            nlohmann::json ev = { { "job", job }, { "state", "running" }, { "stage", Borpa::Sim::stage_name(p.stage) }, { "message", p.message } };
            ev["progress"]    = p.fraction < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.fraction);
            send(ev);
        };

        const Borpa::Sim::JobOutcome out = Borpa::Sim::run_static_job(request, ctx);
        if (out.ok) {
            BOOST_LOG_TRIVIAL(info) << "Sim: job " << job << " done: " << out.summary_json;
            send({ { "job", job }, { "state", "done" }, { "resultUrl", result_url(job).ToUTF8().data() },
                   { "summary", nlohmann::json::parse(out.summary_json) } });
        } else {
            BOOST_LOG_TRIVIAL(warning) << "Sim: job " << job << " failed: " << out.code << ": " << out.message;
            send({ { "job", job }, { "state", "failed" }, { "code", out.code }, { "message", out.message }, { "logTail", out.log_tail } });
        }
    });
}

}} // namespace Slic3r::GUI
