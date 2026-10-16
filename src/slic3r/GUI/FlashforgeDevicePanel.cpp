#include "FlashforgeDevicePanel.hpp"
#include "FlashforgeIfsAutoSync.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Widgets/WebView.hpp"
#include "CrealityCfsUi.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/FlashforgePrinterAgent.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>

#include <iomanip>
#include <regex>
#include <set>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

struct FlashforgeDevicePanel::Bridge
{
    FlashforgeDevicePanel* panel{nullptr};
};

namespace {

std::string url_encode(const std::string& value)
{
    std::ostringstream escaped;
    escaped << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            escaped << c;
        else
            escaped << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return escaped.str();
}

std::string json_string(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// "http://10.1.2.3:7125/x" or "10.1.2.3:7125" -> host and port (7125 when none).
void split_host(const std::string& value, std::string& host, int& port)
{
    std::size_t       start = value.find("://");
    start                   = start == std::string::npos ? 0 : start + 3;
    const std::size_t end   = value.find_first_of("/?#", start);
    std::string       authority = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);
    port = 7125;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        try {
            port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            port = 7125;
        }
        authority.erase(colon);
    }
    host = authority;
}

struct FfPrinter
{
    std::string name, host, model;
    int         port{7125};
};

bool uses_flashforge_agent(const DynamicPrintConfig& config)
{
    return config.has("printer_agent") && boost::iequals(config.opt_string("printer_agent"), "flashforge");
}

// Printer presets with the flashforge agent and a print host, one per host (the selected printer first).
std::vector<FfPrinter> flashforge_printers(PresetBundle& bundle, std::string* current_host)
{
    std::vector<FfPrinter> out;
    std::set<std::string>  seen;
    auto add = [&](const std::string& name, const DynamicPrintConfig& config, const std::string& host_value) {
        FfPrinter p;
        split_host(host_value, p.host, p.port);
        if (p.host.empty() || !seen.insert(p.host).second)
            return;
        p.name  = name;
        p.model = config.has("printer_model") ? config.opt_string("printer_model") : std::string();
        out.push_back(std::move(p));
    };
    const Preset& edited = bundle.printers.get_edited_preset();
    std::string   host_value;
    if (bundle.physical_printers.has_selection()) {
        if (const DynamicPrintConfig* c = bundle.physical_printers.get_selected_printer_config(); c != nullptr && c->has("print_host"))
            host_value = c->opt_string("print_host");
    }
    if (host_value.empty() && edited.config.has("print_host"))
        host_value = edited.config.opt_string("print_host");
    if (uses_flashforge_agent(edited.config) && !host_value.empty()) {
        add(bundle.physical_printers.has_selection() ? bundle.physical_printers.get_selected_printer_name() : edited.name, edited.config, host_value);
        if (current_host != nullptr && !out.empty())
            *current_host = out.front().host;
    }
    for (const Preset& preset : bundle.printers) {
        if (preset.is_default || !preset.is_visible || !uses_flashforge_agent(preset.config) || !preset.config.has("print_host"))
            continue;
        add(preset.name, preset.config, preset.config.opt_string("print_host"));
    }
    return out;
}

// What the page may ask Moonraker: reads of printer state, files, history, webcams and the timelapse
// component; G-code scripts and print control; deleting a file or timelapse video.
bool allowed_request(const std::string& method, const std::string& path)
{
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos || path.find_first_of("\r\n ") != std::string::npos)
        return false;
    static const std::regex reads(
        R"(^/(printer/(info|objects/(list|query)).*|server/(info|files/(list|metadata|directory|roots)|history/(list|totals)|webcams/list|database/item|temperature_store)(\?.*)?|machine/timelapse/.*)$)");
    static const std::regex posts(R"(^/printer/(gcode/script|print/(start|pause|resume|cancel))(\?.*)?$)");
    static const std::regex dels(R"(^/server/files/(gcodes|timelapse)/[^?]+$)");
    if (method == "GET")
        return std::regex_search(path, reads);
    if (method == "POST")
        return std::regex_search(path, posts);
    if (method == "DELETE")
        return std::regex_search(path, dels);
    return false;
}

} // namespace

bool FlashforgeDevicePanel::is_flashforge_printer(const PresetBundle& bundle)
{
    const Preset& preset = bundle.printers.get_edited_preset();
    if (!uses_flashforge_agent(preset.config))
        return false;
    const std::string    model  = preset.config.has("printer_model") ? preset.config.opt_string("printer_model") : std::string();
    const Preset*        parent = bundle.printers.get_preset_parent(preset);
    const VendorProfile* vendor = preset.vendor != nullptr ? preset.vendor : parent != nullptr ? parent->vendor : nullptr;
    return boost::istarts_with(model, "Flashforge") || (vendor != nullptr && boost::iequals(vendor->id, "Flashforge"));
}

FlashforgeDevicePanel::FlashforgeDevicePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_bridge(std::make_shared<Bridge>())
{
    m_bridge->panel = this;
    SetSizer(new wxBoxSizer(wxVERTICAL));
    create_browser();
    register_routes();
}

FlashforgeDevicePanel::~FlashforgeDevicePanel()
{
    m_bridge->panel = nullptr;
    SetEvtHandlerEnabled(false);
    if (m_browser) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

void FlashforgeDevicePanel::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");
    if (m_browser == nullptr)
        return;
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &FlashforgeDevicePanel::on_script_message, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& evt) { evt.Veto(); });
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
}

void FlashforgeDevicePanel::load_page()
{
    if (m_browser == nullptr || m_page_requested)
        return;
    m_page_requested = true;
    const fs::path page = fs::path(resources_dir()) / "web" / "zoukeiDevice" / "index.html";
    wxString       url  = file_url_from_path(page);
    url += wxString::Format("?backend=moonraker&dark=%d", wxGetApp().dark_mode() ? 1 : 0);
    m_browser->LoadURL(url);
}

bool FlashforgeDevicePanel::Show(bool show)
{
    const bool loaded = m_page_requested;
    if (show)
        load_page();
    const bool result = wxPanel::Show(show);
    if (loaded)
        post_command("panel_visible", show);
    return result;
}

void FlashforgeDevicePanel::on_sys_color_changed()
{
    post_command("is_dark_theme", wxGetApp().dark_mode());
}

void FlashforgeDevicePanel::run_script(const std::string& script)
{
    if (m_browser == nullptr || m_browser->IsBeingDeleted())
        return;
    WebView::RunScript(m_browser, wxString::FromUTF8(script));
}

void FlashforgeDevicePanel::post_command(const std::string& command, json data)
{
    json message = {{"command", command}, {"data", std::move(data)}};
    run_script("window.handleStudioCmd('" + url_encode(message.dump(-1, ' ', true, json::error_handler_t::replace)) + "');");
}

void FlashforgeDevicePanel::post_command_async(const std::string& command, json data)
{
    if (wxTheApp == nullptr)
        return;
    std::weak_ptr<Bridge> weak = m_bridge;
    wxTheApp->CallAfter([weak, command, data = std::move(data)]() mutable {
        if (auto bridge = weak.lock(); bridge && bridge->panel)
            bridge->panel->post_command(command, std::move(data));
    });
}

void FlashforgeDevicePanel::on_script_message(wxWebViewEvent& evt)
{
    json j = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
    if (!j.is_object())
        return;
    auto it = m_handlers.find(json_string(j, "command"));
    if (it == m_handlers.end())
        return;
    try {
        it->second(j);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Flashforge panel: command failed: " << e.what();
    }
}

void FlashforgeDevicePanel::send_device_list()
{
    json          list = json::array();
    std::string   current;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr)
        for (const FfPrinter& p : flashforge_printers(*bundle, &current)) {
            const fs::path cover = fs::path(resources_dir()) / "profiles" / "Flashforge" / (p.model + "_cover.png");
            list.push_back({{"address", p.host},
                            {"port", p.port},
                            {"mac", p.host},
                            {"name", p.name},
                            {"model", p.model},
                            {"modelName", boost::erase_first_copy(p.model, "Flashforge ")},
                            {"backend", "moonraker"},
                            {"cover", !p.model.empty() && fs::exists(cover) ? file_url_from_path(cover).ToStdString() : std::string()}});
        }
    post_command("device_list", {{"devices", std::move(list)}, {"current", current}});
}

// One Moonraker request for the page; answered as {requestId, ok, status, body}.
void FlashforgeDevicePanel::route_moonraker_http(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    const std::string address    = json_string(j, "address");
    const std::string method     = json_string(j, "method").empty() ? "GET" : json_string(j, "method");
    const std::string path       = json_string(j, "path");
    const std::string body       = json_string(j, "body");
    auto              reply      = [this, request_id](json out) {
        out["requestId"] = request_id;
        post_command_async("moonraker_http", std::move(out));
    };
    int           port   = 0;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr)
        for (const FfPrinter& p : flashforge_printers(*bundle, nullptr))
            if (p.host == address)
                port = p.port;
    if (request_id.empty() || port == 0 || !allowed_request(method, path)) {
        reply({{"ok", false}, {"status", 0}, {"error", "request not allowed"}});
        return;
    }
    const std::string url      = "http://" + address + ":" + std::to_string(port) + path;
    auto              complete = [reply](std::string answer, unsigned status) {
        reply({{"ok", status >= 200 && status < 300}, {"status", status}, {"body", std::move(answer)}});
    };
    auto failed = [reply](std::string answer, std::string error, unsigned status) {
        reply({{"ok", false}, {"status", status}, {"body", std::move(answer)}, {"error", std::move(error)}});
    };
    try {
        if (method == "POST")
            Http::post(url).timeout_connect(5).timeout_max(120).header("Content-Type", "application/json").set_post_body(body).on_complete(complete).on_error(failed).perform();
        else if (method == "DELETE")
            Http::del(url).timeout_connect(5).timeout_max(30).on_complete(complete).on_error(failed).perform();
        else
            Http::get(url).timeout_connect(5).timeout_max(20).size_limit(8u * 1024 * 1024).on_complete(complete).on_error(failed).perform();
    } catch (const std::exception& e) {
        reply({{"ok", false}, {"status", 0}, {"error", e.what()}});
    }
}

// The Orca filament presets of the selected printer, for the IFS slot editor (same data as the Creality panel).
void FlashforgeDevicePanel::route_get_filament_presets(const json& j)
{
    CrealityCfsAgent::refresh_presets();
    json presets = json::array();
    for (const auto& p : CrealityCfsAgent::presets()) {
        if (!p.is_visible || !p.is_compatible)
            continue;
        presets.push_back({{"name", p.name},
                           {"vendor", p.vendor},
                           {"type", p.type},
                           {"filament_id", p.filament_id},
                           {"color", p.color},
                           {"minTemp", p.min_temp},
                           {"maxTemp", p.max_temp},
                           {"pressure", p.pressure_advance},
                           {"system", p.is_system}});
    }
    post_command("get_filament_presets", {{"requestId", json_string(j, "requestId")}, {"presets", std::move(presets)}});
}

// Writes one IFS slot through the agent's own path (CHANGE_ZCOLOR, plus lane_data with HelixScreen).
// Answered as {requestId, ok, reason}; nothing is sent while the agent reports a block (printing, ...).
void FlashforgeDevicePanel::route_ifs_write_slot(const json& j)
{
    json out = {{"requestId", json_string(j, "requestId")}, {"ok", false}, {"reason", ""}};
    const auto agent = CfsUi::ifs_agent();
    const int  slot  = j.contains("slot") && j["slot"].is_number_integer() ? j["slot"].get<int>() : 0;
    std::string hex  = json_string(j, "hex");
    if (!hex.empty() && hex[0] == '#')
        hex.erase(0, 1);
    static const std::regex hex_re("^[0-9A-Fa-f]{6}$");
    if (!agent)
        out["reason"] = "no Flashforge printer selected";
    else if (slot < 1 || slot > 4 || (!hex.empty() && !std::regex_match(hex, hex_re)))
        out["reason"] = "invalid slot or colour";
    else if (const std::string reason = agent->write_block_reason(); !reason.empty())
        out["reason"] = reason;
    else {
        FlashforgePrinterAgent::SlotWrite w;
        w.port        = slot;
        w.type        = json_string(j, "type");
        w.name        = json_string(j, "name");
        w.vendor      = json_string(j, "vendor");
        w.color       = hex.empty() ? std::string() : "#" + boost::to_upper_copy(hex); // empty -> the agent writes 161616
        w.nozzle_temp = j.contains("nozzle_temp") && j["nozzle_temp"].is_number() ? j["nozzle_temp"].get<int>() : 0;
        w.bed_temp    = j.contains("bed_temp") && j["bed_temp"].is_number() ? j["bed_temp"].get<int>() : 0;
        if (boost::iequals(w.type, "SILK")) { // ZMOD's SILK is a PLA whose name says silk
            w.type = "PLA";
            if (!boost::icontains(w.name, "silk"))
                w.name = w.name.empty() ? "Silk PLA" : "Silk " + w.name;
        }
        agent->write_slots({w});
        out["ok"] = true;
        // Printer -> project: the filaments mapped to this slot follow what was just written (no echo back).
        const std::string preset = json_string(j, "preset"), wtype = w.type, wcolor = w.color;
        const int         wport  = w.port;
        wxGetApp().CallAfter([wport, wtype, wcolor, preset] { FlashforgeIfsAutoSync::apply_slot_to_project(wport, wtype, wcolor, preset); });
    }
    post_command("ifs_write_slot", std::move(out));
}

void FlashforgeDevicePanel::route_ifs_repair(const json& j)
{
    json       out   = {{"requestId", json_string(j, "requestId")}, {"ok", false}, {"reason", ""}};
    const auto agent = CfsUi::ifs_agent();
    if (!agent)
        out["reason"] = "no Flashforge printer selected";
    else if (const std::string reason = agent->write_block_reason(); !reason.empty())
        out["reason"] = reason;
    else {
        agent->repair_lane_data();
        out["ok"] = true;
    }
    post_command("ifs_repair", std::move(out));
}

// "Sincronizar tudo agora": lane_data follows the firmware, the active Spoolman spool is re-evaluated, the project follows the slots.
void FlashforgeDevicePanel::route_ifs_sync_all(const json& j)
{
    json       out   = {{"requestId", json_string(j, "requestId")}, {"ok", false}, {"reason", ""}, {"changed", ""}};
    const auto agent = CfsUi::ifs_agent();
    if (!agent)
        out["reason"] = "no Flashforge printer selected";
    else {
        if (agent->write_block_reason().empty())
            agent->repair_lane_data();
        agent->resync_active_spool();
        const std::string changed = FlashforgeIfsAutoSync::apply_all_to_project();
        out["ok"]      = true;
        out["changed"] = changed;
        CrealityCfsAgent::notify(changed.empty() ? "Flashforge IFS: projeto j\u00e1 coincide com a impressora." :
                                                   "Flashforge IFS: projeto atualizado - " + changed);
    }
    post_command("ifs_sync_all", std::move(out));
}

void FlashforgeDevicePanel::register_routes()
{
    auto& h = m_handlers;
    h["is_dark_theme"]  = [this](const json&) { post_command("is_dark_theme", wxGetApp().dark_mode()); };
    h["get_lang"]       = [this](const json&) { post_command("get_lang", wxGetApp().current_language_code().ToStdString()); };
    h["device_list"]    = [this](const json&) { send_device_list(); };
    h["moonraker_http"] = [this](const json& j) { route_moonraker_http(j); };
    h["get_filament_presets"] = [this](const json& j) { route_get_filament_presets(j); };
    h["ifs_write_slot"]       = [this](const json& j) { route_ifs_write_slot(j); };
    h["ifs_repair"]           = [this](const json& j) { route_ifs_repair(j); };
    h["ifs_sync_all"]         = [this](const json& j) { route_ifs_sync_all(j); };
}

}} // namespace Slic3r::GUI
