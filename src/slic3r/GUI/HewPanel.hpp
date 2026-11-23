#pragma once

#include <string>

#include <wx/event.h>
#include <wx/panel.h>
#include <wx/sharedptr.h>
#include <wx/string.h>
#include <wx/webview.h>

#include "Lazy.hpp"

class wxWindow;

namespace Slic3r { namespace GUI {

// The Hew tab: the bundled Hew modeler (resources/web/hew) in a web view, and a button that
// sends the open model to Prepare. docs/HLSD/hew-tab.md covers the design.
class HewPanel : public wxPanel, public LazyInstance<HewPanel>
{
public:
    explicit HewPanel(wxWindow* parent);
    ~HewPanel() override;

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

    wxWebView* m_browser{ nullptr };
};

}} // namespace Slic3r::GUI
