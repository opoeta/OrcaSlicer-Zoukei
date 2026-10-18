#include "SnapmakerU1DevicePanel.hpp"
#include "SnapmakerU1AutoSync.hpp"
#include "FlashforgeIfsAutoSync.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/msgdlg.h>
#include <wx/sizer.h>

#include <iomanip>
#include <regex>
#include <set>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

struct SnapmakerU1DevicePanel::Bridge
{
    SnapmakerU1DevicePanel* panel{nullptr};
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

int json_int(const json& j, const char* key, int fallback)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

bool json_bool(const json& j, const char* key, bool fallback)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
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

struct U1Printer
{
    std::string name, host, model;
    int         port{7125};
};

bool is_u1_config(const DynamicPrintConfig& config)
{
    return config.has("printer_model") && boost::istarts_with(config.opt_string("printer_model"), "Snapmaker U1");
}

// Printer presets that are a U1 and have a print host, one per host (the selected printer first).
std::vector<U1Printer> u1_printers(PresetBundle& bundle, std::string* current_host)
{
    std::vector<U1Printer> out;
    std::set<std::string>  seen;
    auto add = [&](const std::string& name, const DynamicPrintConfig& config, const std::string& host_value) {
        U1Printer p;
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
    if (is_u1_config(edited.config) && !host_value.empty()) {
        add(bundle.physical_printers.has_selection() ? bundle.physical_printers.get_selected_printer_name() : edited.name, edited.config, host_value);
        if (current_host != nullptr && !out.empty())
            *current_host = out.front().host;
    }
    for (const Preset& preset : bundle.printers) {
        if (preset.is_default || !preset.is_visible || !is_u1_config(preset.config) || !preset.config.has("print_host"))
            continue;
        add(preset.name, preset.config, preset.config.opt_string("print_host"));
    }
    return out;
}

// What the page may ask Moonraker: reads of printer state, files, history, webcams; G-code scripts and print control;
// deleting a file or timelapse video. (Filament data and feeding go through the agent, see route_write_head / route_feed.)
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

bool SnapmakerU1DevicePanel::is_u1_printer(const PresetBundle& bundle)
{
    return is_u1_config(bundle.printers.get_edited_preset().config);
}

SnapmakerU1DevicePanel::SnapmakerU1DevicePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_bridge(std::make_shared<Bridge>())
{
    m_bridge->panel = this;
    SetSizer(new wxBoxSizer(wxVERTICAL));
    create_browser();
    register_routes();
}

SnapmakerU1DevicePanel::~SnapmakerU1DevicePanel()
{
    m_bridge->panel = nullptr;
    SetEvtHandlerEnabled(false);
    if (m_browser) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

void SnapmakerU1DevicePanel::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");
    if (m_browser == nullptr)
        return;
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &SnapmakerU1DevicePanel::on_script_message, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& evt) { evt.Veto(); });
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
}

void SnapmakerU1DevicePanel::load_page()
{
    if (m_browser == nullptr || m_page_requested)
        return;
    m_page_requested = true;
    const fs::path page = fs::path(resources_dir()) / "web" / "zoukeiDevice" / "index.html";
    wxString       url  = file_url_from_path(page);
    url += wxString::Format("?backend=moonraker&family=u1&dark=%d", wxGetApp().dark_mode() ? 1 : 0);
    m_browser->LoadURL(url);
}

bool SnapmakerU1DevicePanel::Show(bool show)
{
    const bool loaded = m_page_requested;
    if (show)
        load_page();
    const bool result = wxPanel::Show(show);
    if (loaded)
        post_command("panel_visible", show);
    return result;
}

void SnapmakerU1DevicePanel::on_sys_color_changed()
{
    post_command("is_dark_theme", wxGetApp().dark_mode());
}

void SnapmakerU1DevicePanel::run_script(const std::string& script)
{
    if (m_browser == nullptr || m_browser->IsBeingDeleted())
        return;
    WebView::RunScript(m_browser, wxString::FromUTF8(script));
}

void SnapmakerU1DevicePanel::post_command(const std::string& command, json data)
{
    json message = {{"command", command}, {"data", std::move(data)}};
    run_script("window.handleStudioCmd('" + url_encode(message.dump(-1, ' ', true, json::error_handler_t::replace)) + "');");
}

void SnapmakerU1DevicePanel::post_command_async(const std::string& command, json data)
{
    if (wxTheApp == nullptr)
        return;
    std::weak_ptr<Bridge> weak = m_bridge;
    wxTheApp->CallAfter([weak, command, data = std::move(data)]() mutable {
        if (auto bridge = weak.lock(); bridge && bridge->panel)
            bridge->panel->post_command(command, std::move(data));
    });
}

void SnapmakerU1DevicePanel::on_script_message(wxWebViewEvent& evt)
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
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker U1 panel: command failed: " << e.what();
    }
}

void SnapmakerU1DevicePanel::send_device_list()
{
    json          list = json::array();
    std::string   current;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr)
        for (const U1Printer& p : u1_printers(*bundle, &current)) {
            const fs::path cover = fs::path(resources_dir()) / "profiles" / "Snapmaker" / (p.model + "_cover.png");
            list.push_back({{"address", p.host},
                            {"port", p.port},
                            {"mac", p.host},
                            {"name", p.name},
                            {"model", p.model},
                            {"modelName", boost::erase_first_copy(p.model, "Snapmaker ")},
                            {"backend", "moonraker"},
                            {"family", "u1"},
                            {"cover", !p.model.empty() && fs::exists(cover) ? file_url_from_path(cover).ToStdString() : std::string()}});
        }
    post_command("device_list", {{"devices", std::move(list)}, {"current", current}});
}

// One Moonraker request for the page; answered as {requestId, ok, status, body}.
void SnapmakerU1DevicePanel::route_moonraker_http(const json& j)
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
        for (const U1Printer& p : u1_printers(*bundle, nullptr))
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

// The Orca filament presets of the selected printer, for the head editor (same data as the Creality / Flashforge panels).
void SnapmakerU1DevicePanel::route_get_filament_presets(const json& j)
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

// Writes the type / colour of one head (SET_PRINT_FILAMENT_CONFIG through the agent). Answered as {requestId, ok, reason}.
// Nothing is sent while the agent reports a block (printing, ...). A head with an official spool is only written after the
// page asked for it explicitly (`force`) AND the user confirmed in the dialog below: FORCE=1 never goes out without both.
void SnapmakerU1DevicePanel::route_write_head(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    auto              reply      = [this, request_id](bool ok, const std::string& reason) {
        post_command("u1_write_head", {{"requestId", request_id}, {"ok", ok}, {"reason", reason}});
    };
    const auto  agent = SnapmakerPrinterAgent::current();
    const int   head  = json_int(j, "head", -1);
    std::string hex   = json_string(j, "hex");
    if (!hex.empty() && hex[0] == '#')
        hex.erase(0, 1);
    static const std::regex hex_re("^[0-9A-Fa-f]{6}$");
    if (!agent || !is_snapmaker_u1_selected()) {
        reply(false, "no Snapmaker U1 selected");
        return;
    }
    if (head < 0 || head >= SnapmakerPrinterAgent::HEAD_COUNT || (!hex.empty() && !std::regex_match(hex, hex_re))) {
        reply(false, "invalid head or colour");
        return;
    }
    SnapmakerPrinterAgent::HeadWrite w;
    w.head     = head;
    w.type     = json_string(j, "type");
    w.name     = json_string(j, "name");
    w.vendor   = json_string(j, "vendor");
    w.color    = hex.empty() ? std::string() : "#" + boost::to_upper_copy(hex);
    w.set_type = json_bool(j, "set_type", true) && !w.type.empty();
    if (j.contains("colours") && j["colours"].is_array() && j["colours"].size() >= 2 && j["colours"].size() <= 4 && !w.color.empty()) {
        std::vector<std::string> cs;
        for (const json& c : j["colours"]) {
            std::string v = c.is_string() ? c.get<std::string>() : std::string();
            if (!v.empty() && v[0] == '#')
                v.erase(0, 1);
            if (!std::regex_match(v, hex_re)) {
                cs.clear();
                break;
            }
            cs.push_back("#" + boost::to_upper_copy(v));
        }
        if (cs.size() >= 2 && cs.front() == w.color) {
            w.colors   = cs;
            w.gradient = json_string(j, "color_type") == "gradient";
        }
    }

    bool       confirmed = false;
    const auto state     = agent->u1_state();
    if (state.valid && head < int(state.heads.size()) && state.heads[size_t(head)].official) {
        if (!json_bool(j, "force", false)) {
            reply(false, "official spool: locked (the page must ask to override it)");
            return;
        }
        wxMessageDialog dlg(this,
                            wxString::Format(_L("Head T%d holds an official Snapmaker spool (NFC tag). Its data is locked on the printer.\n\n"
                                                "Override it with the new type and colour anyway?"),
                                             head + 1),
                            _L("Official Snapmaker spool"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        if (dlg.ShowModal() != wxID_YES) {
            reply(false, "cancelled");
            return;
        }
        confirmed = true;
    }
    const std::string reason = agent->write_head(w, confirmed);
    reply(reason.empty(), reason);
    if (!reason.empty())
        return;
    // Printer -> project: the filaments mapped to this head follow what was just written (no echo back).
    const std::string preset = json_string(j, "preset"), wtype = w.type, wcolor = w.color;
    const auto        wcols = w.colors;
    const bool        wgrad = w.gradient;
    wxGetApp().CallAfter([head, wtype, wcolor, preset, wcols, wgrad] {
        FlashforgeIfsAutoSync::apply_slot_to_project(head + 1, wtype, wcolor, preset, wcols, wgrad);
        SnapmakerU1AutoSync::rebaseline();
    });
}

// Load / unload of one head: heats the nozzle and moves filament, so the page asks first; only an explicit click gets here.
void SnapmakerU1DevicePanel::route_feed(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    const std::string action     = json_string(j, "action");
    const int         head       = json_int(j, "head", -1);
    std::string       reason;
    const auto        agent = SnapmakerPrinterAgent::current();
    if (!agent || !is_snapmaker_u1_selected())
        reason = "no Snapmaker U1 selected";
    else if (action != "load" && action != "unload")
        reason = "invalid action";
    else
        reason = agent->feed_head(head, action == "load" ? SnapmakerPrinterAgent::FeedAction::load : SnapmakerPrinterAgent::FeedAction::unload);
    post_command("u1_feed", {{"requestId", request_id}, {"ok", reason.empty()}, {"reason", reason}});
}

// "Sincronizar tudo agora": the project filaments follow the heads.
void SnapmakerU1DevicePanel::route_sync_all(const json& j)
{
    json out = {{"requestId", json_string(j, "requestId")}, {"ok", false}, {"reason", ""}, {"changed", ""}};
    if (!SnapmakerPrinterAgent::current() || !is_snapmaker_u1_selected())
        out["reason"] = "no Snapmaker U1 selected";
    else {
        const std::string changed = SnapmakerU1AutoSync::apply_all_to_project();
        out["ok"]      = true;
        out["changed"] = changed;
        CrealityCfsAgent::notify(changed.empty() ? _u8L("Snapmaker U1: the project already matches the printer.") :
                                                   _u8L("Snapmaker U1: project updated") + " - " + changed);
    }
    post_command("u1_sync_all", std::move(out));
}

void SnapmakerU1DevicePanel::register_routes()
{
    auto& h = m_handlers;
    h["is_dark_theme"]  = [this](const json&) { post_command("is_dark_theme", wxGetApp().dark_mode()); };
    h["get_lang"]       = [this](const json&) { post_command("get_lang", wxGetApp().current_language_code().ToStdString()); };
    h["device_list"]    = [this](const json&) { send_device_list(); };
    h["moonraker_http"] = [this](const json& j) { route_moonraker_http(j); };
    // The pause marks of a job file are not read for the U1 (the progress bar simply has no marks).
    h["moonraker_pauses"] = [this](const json& j) {
        post_command("moonraker_pauses", {{"requestId", json_string(j, "requestId")}, {"ok", false}, {"error", "not supported"}});
    };
    h["get_filament_presets"] = [this](const json& j) { route_get_filament_presets(j); };
    h["u1_write_head"]        = [this](const json& j) { route_write_head(j); };
    h["u1_feed"]              = [this](const json& j) { route_feed(j); };
    h["u1_sync_all"]          = [this](const json& j) { route_sync_all(j); };
}

}} // namespace Slic3r::GUI
