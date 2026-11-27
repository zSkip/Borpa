#include "HewPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "HewPlate.hpp"
#include "HewSim.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/ProjectStrategy.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <nlohmann/json.hpp>

#include <wx/arrstr.h>
#include <wx/base64.h>
#include <wx/buffer.h>
#include <wx/colour.h>
#include <wx/datetime.h>
#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/uri.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/wxcrt.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <ios>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

// Asks the page for its plate; a page whose bridge is not installed yet answers "not-ready".
const char* const SEND_TO_PLATE_SCRIPT =
    "(function(){if(window.__borpaHew&&window.__borpaHew.sendToPlate){window.__borpaHew.sendToPlate();}"
    "else{window.wx.postMessage(JSON.stringify({borpa:1,type:'error',code:'not-ready',message:''}));}})();";

boost::filesystem::path bundle_dir() { return boost::filesystem::path(resources_dir()) / "web" / "hew"; }

// How often the panel checks, while it is shown, whether the filament list changed.
constexpr int PRESET_CHECK_MS = 1000;

const std::string* first_string(const DynamicPrintConfig& config, const char* key)
{
    const auto* opt = config.option<ConfigOptionStrings>(key);
    return opt != nullptr && !opt->values.empty() ? &opt->values.front() : nullptr;
}

// Objects closer than this, in mm, are parts of one model on the plate. Hew writes touching
// faces at the same coordinates, so this only absorbs float rounding.
constexpr double TOUCH_TOLERANCE_MM = 0.01;

// How many filaments the selected printer prints at once: one per extruder, or any number with
// a single-extruder multi-material unit (an AMS, an MMU).
size_t printer_holds(const PresetBundle& bundle)
{
    const DynamicPrintConfig& config   = bundle.printers.get_edited_preset().config;
    const auto*               nozzles  = config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto*               multi    = config.option<ConfigOptionBool>("single_extruder_multi_material");
    const size_t              extruders = nozzles != nullptr && !nozzles->values.empty() ? nozzles->values.size() : 1;
    return multi != nullptr && multi->value ? MAXIMUM_EXTRUDER_NUMBER : extruders;
}

wxString join_names(const std::vector<std::string>& names)
{
    wxString out;
    for (const std::string& name : names)
        out += (out.empty() ? "" : ", ") + wxString::FromUTF8(name);
    return out;
}

// The filaments the selected printer can use, as the page takes them (borpa/filaments.ts in Hew):
// every filament preset the filament combo boxes list (visible and compatible with the printer),
// and every preset in a project slot, with the slot it occupies. A preset in a slot shows the
// slot's colour.
nlohmann::json filament_list()
{
    nlohmann::json      list = nlohmann::json::array();
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return list;

    const std::vector<std::string>& slots = bundle->filament_presets;
    const auto* slot_colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    for (const Preset& preset : bundle->filaments.get_presets()) {
        const auto slot = std::find(slots.begin(), slots.end(), preset.name);
        if (slot == slots.end() && (!preset.is_visible || !preset.is_compatible || preset.is_default))
            continue;
        const DynamicPrintConfig& config = preset.config;
        nlohmann::json            entry  = {{"name", preset.name}};
        const std::string*        type   = first_string(config, "filament_type");
        const std::string*        vendor = first_string(config, "filament_vendor");
        const std::string*        colour = first_string(config, "filament_colour");
        entry["type"]   = type != nullptr ? *type : std::string();
        entry["vendor"] = vendor != nullptr ? *vendor : std::string();
        entry["colour"] = colour != nullptr ? *colour : std::string();
        const auto* density = config.option<ConfigOptionFloats>("filament_density");
        if (density != nullptr && !density->values.empty() && density->values.front() > 0.)
            entry["density"] = density->values.front();
        else
            entry["density"] = nullptr;
        const auto* softening = config.option<ConfigOptionInts>("temperature_vitrification");
        if (softening != nullptr && !softening->values.empty())
            entry["softeningTemp"] = softening->values.front();
        else
            entry["softeningTemp"] = nullptr;
        entry["slot"] = nullptr;
        if (slot != slots.end()) {
            const size_t i = static_cast<size_t>(slot - slots.begin());
            entry["slot"]  = i + 1;
            if (slot_colours != nullptr && i < slot_colours->values.size() && !slot_colours->values[i].empty())
                entry["colour"] = slot_colours->values[i];
        }
        list.push_back(std::move(entry));
    }
    return list;
}

// The page compiles its kernel with WebAssembly.compileStreaming, which requires application/wasm.
wxString mime_type(const wxString& extension)
{
    static const std::pair<const char*, const char*> types[] = {
        {"html", "text/html"},
        {"js", "text/javascript"},
        {"mjs", "text/javascript"},
        {"css", "text/css"},
        {"wasm", "application/wasm"},
        {"json", "application/json"},
        {"map", "application/json"},
        {"webmanifest", "application/manifest+json"},
        {"svg", "image/svg+xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"webp", "image/webp"},
        {"gif", "image/gif"},
        {"ico", "image/x-icon"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},
        {"otf", "font/otf"},
        {"txt", "text/plain"},
        {"hew", "application/octet-stream"},
        {"bsr", "application/octet-stream"},
    };
    const wxString ext = extension.Lower();
    for (const auto& [name, type] : types)
        if (ext == name)
            return type;
    return "application/octet-stream";
}

// Serves files from <resources>/web/hew and nothing outside it.
class HewResourceHandler : public wxWebViewHandler
{
public:
    HewResourceHandler() : wxWebViewHandler("hew") {}

    wxFSFile* GetFile(const wxString& uri) override
    {
        wxString path = wxURI::Unescape(wxURI(uri).GetPath());
        if (path.StartsWith("/"))
            path.Remove(0, 1);
        if (path.empty())
            path = "index.html";
        if (path.StartsWith("/") || path.Contains(":") || path.Contains("\\"))
            return nullptr;
        const wxArrayString segments = wxSplit(path, '/');
        for (const wxString& segment : segments)
            if (segment == "..")
                return nullptr;

        // sim/<job>/<file>: a simulation result from the job folders, not the bundle.
        boost::filesystem::path full;
        if (segments.size() == 3 && segments[0] == "sim") {
            const std::string served = HewSim::served_file(segments[1], segments[2]);
            if (served.empty())
                return nullptr;
            full = into_path(wxString::FromUTF8(served));
        } else
            full = bundle_dir() / into_path(path);
        boost::system::error_code     ec;
        if (!boost::filesystem::is_regular_file(full, ec))
            return nullptr;
        const wxString full_path = from_path(full);
        auto*          stream    = new wxFFileInputStream(full_path);
        if (!stream->IsOk()) {
            delete stream;
            return nullptr;
        }
        return new wxFSFile(stream, uri, mime_type(wxFileName(full_path).GetExt()), wxEmptyString, wxDateTime::Now());
    }
};

// A file stem made of letters, digits, spaces, '-' and '_' only.
wxString file_stem(const std::string& name)
{
    wxString stem;
    for (const wxUniChar c : wxString::FromUTF8(name))
        if (wxIsalnum(c) || c == ' ' || c == '-' || c == '_')
            stem += c;
    stem.Trim(true).Trim(false);
    // Well inside the file name length limits of every platform.
    if (stem.length() > 100)
        stem.Truncate(100);
    return stem.empty() ? wxString("Hew model") : stem;
}

} // namespace

wxSharedPtr<wxWebViewHandler> HewPanel::make_resource_handler() { return wxSharedPtr<wxWebViewHandler>(new HewResourceHandler); }

wxString HewPanel::start_url()
{
#ifdef __WIN32__
    // Edge serves a registered handler at https://<scheme>.wxsite/, a secure context, which the
    // page needs for WebAssembly and its workers.
    return "https://hew.wxsite/index.html";
#else
    return "hew://app/index.html";
#endif
}

bool HewPanel::bundle_available()
{
    boost::system::error_code ec;
    return boost::filesystem::is_regular_file(bundle_dir() / "index.html", ec);
}

HewPanel::HewPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    if (!bundle_available()) {
        auto* text = new wxStaticText(this, wxID_ANY,
                                      _L("The Hew modeler is not included in this build. Run scripts/build_hew.mjs and restart."),
                                      wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        sizer->AddStretchSpacer(1);
        sizer->Add(text, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(20));
        sizer->AddStretchSpacer(1);
        SetSizer(sizer);
        return;
    }

    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    auto* send    = new ::Button(this, _L("Send to Prepare"));
    send->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser != nullptr)
            WebView::RunScript(m_browser, SEND_TO_PLATE_SCRIPT);
    });
    toolbar->AddStretchSpacer(1);
    toolbar->Add(send, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    sizer->Add(toolbar, 0, wxEXPAND);
    m_send_button = send;

    m_browser = WebView::CreateWebView(this, "");
#ifdef __WIN32__
    // Edge applies handlers registered before its asynchronous creation completes. The other
    // backends need them before Create(), so WebView::CreateWebView() registers this one there.
    m_browser->RegisterHandler(make_resource_handler());
#endif
    const nlohmann::json host = {{"version", SoftFever_VERSION}, {"dark", wxGetApp().dark_mode()}};
    m_browser->AddUserScript(wxString::FromUTF8("window.__BORPA_HOST__ = " + host.dump() + ";"), wxWEBVIEW_INJECT_AT_DOCUMENT_START);

    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &HewPanel::on_script_message, this);
    // The page follows the printer and filament presets. They change in other tabs and dialogs,
    // so the panel compares the list while it is shown instead of hooking every place that can
    // change a preset.
    m_preset_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (IsShownOnScreen())
            send_filaments(false);
    });
    m_preset_timer.Start(PRESET_CHECK_MS);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &HewPanel::on_new_window, this);
    // Handled so WebView::RecreateAll() (a theme change) does not reload the page, which would
    // discard the model being edited.
    m_browser->Bind(EVT_WEBVIEW_RECREATED, [](wxCommandEvent&) {});

    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    m_sim = std::make_unique<HewSim>(this, [this](const wxString& script) {
        if (m_browser != nullptr)
            WebView::RunScript(m_browser, script);
    });
    apply_mode();

    m_browser->LoadURL(start_url());
}

HewPanel::~HewPanel()
{
    m_preset_timer.Stop();
    // Stops a running study, and its solver, before the page goes.
    m_sim.reset();
    SetEvtHandlerEnabled(false);
    if (m_browser != nullptr) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

void HewPanel::on_new_window(wxWebViewEvent& evt)
{
    const wxString url = evt.GetURL();
    if (!url.empty())
        wxLaunchDefaultBrowser(url);
    evt.Veto();
}

void HewPanel::on_script_message(wxWebViewEvent& evt)
{
    try {
        nlohmann::json msg = nlohmann::json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
        if (!msg.is_object())
            return;
        const auto borpa = msg.find("borpa");
        if (borpa == msg.end() || *borpa != 1)
            return;

        const std::string type = msg.value("type", std::string());
        // Dialogs and model loading run after the web view's message callback has returned.
        if (type == "ready") {
            BOOST_LOG_TRIVIAL(info) << "Hew: modeler ready";
            // A reloaded page has lost the list it had.
            CallAfter([this] {
                m_page_ready = true;
                send_filaments(true);
                apply_mode();
                if (m_sim)
                    WebView::RunScript(m_browser, m_sim->capabilities_script());
            });
        } else if (type == "sim.run") {
            // The whole message: HewSim reads the study and the geometry from it.
            CallAfter([this, text = std::string(evt.GetString().ToUTF8().data())] {
                if (m_sim)
                    m_sim->run(text);
            });
        } else if (type == "sim.cancel") {
            CallAfter([this, job = msg.value("job", std::string())] {
                if (m_sim)
                    m_sim->cancel(job);
            });
        } else if (type == "plate") {
            BOOST_LOG_TRIVIAL(info) << "Hew: plate received, non-solid objects in it: " << msg.value("nonSolid", 0);
            std::map<std::string, std::string> filaments;
            if (const auto f = msg.find("filaments"); f != msg.end() && f->is_object())
                for (const auto& [material, preset] : f->items())
                    if (preset.is_string())
                        filaments[material] = preset.get<std::string>();
            CallAfter([this, name = msg.value("name", std::string()), data = msg.value("data", std::string()), filaments = std::move(filaments)] {
                on_plate(name, data, filaments);
            });
        } else if (type == "error") {
            BOOST_LOG_TRIVIAL(warning) << "Hew: " << msg.value("code", std::string()) << " " << msg.value("message", std::string());
            CallAfter([this, code = msg.value("code", std::string())] { show_error(code); });
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Hew: bad message from the page: " << e.what();
    }
}

void HewPanel::on_plate(const std::string& name, const std::string& data, const std::map<std::string, std::string>& filaments)
{
    try {
        const wxMemoryBuffer bytes = wxBase64Decode(data.data(), data.size());
        if (bytes.IsEmpty()) {
            show_error("export-failed");
            return;
        }

        const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "borpa_hew";
        boost::filesystem::create_directories(dir);
        const boost::filesystem::path path = dir / into_path(file_stem(name) + ".3mf");
        {
            boost::filesystem::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char*>(bytes.GetData()), static_cast<std::streamsize>(bytes.GetDataLen()));
            if (!out)
                throw std::runtime_error("cannot write " + path.string());
        }

        // Read here rather than through load_files: the objects are regrouped and given their
        // filaments first, and load_files would ask whether a stacked model is one object.
        DynamicPrintConfig        file_config;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        PlateDataPtrs             plate_data;
        std::vector<Preset*>      file_presets;
        Model model = Model::read_from_file(path.string(), &file_config, &substitutions,
                                            LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances, &plate_data, &file_presets);
        release_PlateData_list(plate_data);
        for (Preset* preset : file_presets)
            delete preset;

        // Each object's filament preset, by the name of its material in the file.
        const std::vector<std::string> materials = HewPlate::item_material_names(path.string());
        if (materials.size() != model.objects.size())
            BOOST_LOG_TRIVIAL(warning) << "Hew: the plate has " << model.objects.size() << " objects and " << materials.size()
                                       << " material names; every object prints in filament 1";
        std::vector<std::string> preset_of(model.objects.size());
        std::vector<std::string> needed;
        for (size_t i = 0; i < preset_of.size() && materials.size() == model.objects.size(); ++i)
            if (const auto it = filaments.find(materials[i]); it != filaments.end() && !it->second.empty()) {
                preset_of[i] = it->second;
                if (std::find(needed.begin(), needed.end(), it->second) == needed.end())
                    needed.push_back(it->second);
            }

        PresetBundle*     bundle = wxGetApp().preset_bundle;
        std::vector<bool> known;
        for (const std::string& preset : needed)
            known.push_back(bundle->filaments.find_preset(preset, false) != nullptr);
        const HewPlate::SlotPlan plan = HewPlate::plan_slots(needed, bundle->filament_presets, bundle->num_physical_filaments(),
                                                             MAXIMUM_EXTRUDER_NUMBER, printer_holds(*bundle), known);
        auto log_slots = [bundle](const char* when) {
            std::string list;
            for (const std::string& preset : bundle->filament_presets)
                list += "[" + preset + "]";
            BOOST_LOG_TRIVIAL(info) << "Hew: filament slots " << when << " (" << bundle->num_physical_filaments() << " physical): " << list;
        };
        log_slots("before");
        for (const std::string& preset : plan.added) {
            const Preset*      found  = bundle->filaments.find_preset(preset, false);
            const std::string* colour = found != nullptr ? first_string(found->config, "filament_colour") : nullptr;
            wxGetApp().sidebar().add_custom_filament(wxColour(colour != nullptr ? *colour : std::string("#FFFFFF")), preset);
        }
        // add_custom_filament names the slot's preset after its combo box was built.
        if (!plan.added.empty())
            wxGetApp().sidebar().update_presets(Preset::TYPE_FILAMENT);
        log_slots("after");
        // The slots as they now are; a slot that could not be added prints in filament 1.
        auto slot_of = [bundle](const std::string& preset) {
            const auto&  slots    = bundle->filament_presets;
            const size_t physical = std::min(bundle->num_physical_filaments(), slots.size());
            const auto   it       = std::find(slots.begin(), slots.begin() + physical, preset);
            return it == slots.begin() + physical ? 1 : int(it - slots.begin()) + 1;
        };

        // Objects that touch become the parts of one object; each part prints in its filament.
        Model plate;
        for (const std::vector<size_t>& group : HewPlate::touching_groups(model, TOUCH_TOLERANCE_MM)) {
            // A lone object prints in its object's extruder; the parts of a merged object keep
            // their own, which convert_multipart_object carries over.
            Model parts;
            for (size_t i : group) {
                ModelObject* object = parts.add_object(*model.objects[i]);
                if (preset_of[i].empty())
                    continue;
                if (group.size() == 1)
                    object->config.set("extruder", slot_of(preset_of[i]));
                else
                    for (ModelVolume* volume : object->volumes)
                        volume->config.set("extruder", slot_of(preset_of[i]));
            }
            if (parts.objects.size() > 1) {
                parts.convert_multipart_object(unsigned(bundle->filament_presets.size()), true);
                ModelObject* merged = parts.objects.front();
                merged->add_instance();
                merged->center_around_origin();
            }
            const ModelObject* added = plate.add_object(*parts.objects.front());
            std::string        slots;
            for (const ModelVolume* volume : added->volumes)
                slots += " " + std::to_string(volume->config.has("extruder") ? volume->config.extruder() : added->config.extruder());
            BOOST_LOG_TRIVIAL(info) << "Hew: plate object \"" << added->name << "\", " << added->volumes.size() << " part(s), filaments" << slots;
        }

        const std::vector<size_t> loaded = wxGetApp().plater()->add_hew_objects(plate);
        if (!loaded.empty() && wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);

        wxString warning;
        if (!plan.unknown.empty())
            // TRN %1% is a list of filament preset names.
            warning += format_wxstr(_L("These filaments are not in the filament list, so their parts print with filament 1: %1%"),
                                    join_names(plan.unknown)) + "\n\n";
        if (!plan.overflow.empty())
            // TRN %1% is a list of filament preset names.
            warning += format_wxstr(_L("The selected printer cannot print all the filaments of this model at once. These "
                                       "filaments did not fit and need another printer or a filament change by hand: %1%"),
                                    join_names(plan.overflow));
        if (!warning.empty())
            MessageDialog(this, warning.Trim(), _L("Hew"), wxOK | wxICON_WARNING).ShowModal();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Hew: sending the model to Prepare failed: " << e.what();
        // TRN %1% is the reason given by the system, such as a file that could not be written.
        MessageDialog(this, format_wxstr(_L("The Hew model could not be added to the plate: %1%"), e.what()), _L("Hew"),
                      wxOK | wxICON_ERROR)
            .ShowModal();
    }
}

void HewPanel::send_filaments(bool force)
{
    if (m_browser == nullptr || !m_page_ready)
        return;
    // ASCII-only JSON: preset names can hold any character, and the script must stay valid.
    std::string list = filament_list().dump(-1, ' ', true);
    if (!force && list == m_sent_filaments)
        return;
    BOOST_LOG_TRIVIAL(info) << "Hew: sending the filament list";
    WebView::RunScript(m_browser, wxString::FromUTF8("(function(){var h=window.__borpaHew;if(h&&h.setFilaments){h.setFilaments(" + list + ");}})();"));
    m_sent_filaments = std::move(list);
}

void HewPanel::show_as_sim(bool sim)
{
    s_show_as_sim = sim;
    if (HewPanel* panel = if_built())
        panel->apply_mode();
}

void HewPanel::apply_mode()
{
    // Sending to Prepare belongs to the Model tab.
    if (m_send_button != nullptr && m_send_button->IsShown() == s_show_as_sim) {
        m_send_button->Show(!s_show_as_sim);
        Layout();
    }
    if (m_browser != nullptr && m_page_ready)
        WebView::RunScript(m_browser, wxString::Format("(function(){var h=window.__borpaHew;if(h&&h.setMode){h.setMode('%s');}})();",
                                                       s_show_as_sim ? "sim" : "model"));
}

void HewPanel::show_error(const std::string& code)
{
    wxString message;
    if (code == "empty")
        message = _L("There is nothing to send: the Hew model has no solid objects.");
    else if (code == "not-ready")
        message = _L("The Hew modeler is still loading. Try again in a moment.");
    else
        message = _L("Hew could not export the model.");
    MessageDialog(this, message, _L("Hew"), wxOK | wxICON_WARNING).ShowModal();
}

}} // namespace Slic3r::GUI
