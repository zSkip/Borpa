#pragma once

#include <memory>
#include <string>

#include <wx/event.h>
#include <wx/panel.h>
#include <wx/sharedptr.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include "Lazy.hpp"

class wxWindow;

namespace Slic3r { namespace GUI {

class HewSim;

// The Hew page: the bundled Hew modeler (resources/web/hew) in a web view, shown by two tabs.
// Under Model it edits the model and has a button that sends it to Prepare; under Sim the same
// page and document set up and run simulations, which HewSim runs. The page is told which
// filaments the selected printer can use, so the model is built in them.
// docs/HLSD/hew-tab.md covers the design.
class HewPanel : public wxPanel, public LazyInstance<HewPanel>
{
public:
    explicit HewPanel(wxWindow* parent);
    ~HewPanel() override;

    // Which tab shows the page. Callable before the panel is built; the page is told once it is
    // ready, and again whenever the tab changes.
    static void show_as_sim(bool sim);

    // Serves resources/web/hew under the "hew" scheme. Edge (WebView2) exposes it at the
    // virtual host https://hew.wxsite/, the other backends as hew://.
    static wxSharedPtr<wxWebViewHandler> make_resource_handler();
    static wxString start_url();
    // True when the bundle has been built into resources/web/hew.
    static bool bundle_available();

private:
    void on_script_message(wxWebViewEvent& evt);
    void on_new_window(wxWebViewEvent& evt);
    void on_plate(const std::string& name, const std::string& data);
    void show_error(const std::string& code);
    // Sends the page the filament list when it differs from the last one sent, or always when
    // `force` is set. Nothing is sent before the page reports it is ready.
    void send_filaments(bool force);
    void apply_mode();

    inline static bool s_show_as_sim{ false };

    wxWebView*              m_browser{ nullptr };
    wxWindow*               m_send_button{ nullptr };
    std::unique_ptr<HewSim> m_sim;
    wxTimer     m_preset_timer;
    bool        m_page_ready{ false };
    std::string m_sent_filaments;
};

}} // namespace Slic3r::GUI
