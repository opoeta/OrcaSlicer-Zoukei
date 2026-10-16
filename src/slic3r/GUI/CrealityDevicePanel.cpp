#include "CrealityDevicePanel.hpp"

#include "CrealityDevice/DeviceStore.hpp"
#include "CrealityDevice/ImageServer.hpp"
#include "CrealityDevice/LanDeviceProbe.hpp"
#include "CrealityDevice/WsProxy.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/Http.hpp"

#include "cxmdns.h"

#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/utils.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

using namespace CrealityDevice;
using json = nlohmann::json;

// Lets work on other threads find the panel again, on the UI thread, only while it lives.
struct CrealityDevicePanel::Bridge
{
    CrealityDevicePanel* panel{nullptr};
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

bool is_lan_ipv4(const std::string& address)
{
    boost::system::error_code ec;
    const auto parsed = boost::asio::ip::make_address(address, ec);
    if (ec || !parsed.is_v4())
        return false;
    const auto b = parsed.to_v4().to_bytes();
    return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
}

// "http://10.1.2.3:8000/x" -> "10.1.2.3"
std::string host_of_url(const std::string& url)
{
    std::size_t start = url.find("://");
    start             = start == std::string::npos ? 0 : start + 3;
    const std::size_t end = url.find_first_of(":/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string json_string(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

json read_json_file(const fs::path& path)
{
    try {
        if (!fs::exists(path))
            return json();
        boost::nowide::ifstream in(path.string());
        std::stringstream       buffer;
        buffer << in.rdbuf();
        json parsed = json::parse(buffer.str(), nullptr, false);
        return parsed.is_discarded() ? json() : parsed;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality panel: cannot read " << path.string() << ": " << e.what();
        return json();
    }
}

// The newest Creality Print data folder holding `file` (read only; never written).
fs::path creality_print_file(const std::string& file)
{
    wxString appdata;
    if (!wxGetEnv("APPDATA", &appdata))
        return {};
    const fs::path root = fs::path(appdata.ToUTF8().data()) / "Creality" / "Creality Print";
    fs::path       best;
    std::string    best_version;
    try {
        if (!fs::is_directory(root))
            return {};
        for (const auto& entry : fs::directory_iterator(root)) {
            if (!fs::is_directory(entry.path()))
                continue;
            const fs::path candidate = entry.path() / file;
            const std::string version = entry.path().filename().string();
            if (fs::exists(candidate) && (best.empty() || version > best_version)) {
                best         = candidate;
                best_version = version;
            }
        }
    } catch (const std::exception&) {}
    return best;
}

// Known addresses the proxies may reach: the device list plus printers the user just probed.
std::mutex            s_hosts_mutex;
std::set<std::string> s_hosts;

void remember_host(const std::string& host)
{
    if (!is_lan_ipv4(host))
        return;
    std::lock_guard<std::mutex> lock(s_hosts_mutex);
    s_hosts.insert(host);
}

void refresh_hosts_from_store()
{
    const json data = DeviceStore::Ins().GetData();
    if (!data.contains("groups") || !data["groups"].is_array())
        return;
    for (const auto& group : data["groups"])
        if (group.is_object() && group.contains("list") && group["list"].is_array())
            for (const auto& item : group["list"])
                if (item.is_object())
                    remember_host(item.value("address", std::string()));
}

// Printer host of the selected printer (physical printer first), without scheme or port.
std::string selected_print_host_address()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    const DynamicPrintConfig* config = nullptr;
    if (bundle->physical_printers.has_selection())
        config = bundle->physical_printers.get_selected_printer_config();
    if (config == nullptr || !config->has("print_host") || config->opt_string("print_host").empty())
        config = &bundle->printers.get_edited_preset().config;
    if (!config->has("print_host"))
        return {};
    return host_of_url(config->opt_string("print_host"));
}

// Model code ("F008") -> product name and Orca's printer picture. The names come from Creality
// Print's machine list when it is installed (read only), else from a short built-in table.
struct ModelInfo
{
    std::string name;
    std::string cover; // file:// URL, empty when Orca has no picture
};

ModelInfo model_info(const std::string& code)
{
    static std::map<std::string, std::string> names;
    static std::map<std::string, std::string> covers;
    static bool                               loaded = false;
    if (!loaded) {
        loaded = true;
        json list = read_json_file(fs::path(data_dir()) / "zoukei_creality_machine_list.json");
        if (!list.is_array()) {
            const fs::path cp_list = creality_print_file("system/Creality/machineList.json");
            const json     parsed  = cp_list.empty() ? json() : read_json_file(cp_list);
            if (parsed.is_object() && parsed.contains("printerList") && parsed["printerList"].is_array())
                list = parsed["printerList"];
        }
        if (list.is_array())
            for (const auto& item : list)
                if (item.is_object() && !json_string(item, "printerIntName").empty() && !json_string(item, "name").empty())
                    names[json_string(item, "printerIntName")] = json_string(item, "name");
        for (const auto& [id, name] : std::vector<std::pair<const char*, const char*>>{
                 {"F008", "K2 Plus"}, {"F012", "K2 Pro"}, {"F021", "K2"}, {"F022", "SPARKX i7"}, {"K1", "K1"}, {"K1 SE", "K1 SE"}, {"K1C", "K1C"}})
            names.emplace(id, name);
    }
    ModelInfo info;
    const auto it = names.find(code);
    info.name     = it != names.end() ? it->second : code;
    if (it != names.end()) {
        auto cached = covers.find(code);
        if (cached == covers.end()) {
            const fs::path cover = fs::path(resources_dir()) / "profiles" / "Creality" / ("Creality " + it->second + "_cover.png");
            cached               = covers.emplace(code, fs::exists(cover) ? file_url_from_path(cover).ToStdString() : std::string()).first;
        }
        info.cover = cached->second;
    }
    return info;
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool CrealityDevicePanel::is_creality_printer(const PresetBundle& bundle)
{
    const Preset& preset = bundle.printers.get_selected_preset();
    const Preset* parent = bundle.printers.get_selected_preset_parent();
    for (const Preset* p : {&preset, parent})
        if (p != nullptr && p->vendor != nullptr && boost::iequals(p->vendor->id, "Creality"))
            return true;

    const DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;
    if (const auto* host_type = config.option<ConfigOptionEnum<PrintHostType>>("host_type"); host_type != nullptr && host_type->value == htCrealityPrint)
        return true;
    if (config.has("printer_model") && boost::istarts_with(config.opt_string("printer_model"), "Creality"))
        return true;
    return false;
}

CrealityDevicePanel::CrealityDevicePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_bridge(std::make_shared<Bridge>())
{
    m_bridge->panel = this;
    ensure_device_store_loaded();

    m_image_server = std::make_unique<ImageServer>([](const std::string& host) {
        std::lock_guard<std::mutex> lock(s_hosts_mutex);
        return s_hosts.count(host) != 0;
    });
    m_ws_proxy = std::make_unique<WsProxy>([this](const std::string& script) { run_script(script); },
                                           [](const std::string& host) { return is_known_lan_host(host); });

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer);
    create_browser();
    register_routes();
}

CrealityDevicePanel::~CrealityDevicePanel()
{
    m_bridge->panel = nullptr;
    SetEvtHandlerEnabled(false);
    if (m_ws_proxy)
        m_ws_proxy->shutdown();
    if (m_image_server)
        m_image_server->stop();
    if (m_browser) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

void CrealityDevicePanel::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");
    if (m_browser == nullptr)
        return;
    m_browser->AddUserScript(wxString::FromUTF8(WsProxy::page_script()));
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &CrealityDevicePanel::on_script_message, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &CrealityDevicePanel::on_new_window, this);
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
}

void CrealityDevicePanel::load_page()
{
    if (m_browser == nullptr || m_page_requested)
        return;
    m_page_requested = true;
    const fs::path page = fs::path(resources_dir()) / "web" / "zoukeiDevice" / "index.html";
    wxString url = file_url_from_path(page);
    url += wxString::Format("?port=%u&dark=%d", unsigned(m_image_server ? m_image_server->port() : 0), wxGetApp().dark_mode() ? 1 : 0);
    BOOST_LOG_TRIVIAL(info) << "Creality panel: loading " << page.string();
    m_browser->LoadURL(url);
}

bool CrealityDevicePanel::Show(bool show)
{
    const bool page_was_loading = m_page_requested;
    if (show)
        load_page();
    const bool result = wxPanel::Show(show);
    // The page stops the camera and the list's refresh while the tab is not on screen.
    if (page_was_loading)
        post_command("panel_visible", show);
    return result;
}

void CrealityDevicePanel::on_sys_color_changed()
{
    post_command("is_dark_theme", wxGetApp().dark_mode());
}

// The panel never opens windows of its own; anything it asks for is dropped.
void CrealityDevicePanel::on_new_window(wxWebViewEvent& evt)
{
    evt.Veto();
}

void CrealityDevicePanel::run_script(const std::string& script)
{
    if (m_browser == nullptr || m_browser->IsBeingDeleted())
        return;
    WebView::RunScript(m_browser, wxString::FromUTF8(script));
}

void CrealityDevicePanel::post_command(const std::string& command, json data)
{
    json message = {{"command", command}, {"data", std::move(data)}};
    run_script("window.handleStudioCmd('" + url_encode(message.dump(-1, ' ', true, json::error_handler_t::replace)) + "');");
}

void CrealityDevicePanel::post_command_async(const std::string& command, json data)
{
    if (wxTheApp == nullptr)
        return;
    std::weak_ptr<Bridge> weak = m_bridge;
    wxTheApp->CallAfter([weak, command, data = std::move(data)]() mutable {
        if (auto bridge = weak.lock(); bridge && bridge->panel)
            bridge->panel->post_command(command, std::move(data));
    });
}

bool CrealityDevicePanel::is_known_lan_host(const std::string& host)
{
    if (!is_lan_ipv4(host))
        return false;
    std::lock_guard<std::mutex> lock(s_hosts_mutex);
    return s_hosts.count(host) != 0;
}

void CrealityDevicePanel::on_script_message(wxWebViewEvent& evt)
{
    json j;
    try {
        j = json::parse(evt.GetString().ToUTF8().data());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality panel: unreadable message: " << e.what();
        return;
    }
    if (!j.is_object())
        return;
    const std::string command = json_string(j, "command");
    if (command == "ws_proxy") {
        if (m_ws_proxy && j.contains("payload"))
            m_ws_proxy->handle(j["payload"]);
        return;
    }
    auto it = m_handlers.find(command);
    if (it == m_handlers.end()) {
        BOOST_LOG_TRIVIAL(info) << "Creality panel: ignored command " << command;
        return;
    }
    try {
        it->second(j);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality panel: command " << command << " failed: " << e.what();
    }
    refresh_hosts_from_store();
}

// ---------------------------------------------------------------------------------------------
// Device list

namespace {

// Adds the probed printer to the list, or points its entry at the new address when the printer
// (same MAC) is already known. Returns false when nothing was stored.
bool store_probed_device(const LanConnectionProfile& profile)
{
    DeviceStore& store = DeviceStore::Ins();
    if (store.FindByAddress(profile.address))
        return true;
    if (!profile.mac.empty() && store.FindByMac(profile.mac)) {
        DeviceStore::UpdatePatch patch;
        patch.address                     = profile.address;
        patch.model                       = profile.model;
        patch.connection.secureConnection = profile.secure_connection;
        patch.connection.wssPort          = profile.wss_port;
        patch.connection.videoPort        = profile.video_port;
        return store.UpdateDevicePatch(profile.mac, patch, true);
    }
    DeviceStore::Data data{};
    data.address          = profile.address;
    data.mac              = profile.mac;
    data.model            = profile.model;
    data.connectType      = 3;
    data.hostType         = 0;
    data.secureConnection = profile.secure_connection;
    data.wssPort          = profile.wss_port;
    data.videoPort        = profile.video_port;
    const json        groups = store.GetData()["groups"];
    const std::string group  = groups.is_array() && !groups.empty() ? groups[0].value("group", std::string("New Group1")) : std::string("New Group1");
    store.AddDevice(group, data);
    remember_host(profile.address);
    return true;
}

} // namespace

void CrealityDevicePanel::ensure_device_store_loaded()
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;

    DeviceStore& store = DeviceStore::Ins();
    if (!store.Load()) {
        // First use: take the list Creality Print already has (read only), else the
        // selected printer's host.
        const fs::path cp_devices = creality_print_file("deviceInfo.json");
        json           imported   = cp_devices.empty() ? json() : read_json_file(cp_devices);
        if (imported.is_object() && imported.contains("groups")) {
            imported.erase("mergeState");
            BOOST_LOG_TRIVIAL(info) << "Creality panel: imported the device list of Creality Print (" << cp_devices.string() << ")";
            store.Import(imported);
        } else {
            store.Import(json::object());
            const std::string address = wxGetApp().preset_bundle != nullptr && is_creality_printer(*wxGetApp().preset_bundle) ?
                                            selected_print_host_address() :
                                            std::string();
            if (is_lan_ipv4(address)) {
                remember_host(address);
                // Fill in the device from the printer itself; the list needs its MAC.
                std::thread([address] {
                    const auto result = LanDeviceProbe().probe(address, ProbePolicy::PreferSecure);
                    if (!result.ok() || wxTheApp == nullptr)
                        return;
                    const auto profile = *result.profile;
                    wxTheApp->CallAfter([profile] {
                        store_probed_device(profile);
                        BOOST_LOG_TRIVIAL(info) << "Creality panel: added the selected printer " << profile.address << " (" << profile.model << ")";
                        if (CrealityDevicePanel* panel = CrealityDevicePanel::if_built())
                            panel->send_device_list();
                    });
                }).detach();
            }
        }
    }
    refresh_hosts_from_store();
}

// The printers the page lists: LAN Creality printers of the device store.
nlohmann::json CrealityDevicePanel::device_list() const
{
    json              out  = json::array();
    const json        data = DeviceStore::Ins().GetData();
    const std::string current = DeviceStore::Ins().GetCurrentDevice();
    if (!data.contains("groups") || !data["groups"].is_array())
        return out;
    for (const auto& group : data["groups"]) {
        if (!group.is_object() || !group.contains("list") || !group["list"].is_array())
            continue;
        for (const auto& item : group["list"]) {
            if (!item.is_object() || item.value("connectType", 0) == 1001)
                continue;
            const std::string address = item.value("address", std::string());
            if (!is_lan_ipv4(address))
                continue;
            const std::string model = item.value("model", std::string());
            const ModelInfo   info  = model_info(model);
            out.push_back({{"address", address},
                           {"mac", item.value("mac", std::string())},
                           {"model", model},
                           {"modelName", info.name},
                           {"cover", info.cover},
                           {"name", item.value("name", std::string())},
                           {"secure", item.value("secureConnection", false)},
                           {"wssPort", item.value("wssPort", 0)},
                           {"videoPort", item.value("videoPort", 0)}});
        }
    }
    return out;
}

void CrealityDevicePanel::send_device_list()
{
    refresh_hosts_from_store();
    json list;
    try {
        list = device_list();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality panel: device list unreadable: " << e.what();
        list = json::array();
    }
    post_command("device_list", {{"devices", std::move(list)}, {"current", DeviceStore::Ins().GetCurrentDevice()}});
}

// Adds a printer by IP: it is asked for its identity first (HTTPS, then HTTP), so a wrong
// address never ends up in the list.
void CrealityDevicePanel::route_device_add(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    const std::string address    = json_string(j, "address");
    auto reply = [weak = std::weak_ptr<Bridge>(m_bridge), request_id](json out) {
        out["requestId"] = request_id;
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([weak, out = std::move(out)]() mutable {
                if (auto bridge = weak.lock(); bridge && bridge->panel) {
                    const bool ok = out.value("ok", false);
                    bridge->panel->post_command("device_add_result", std::move(out));
                    if (ok)
                        bridge->panel->send_device_list();
                }
            });
    };
    if (request_id.empty() || request_id.size() > 128 || !is_lan_ipv4(address)) {
        reply({{"ok", false}, {"errorType", "invalid_address"}, {"error", ""}});
        return;
    }
    submit_lan_device_probe_task([reply, address] {
        const auto result = LanDeviceProbe().probe(address, ProbePolicy::PreferSecure);
        if (!result.ok()) {
            reply({{"ok", false}, {"errorType", probe_error_name(result.error)}, {"error", result.detail}});
            return;
        }
        const LanConnectionProfile profile = *result.profile;
        if (wxTheApp == nullptr)
            return;
        wxTheApp->CallAfter([reply, profile] {
            reply({{"ok", store_probed_device(profile)}, {"errorType", ""}, {"error", ""}});
        });
    });
}

// Finds Creality printers by mDNS and asks each for its identity.
void CrealityDevicePanel::route_lan_scan(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    if (m_scan_running) {
        post_command("lan_scan_result", {{"requestId", request_id}, {"status", 1}, {"busy", true}, {"found", json::array()}});
        return;
    }
    m_scan_running             = true;
    std::weak_ptr<Bridge> weak = m_bridge;
    std::thread([weak, request_id] {
        std::vector<std::string> ips;
        try {
            for (const auto& item : cxnet::syncDiscoveryService({"CXSWBox", "creality", "Creality"})) {
                if (!is_lan_ipv4(item.machineIp) || item.answer.rfind("_CXSWBox", 0) == 0)
                    continue;
                // "_creality<type:2><moonraker port:4>..." announces a Klipper host, not a K-series printer.
                if (item.answer.size() > 15 && item.answer.rfind("_creality00", 0) == 0 &&
                    std::all_of(item.answer.begin() + 9, item.answer.begin() + 15, [](unsigned char c) { return std::isdigit(c) != 0; }))
                    continue;
                if (std::find(ips.begin(), ips.end(), item.machineIp) == ips.end())
                    ips.push_back(item.machineIp);
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Creality panel: LAN discovery failed: " << e.what();
        }

        ProbeOptions options;
        options.preflight_ports       = true;
        options.https_timeout_seconds = 2;
        options.http_timeout_seconds  = 3;
        const auto results            = LanDeviceProbe().probe_many(ips, ProbePolicy::PreferSecure, options);
        json       found              = json::array();
        for (const auto& result : results)
            if (result.ok()) {
                remember_host(result.profile->address);
                json entry = {{"address", result.profile->address}, {"mac", result.profile->mac}, {"model", result.profile->model},
                              {"modelName", model_info(result.profile->model).name}};
                found.push_back(std::move(entry));
            }
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([weak, request_id, found = std::move(found)]() mutable {
                if (auto bridge = weak.lock(); bridge && bridge->panel) {
                    bridge->panel->m_scan_running = false;
                    bridge->panel->post_command("lan_scan_result", {{"requestId", request_id}, {"status", 0}, {"found", std::move(found)}});
                }
            });
    }).detach();
}

// The Orca filament presets of the selected printer, for the CFS slot editor.
void CrealityDevicePanel::route_get_filament_presets(const json& j)
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

// ---------------------------------------------------------------------------------------------
// Camera: WebRTC signalling goes through the host (the page cannot POST to the printer).

void CrealityDevicePanel::route_get_webrtc_local_param(const json& j)
{
    const std::string url        = json_string(j, "url");
    std::string       sdp        = json_string(j, "sdp");
    const std::string request_id = json_string(j, "requestId");
    const std::string token      = json_string(j, "token");
    bool              encrypted  = j.value("videoEncryption", false) || url.rfind("https://", 0) == 0;
    const std::string address    = host_of_url(url);

    auto post_result = [weak = std::weak_ptr<Bridge>(m_bridge), request_id](json out) {
        out["requestId"] = request_id;
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([weak, out = std::move(out)]() mutable {
                if (auto bridge = weak.lock(); bridge && bridge->panel)
                    bridge->panel->post_command("get_webrtc_local_param", std::move(out));
            });
    };
    if (!is_lan_ipv4(address) || !(boost::istarts_with(url, "http://") || boost::istarts_with(url, "https://"))) {
        post_result({{"url", url}, {"videoEncryption", encrypted}, {"status", 0}, {"error", "only LAN printers are reached"}});
        return;
    }

    // The browser hides its LAN address behind an mDNS name; the printer needs the real one.
    try {
        boost::asio::io_context        io;
        boost::asio::ip::udp::socket   socket(io);
        socket.connect(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(address), 80));
        const std::string local_ip = socket.local_endpoint().address().to_string();
        socket.close();
        std::vector<std::string> lines;
        boost::split(lines, sdp, boost::is_any_of("\n"));
        for (const auto& line : lines) {
            if (line.find("a=candidate") == std::string::npos)
                continue;
            std::vector<std::string> parts;
            boost::split(parts, line, boost::is_any_of(" "));
            if (parts.size() > 4 && !parts[4].empty())
                boost::algorithm::replace_first(sdp, parts[4], local_ip);
            break;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << "Creality panel: local address for WebRTC not found: " << e.what();
    }

    json offer = {{"type", "offer"}, {"sdp", sdp}};
    if (!token.empty())
        offer["token"] = token;
    const std::string offer_text = offer.dump();
    std::string       body(boost::beast::detail::base64::encoded_size(offer_text.size()), '\0');
    body.resize(boost::beast::detail::base64::encode(body.data(), offer_text.data(), offer_text.size()));

    auto post_offer = [post_result, body](const std::string& target, bool secure, std::function<void()> on_failure) {
        try {
            Http::post(target)
                .timeout_connect(10)
                .timeout_max(15)
                .header("Content-Type", "plain/text")
                .set_post_body(body)
                .on_complete([post_result, target, secure](std::string answer, unsigned status) {
                    post_result({{"sdp", std::move(answer)}, {"url", target}, {"videoEncryption", secure}, {"status", status}});
                })
                .on_error([post_result, target, secure, on_failure](std::string answer, std::string error, unsigned status) {
                    if (on_failure) {
                        on_failure();
                        return;
                    }
                    json out = {{"url", target}, {"videoEncryption", secure}, {"status", status}};
                    if (!answer.empty())
                        out["sdp"] = std::move(answer);
                    else
                        out["error"] = std::move(error);
                    post_result(std::move(out));
                })
                .perform();
        } catch (const std::exception& e) {
            if (on_failure)
                on_failure();
            else
                post_result({{"url", target}, {"videoEncryption", secure}, {"status", 0}, {"error", e.what()}});
        }
    };

    if (encrypted) {
        // As Creality Print: fall back to the printer's plain signalling port.
        const std::string fallback = "http://" + address + ":8000/call/webrtc_local";
        post_offer(url, true, [post_offer, fallback] { post_offer(fallback, false, nullptr); });
    } else {
        post_offer(url, false, nullptr);
    }
}

// ---------------------------------------------------------------------------------------------
// Timelapse download: the page cannot save files, so the host asks for a path and fetches the
// video from the printer.

void CrealityDevicePanel::route_download_video(const json& j)
{
    const std::string address    = json_string(j, "address");
    const std::string name       = json_string(j, "name");
    const std::string request_id = json_string(j, "requestId");
    auto reply = [this, request_id](json out) {
        out["requestId"] = request_id;
        post_command_async("download_video", std::move(out));
    };
    const bool name_ok = name.size() > 4 && name.size() < 128 && boost::iends_with(name, ".mp4") &&
                         std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
    if (!is_known_lan_host(address) || !name_ok) {
        reply({{"ok", false}, {"error", "invalid request"}});
        return;
    }
    wxFileDialog dlg(this, wxString("Save timelapse video"), wxEmptyString, wxString::FromUTF8(name), "MP4 (*.mp4)|*.mp4",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) {
        reply({{"ok", false}, {"cancelled", true}});
        return;
    }
    const std::string target = dlg.GetPath().ToUTF8().data();
    try {
        Http::get("http://" + address + "/downloads/video/" + name)
            .timeout_connect(10)
            .timeout_max(600)
            .size_limit(1024ull * 1024 * 1024)
            .on_complete([reply, target](std::string body, unsigned status) {
                if (status != 200 || body.empty()) {
                    reply({{"ok", false}, {"error", "HTTP " + std::to_string(status)}});
                    return;
                }
                boost::nowide::ofstream out(target, std::ios::binary | std::ios::trunc);
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                out.close();
                if (!out)
                    reply({{"ok", false}, {"error", "could not write the file"}});
                else
                    reply({{"ok", true}, {"path", target}});
            })
            .on_error([reply](std::string, std::string error, unsigned) { reply({{"ok", false}, {"error", error}}); })
            .perform();
    } catch (const std::exception& e) {
        reply({{"ok", false}, {"error", e.what()}});
    }
}

// ---------------------------------------------------------------------------------------------
// Routes: everything the page may ask of the host.

void CrealityDevicePanel::register_routes()
{
    auto& h = m_handlers;

    // Orca's language and theme (the theme is also pushed on a live switch, see on_sys_color_changed).
    h["is_dark_theme"] = [this](const json&) { post_command("is_dark_theme", wxGetApp().dark_mode()); };
    h["get_lang"]      = [this](const json&) { post_command("get_lang", wxGetApp().current_language_code().ToStdString()); };

    // Device list.
    h["device_list"]   = [this](const json&) { send_device_list(); };
    h["device_select"] = [](const json& j) {
        const std::string mac = json_string(j, "mac");
        if (!mac.empty())
            DeviceStore::Ins().SetCurrentDevice(mac);
    };
    h["device_rename"] = [this](const json& j) {
        DeviceStore::Ins().EditDeiveName(json_string(j, "address"), json_string(j, "name"));
        send_device_list();
    };
    h["device_remove"] = [this](const json& j) {
        DeviceStore::Ins().RemoveDevice(json_string(j, "address"));
        send_device_list();
    };
    h["device_add"] = [this](const json& j) { route_device_add(j); };
    h["lan_scan"]   = [this](const json& j) { route_lan_scan(j); };

    // Camera signalling and the Orca filament presets.
    h["get_webrtc_local_param"] = [this](const json& j) { route_get_webrtc_local_param(j); };
    h["get_filament_presets"]   = [this](const json& j) { route_get_filament_presets(j); };
    h["download_video"]         = [this](const json& j) { route_download_video(j); };
}

}} // namespace Slic3r::GUI
