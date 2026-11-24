#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <wx/string.h>

class wxEvtHandler;

namespace Slic3r { namespace GUI {

// Runs the Sim tab's studies for the Hew page: one job at a time on a worker thread, through the
// borpa_sim pipeline (mesher and solver as child processes). Events go back to the page as
// scripts calling window.__borpaHew.simEvent; docs/HLSD/hew-tab.md lists them.
class HewSim
{
public:
    // `run_script` runs a script in the page; it is only called on the GUI thread, through
    // `owner`'s CallAfter, so it must stay valid as long as `owner` does.
    HewSim(wxEvtHandler* owner, std::function<void(const wxString&)> run_script);
    // Cancels a running job and waits for it to stop.
    ~HewSim();

    // A `sim.run` message from the page. A job already running is cancelled first.
    void run(const std::string& message);
    // A `sim.cancel` message: cancels the job if it is the one running.
    void cancel(const std::string& job);

    // The script that tells the page which solvers are installed.
    wxString capabilities_script() const;

    // Where job folders live: <temp>/borpa_sim.
    static std::string jobs_root();
    // The file served at sim/<job>/<name>, or an empty string for anything not served.
    static std::string served_file(const wxString& job, const wxString& name);
    // The URL the page loads a job's result from.
    static wxString result_url(const std::string& job);

private:
    void stop();
    void post_event(const std::string& json);

    wxEvtHandler*                         m_owner;
    std::function<void(const wxString&)>  m_run_script;
    std::string                           m_solvers_dir;
    std::thread                           m_worker;
    std::string                           m_job;
    std::shared_ptr<std::atomic<bool>>    m_cancel;
    // Cleared by the destructor so events still queued for the GUI thread are dropped.
    std::shared_ptr<std::atomic<bool>>    m_alive;
};

}} // namespace Slic3r::GUI
