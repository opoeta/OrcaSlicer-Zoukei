#include "CrealityDevicePanel.hpp"

#include "CrealityDevice/DeviceStore.hpp"
#include "CrealityDevice/ImageServer.hpp"
#include "CrealityDevice/LanDeviceProbe.hpp"
#include "CrealityDevice/WsProxy.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include "cxmdns.h"

#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <wx/sizer.h>
#include <wx/utils.h>

#include <cstdint>
#include <iomanip>
#include <limits>
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

// Creality Print's version this page comes from; the page reports it, nothing else.
const char* const PANEL_VERSION = "7.3.0";

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

bool write_json_file(const fs::path& path, const json& data)
{
    try {
        if (!path.parent_path().empty() && !fs::exists(path.parent_path()))
            fs::create_directories(path.parent_path());
        boost::nowide::ofstream out(path.string(), std::ios::out | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << std::setw(4) << data << std::endl;
        return out.good();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality panel: cannot write " << path.string() << ": " << e.what();
        return false;
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

const char* UpdateDeviceCode(DeviceStore::UpdateDeviceOutcome outcome)
{
    using Outcome = DeviceStore::UpdateDeviceOutcome;
    switch (outcome) {
    case Outcome::Updated: return "updated";
    case Outcome::AlreadyApplied: return "already_applied";
    case Outcome::StaleSource: return "stale_source";
    case Outcome::SourceNotFound: return "source_not_found";
    case Outcome::DuplicateMacConflict: return "duplicate_mac_conflict";
    case Outcome::AddressConflict: return "address_conflict";
    case Outcome::PersistenceFailed: return "persistence_failed";
    case Outcome::InvalidRequest: return "invalid_request";
    }
    return "invalid_request";
}

bool UpdateDeviceSucceeded(DeviceStore::UpdateDeviceOutcome outcome)
{
    return outcome == DeviceStore::UpdateDeviceOutcome::Updated || outcome == DeviceStore::UpdateDeviceOutcome::AlreadyApplied;
}

const char* DeviceOwnershipBatchCode(DeviceStore::DeviceOwnershipBatchOutcome outcome)
{
    using Outcome = DeviceStore::DeviceOwnershipBatchOutcome;
    switch (outcome) {
    case Outcome::Reconciled: return "reconciled";
    case Outcome::AlreadyApplied: return "already_applied";
    case Outcome::StaleSource: return "stale_source";
    case Outcome::PersistenceFailed: return "persistence_failed";
    case Outcome::InvalidRequest: return "invalid_request";
    }
    return "invalid_request";
}

bool DeviceOwnershipBatchSucceeded(DeviceStore::DeviceOwnershipBatchOutcome outcome)
{
    return outcome == DeviceStore::DeviceOwnershipBatchOutcome::Reconciled ||
           outcome == DeviceStore::DeviceOwnershipBatchOutcome::AlreadyApplied;
}

json ModernLanSnapshotToJson(const DeviceStore::ModernLanDeviceSnapshot& snapshot)
{
    const auto& data = snapshot.data;
    return {{"rawIndex", snapshot.rawIndex},
            {"group", snapshot.group},
            {"address", data.address},
            {"addressRevision", data.addressRevision},
            {"mac", data.mac},
            {"model", data.model},
            {"name", data.name},
            {"deviceUI", data.deviceUI},
            {"connectType", data.connectType},
            {"type", data.connectType},
            {"oldPrinter", data.oldPrinter},
            {"secureConnection", data.secureConnection},
            {"wssPort", data.wssPort},
            {"videoPort", data.videoPort},
            {"moonrakerPort", data.moonrakerPort},
            {"fluiddPort", data.fluiddPort},
            {"mainsailPort", data.mainsailPort},
            {"apiKey", data.apiKey},
            {"hostType", data.hostType},
            {"caFile", data.caFile},
            {"ignoreCertRevocation", data.ignoreCertRevocation}};
}

json InvalidProbeRequest(const std::string& request_id, const std::string& detail)
{
    return {{"requestId", request_id}, {"status", 1}, {"errorType", "invalid_request"}, {"error", detail}, {"httpStatus", 0},
            {"attempts", json::array()}};
}

// Creality Print's frontend language codes.
std::string frontend_language_code()
{
    const wxString code = wxGetApp().current_language_code();
    if (code == "zh_TW")
        return "zh_TW";
    if (code == "zh_CN")
        return "zh_CN";
    static const std::map<std::string, std::string> mapping{
        {"en", "en_GB"}, {"ca", "ca_ES"}, {"cs", "cs_CZ"}, {"sk", "cs_CZ"}, {"de", "de_DE"}, {"es", "es_ES"}, {"fr", "fr_FR"},
        {"hu", "hu_HU"}, {"id", "id_ID"}, {"it", "it_IT"}, {"ja", "ja_JP"}, {"ko", "ko_KR"}, {"nl", "nl_NL"}, {"pl", "pl_PL"},
        {"pt", "pt_BR"}, {"ru", "ru_RU"}, {"sv", "sv_SE"}, {"th", "th_TH"}, {"tr", "tr_TR"}, {"uk", "uk_UA"}, {"vi", "vi_VN"},
        {"zh", "zh_CN"}};
    const auto it = mapping.find(code.BeforeFirst('_').ToStdString());
    return it != mapping.end() ? it->second : "en_GB";
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
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &CrealityDevicePanel::on_loaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &CrealityDevicePanel::on_new_window, this);
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
}

void CrealityDevicePanel::load_page()
{
    if (m_browser == nullptr || m_page_requested)
        return;
    m_page_requested = true;
    const fs::path page = fs::path(resources_dir()) / "web" / "crealityDevice" / "index.html";
    wxString url = file_url_from_path(page);
    url += wxString::Format("?version=%s&port=%u&os=%s&customized=0&type=Release", PANEL_VERSION,
                            unsigned(m_image_server ? m_image_server->port() : 0), url_encode(wxGetOsDescription().ToStdString()));
    BOOST_LOG_TRIVIAL(info) << "Creality panel: loading " << page.string();
    m_browser->LoadURL(url);
}

bool CrealityDevicePanel::Show(bool show)
{
    if (show)
        load_page();
    return wxPanel::Show(show);
}

void CrealityDevicePanel::on_sys_color_changed()
{
    post_command("is_dark_theme", wxGetApp().dark_mode());
}

void CrealityDevicePanel::on_loaded(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(info) << "Creality panel: page loaded";
}

void CrealityDevicePanel::on_new_window(wxWebViewEvent& evt)
{
    const std::string url = evt.GetURL().ToStdString();
    if (boost::istarts_with(url, "http://") || boost::istarts_with(url, "https://"))
        wxLaunchDefaultBrowser(evt.GetURL());
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
                // Fill in the device from the printer itself; the page needs its MAC.
                std::thread([address] {
                    const auto result = LanDeviceProbe().probe(address, ProbePolicy::PreferSecure);
                    if (!result.ok())
                        return;
                    const auto profile = *result.profile;
                    if (wxTheApp == nullptr)
                        return;
                    wxTheApp->CallAfter([profile] {
                        DeviceStore& store = DeviceStore::Ins();
                        if (store.FindByAddress(profile.address) || store.FindByMac(profile.mac))
                            return;
                        DeviceStore::Data data{};
                        data.address          = profile.address;
                        data.mac              = profile.mac;
                        data.model            = profile.model;
                        data.connectType      = 3;
                        data.hostType         = 0;
                        data.secureConnection = profile.secure_connection;
                        data.wssPort          = profile.wss_port;
                        data.videoPort        = profile.video_port;
                        const json groups = store.GetData()["groups"];
                        const std::string group = groups.is_array() && !groups.empty() ? groups[0].value("group", std::string("New Group1")) :
                                                                                         std::string("New Group1");
                        store.AddDevice(group, data);
                        store.SetCurrentDevice(profile.mac);
                        BOOST_LOG_TRIVIAL(info) << "Creality panel: added the selected printer " << profile.address << " (" << profile.model << ")";
                        if (CrealityDevicePanel* panel = CrealityDevicePanel::if_built())
                            panel->route_init_device(json::object());
                    });
                }).detach();
            }
        }
    }
    refresh_hosts_from_store();
}

void CrealityDevicePanel::route_init_device(const json&)
{
    json data = DeviceStore::Ins().GetData();
    if (!data.contains("current_device") || !data["current_device"].is_object() ||
        data["current_device"].value("mac", std::string()).empty()) {
        // One device and nothing chosen yet: that is the current one.
        std::vector<std::string> macs;
        if (data.contains("groups") && data["groups"].is_array())
            for (const auto& group : data["groups"])
                if (group.is_object() && group.contains("list") && group["list"].is_array())
                    for (const auto& item : group["list"])
                        if (item.is_object() && !item.value("mac", std::string()).empty())
                            macs.push_back(item.value("mac", std::string()));
        if (macs.size() == 1)
            data["current_device"] = {{"mac", macs.front()}};
    }
    post_command("init_device", std::move(data));
}

// ---------------------------------------------------------------------------------------------
// Machine list (printer names and pictures by model code)

json CrealityDevicePanel::machine_list() const
{
    const fs::path cache = fs::path(data_dir()) / "zoukei_creality_machine_list.json";
    json           list  = read_json_file(cache);
    if (!list.is_array()) {
        const fs::path cp_list = creality_print_file("system/Creality/machineList.json");
        json           parsed  = cp_list.empty() ? json() : read_json_file(cp_list);
        if (parsed.is_object() && parsed.contains("printerList") && parsed["printerList"].is_array()) {
            list = parsed["printerList"];
            write_json_file(cache, list);
        }
    }
    if (!list.is_array()) {
        static const std::vector<std::pair<const char*, const char*>> known = {
            {"F008", "K2 Plus"}, {"F012", "K2 Pro"}, {"F021", "K2"}, {"F022", "SPARKX i7"}, {"K1", "K1"}, {"K1 SE", "K1 SE"}, {"K1C", "K1C"}};
        list = json::array();
        for (const auto& [code, name] : known)
            list.push_back({{"name", name}, {"printerIntName", code}, {"nozzleDiameter", {"0.4"}}, {"type", 1}});
    }

    // Pictures: Orca's own printer covers instead of Creality's CDN.
    const fs::path covers = fs::path(resources_dir()) / "profiles" / "Creality";
    json           out    = json::array();
    for (json item : list) {
        if (!item.is_object())
            continue;
        const std::string name  = item.value("name", std::string());
        const fs::path    cover = covers / ("Creality " + name + "_cover.png");
        item["thumbnail"]       = !name.empty() && fs::exists(cover) ? file_url_from_path(cover).ToStdString() : std::string();
        item.erase("zipUrl");
        out.push_back(std::move(item));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Per-user page state (card/list layout, custom slot colours)

static fs::path user_operation_state_path() { return fs::path(data_dir()) / "cache" / "creality_device" / "user_operation_state.json"; }

json CrealityDevicePanel::user_operation_state() const
{
    json state = read_json_file(user_operation_state_path());
    if (!state.is_object())
        state = json::object();
    if (state.value("deviceLayout", std::string()) != "list")
        state["deviceLayout"] = "card";
    if (!state.contains("customColorList") || !state["customColorList"].is_array())
        state["customColorList"] = json::array();
    return state;
}

bool CrealityDevicePanel::save_user_operation_state(const json& state) const { return write_json_file(user_operation_state_path(), state); }

void CrealityDevicePanel::route_set_user_custom_color_list(const json& j)
{
    const json payload = j.contains("data") ? j["data"] : j;
    const json* incoming = payload.is_array() ? &payload :
                           (payload.is_object() && payload.contains("customColorList") && payload["customColorList"].is_array()) ?
                                                &payload["customColorList"] :
                                                nullptr;
    auto slot_key = [](const json& item) {
        auto part = [&](const char* key) { return item.contains(key) ? item[key].dump() : std::string(); };
        return part("address") + "|" + part("boxType") + "|" + part("boxId") + "|" + part("materialId");
    };
    json state = user_operation_state();
    json& list = state["customColorList"];
    bool  ok   = false;
    if (incoming != nullptr) {
        for (const json& item : *incoming) {
            if (!item.is_object() || json_string(item, "address").empty() || json_string(item, "customColor").empty() ||
                !item.contains("boxType") || !item.contains("boxId") || !item.contains("materialId"))
                continue;
            const json normalized = {{"address", item["address"]}, {"boxType", item["boxType"]}, {"boxId", item["boxId"]},
                                     {"materialId", item["materialId"]}, {"customColor", item["customColor"]}};
            bool replaced = false;
            for (json& existing : list)
                if (slot_key(existing) == slot_key(normalized)) {
                    existing = normalized;
                    replaced = true;
                    break;
                }
            if (!replaced)
                list.push_back(normalized);
        }
        ok = save_user_operation_state(state);
    }
    post_command("set_user_custom_color_list", {{"success", ok}, {"customColorList", state["customColorList"]}});
}

std::string CrealityDevicePanel::system_id() const
{
    AppConfig* config = wxGetApp().app_config;
    std::string id    = config->get("creality_panel_system_id");
    if (id.empty()) {
        id = boost::uuids::to_string(boost::uuids::random_generator()());
        config->set("creality_panel_system_id", id);
    }
    return id;
}

// ---------------------------------------------------------------------------------------------
// LAN probing

void CrealityDevicePanel::route_probe_lan_device(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    if (request_id.empty() || request_id.size() > 128) {
        post_command("probe_lan_device_result", InvalidProbeRequest(request_id, "requestId is required"));
        return;
    }
    const std::string address     = json_string(j, "address");
    const std::string policy_name = j.value("policy", std::string("prefer_secure"));
    ProbePolicy       policy;
    if (policy_name == "prefer_secure")
        policy = ProbePolicy::PreferSecure;
    else if (policy_name == "secure_only")
        policy = ProbePolicy::SecureOnly;
    else if (policy_name == "http_only")
        policy = ProbePolicy::HttpOnly;
    else {
        post_command("probe_lan_device_result", InvalidProbeRequest(request_id, "unknown probe policy"));
        return;
    }
    if (!is_lan_ipv4(address)) {
        json response         = InvalidProbeRequest(request_id, "only LAN addresses are probed");
        response["errorType"] = "invalid_address";
        post_command("probe_lan_device_result", std::move(response));
        return;
    }
    std::weak_ptr<Bridge> weak = m_bridge;
    submit_lan_device_probe_task([weak, request_id, address, policy] {
        const auto result   = LanDeviceProbe().probe(address, policy);
        json       response = result.to_json();
        response["requestId"] = request_id;
        if (result.ok())
            remember_host(address);
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([weak, response = std::move(response)]() mutable {
                if (auto bridge = weak.lock(); bridge && bridge->panel)
                    bridge->panel->post_command("probe_lan_device_result", std::move(response));
            });
    });
}

void CrealityDevicePanel::route_get_printer_https_info(const json& j)
{
    std::string url = json_string(j, "url");
    const std::string address = url.rfind("https://", 0) == 0 ? host_of_url(url) : std::string();
    if (!is_lan_ipv4(address)) {
        post_command("get_printer_https_info", InvalidProbeRequest(std::string(), "only LAN addresses are probed"));
        return;
    }
    std::weak_ptr<Bridge> weak = m_bridge;
    submit_lan_device_probe_task([weak, address] {
        ProbeOptions options;
        options.https_timeout_seconds = 6;
        const auto result = LanDeviceProbe().probe(address, ProbePolicy::SecureOnly, options);
        if (result.ok())
            remember_host(address);
        json response = result.to_json();
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([weak, response = std::move(response)]() mutable {
                if (auto bridge = weak.lock(); bridge && bridge->panel)
                    bridge->panel->post_command("get_printer_https_info", std::move(response));
            });
    });
}

void CrealityDevicePanel::route_scan_device(const json& j)
{
    const std::string request_id = json_string(j, "requestId");
    if (m_scan_running) {
        json busy = {{"status", 1}, {"errorType", "scan_busy"}, {"servers", json::array()}, {"serverInfos", json::array()},
                     {"boxs", json::array()}, {"klipper", json::array()}};
        if (!request_id.empty())
            busy["requestId"] = request_id;
        post_command("scan_device", std::move(busy));
        return;
    }
    m_scan_running              = true;
    std::weak_ptr<Bridge> weak = m_bridge;
    std::thread([weak, request_id] {
        auto post = [weak](const std::string& command, json data, bool done) {
            if (wxTheApp != nullptr)
                wxTheApp->CallAfter([weak, command, data = std::move(data), done]() mutable {
                    if (auto bridge = weak.lock(); bridge && bridge->panel) {
                        if (done)
                            bridge->panel->m_scan_running = false;
                        bridge->panel->post_command(command, std::move(data));
                    }
                });
        };
        std::vector<std::string> ips, box_ips, klipper_ips;
        try {
            for (const auto& item : cxnet::syncDiscoveryService({"CXSWBox", "creality", "Creality"})) {
                if (!is_lan_ipv4(item.machineIp))
                    continue;
                if (item.answer.rfind("_CXSWBox", 0) == 0) {
                    box_ips.push_back(item.machineIp);
                    continue;
                }
                // "_creality<type:2><moonraker port:4>..." announces a Klipper host.
                if (item.answer.size() > 15 && item.answer.rfind("_creality00", 0) == 0 &&
                    std::all_of(item.answer.begin() + 9, item.answer.begin() + 15, [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    klipper_ips.push_back(item.machineIp + ":" + item.answer.substr(11, 4));
                    continue;
                }
                if (std::find(ips.begin(), ips.end(), item.machineIp) == ips.end())
                    ips.push_back(item.machineIp);
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Creality panel: LAN discovery failed: " << e.what();
        }
        post("scan_device_verifying", request_id.empty() ? json("") : json{{"requestId", request_id}}, false);

        ProbeOptions options;
        options.preflight_ports       = true;
        options.https_timeout_seconds = 2;
        options.http_timeout_seconds  = 3;
        const auto results            = LanDeviceProbe().probe_many(ips, ProbePolicy::PreferSecure, options);
        json       infos              = json::array();
        for (const auto& result : results)
            if (result.ok()) {
                remember_host(result.profile->address);
                infos.push_back(result.profile->to_json());
            }
        json data = {{"status", 0}, {"servers", ips}, {"serverInfos", infos}, {"boxs", box_ips}, {"klipper", klipper_ips}};
        if (!request_id.empty())
            data["requestId"] = request_id;
        post("scan_device", std::move(data), true);
    }).detach();
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
// Printer capabilities shown in the panel (LED, chamber heater, side fan, filtration).

void CrealityDevicePanel::route_req_device_move_direction(const json& j)
{
    static const json caps_table = read_json_file(fs::path(resources_dir()) / "web" / "crealityDevice" / "machine_caps.json");
    const std::string preset_name = json_string(j, "preset_name");
    const std::string address     = json_string(j, "address");
    static const char* keys[]     = {"machine_platform_motion_enable", "machine_LED_light_exist", "auxiliary_fan", "support_air_filtration",
                                     "machine_ptc_exist"};
    std::map<std::string, int> caps;
    for (const char* key : keys)
        caps[key] = 0;
    bool found = false;
    if (caps_table.is_object() && caps_table.contains(preset_name)) {
        for (const char* key : keys)
            caps[key] = caps_table[preset_name].value(key, 0);
        found = true;
    } else if (PresetBundle* bundle = wxGetApp().preset_bundle) {
        if (const Preset* preset = bundle->printers.find_preset(preset_name)) {
            for (const char* key : keys)
                if (const ConfigOption* option = preset->config.option(key))
                    caps[key] = option->getBool() ? 1 : 0;
            found = true;
        }
    }
    if (!found)
        BOOST_LOG_TRIVIAL(info) << "Creality panel: no capabilities known for " << preset_name;
    post_command("req_device_move_direction", {{"direction", caps["machine_platform_motion_enable"]},
                                               {"machine_LED_light_exist", caps["machine_LED_light_exist"]},
                                               {"auxiliary_fan", caps["auxiliary_fan"]},
                                               {"support_air_filtration", caps["support_air_filtration"]},
                                               {"machine_ptc_exist", caps["machine_ptc_exist"]},
                                               {"address", address}});
}

void CrealityDevicePanel::route_get_model_match(const json& j)
{
    const std::string a = json_string(j, "modelA");
    const std::string b = json_string(j, "modelB");
    int               result = 0;
    if (a != b) {
        auto model_of = [](const std::string& preset_name, const std::string& fallback) {
            std::string model = fallback;
            if (PresetBundle* bundle = wxGetApp().preset_bundle)
                if (const Preset* preset = bundle->printers.find_preset(preset_name); preset && preset->config.has("printer_model"))
                    model = preset->config.opt_string("printer_model");
            boost::algorithm::trim(model);
            if (!model.empty() && !boost::istarts_with(model, "Creality") && !boost::istarts_with(model, "SPARKX"))
                model = "Creality " + model;
            return model;
        };
        const std::string ma = model_of(a, json_string(j, "printerModelA"));
        const std::string mb = model_of(b, json_string(j, "printerModelB"));
        result               = !ma.empty() && !mb.empty() && boost::iequals(ma, mb) ? 0 : 1;
    }
    run_script("window.handleStudioCmd('" + url_encode(json{{"command", "get_model_match"}, {"result", result}}.dump()) + "');");
}

// ---------------------------------------------------------------------------------------------

void CrealityDevicePanel::register_routes()
{
    auto& h = m_handlers;
    auto ignore = [](const json&) {};

    // --- system (SysRoutes) ---
    h["is_dark_theme"] = [this](const json&) { post_command("is_dark_theme", wxGetApp().dark_mode()); };
    h["get_lang"]      = [this](const json&) { post_command("get_lang", frontend_language_code()); };
    h["get_region"]    = [this](const json&) { post_command("get_region", wxGetApp().app_config->get_region()); };
    h["get_system_id"] = [this](const json&) { post_command("get_system_id", system_id()); };
    // Creality Cloud is not used: always signed out.
    h["get_user"] = [this](const json&) {
        post_command("get_user", {{"bLogin", 0}, {"token", ""}, {"userId", ""}, {"region", wxGetApp().app_config->get_region()},
                                  {"pid", Slic3r::get_current_pid()}});
    };
    h["common_openurl"] = [](const json& j) {
        const std::string url = json_string(j, "url");
        if (boost::istarts_with(url, "http://") || boost::istarts_with(url, "https://"))
            wxLaunchDefaultBrowser(wxString::FromUTF8(url));
    };
    h["switch_to_tab"] = [](const json& j) {
        const std::string tab = boost::algorithm::to_lower_copy(json_string(j, "tabName"));
        MainFrame*        frame = wxGetApp().mainframe;
        if (frame == nullptr)
            return;
        if (tab == "home")
            frame->request_select_tab(TAB_ID_HOME);
        else if (tab == "prepare" || tab == "slice" || tab == "3d")
            frame->request_select_tab(TAB_ID_PREPARE);
        else if (tab == "preview")
            frame->request_select_tab(TAB_ID_PREVIEW);
        else
            BOOST_LOG_TRIVIAL(info) << "Creality panel: no Orca tab for " << tab;
    };
    h["get_machine_list"] = [this](const json&) { post_command("get_machine_list", machine_list()); };

    // --- device list (DeviceMgrRoutes) ---
    h["init_device"]   = [this](const json& j) { route_init_device(j); };
    h["update_devices"] = [this](const json& j) {
        if (j.contains("data"))
            m_latest_devices = j["data"];
    };
    h["request_all_device"] = h["get_devices"] = [this](const json& j) {
        post_command(json_string(j, "command"), m_latest_devices.is_null() ? json::object() : m_latest_devices);
    };
    h["set_current_device"] = [](const json& j) {
        std::string mac = j.value("device_id", j.value("mac", std::string()));
        if (!mac.empty())
            DeviceStore::Ins().SetCurrentDevice(mac);
    };
    h["get_current_device"] = [this](const json&) {
        const std::string mac = DeviceStore::Ins().GetCurrentDevice();
        if (!mac.empty())
            post_command("get_current_device", {{"mac", mac}});
    };
    h["get_device_merge_state"] = [this](const json&) { post_command("get_device_merge_state", DeviceStore::Ins().IsMergeState()); };
    h["set_device_merge_state"] = [](const json& j) {
        if (j.contains("state") && j["state"].is_boolean())
            DeviceStore::Ins().SetMergeState(j["state"].get<bool>());
    };
    h["add_group"]              = [](const json& j) { DeviceStore::Ins().AddGroup(json_string(j, "group")); };
    h["remove_group"]           = [](const json& j) { DeviceStore::Ins().RemoveGroup(json_string(j, "name")); };
    h["edit_device_group_name"] = [](const json& j) { DeviceStore::Ins().EditGroupName(json_string(j, "oldName"), json_string(j, "newName")); };
    h["edit_device_name"]       = [](const json& j) { DeviceStore::Ins().EditDeiveName(json_string(j, "address"), json_string(j, "name")); };
    h["remove_device"]          = [](const json& j) { DeviceStore::Ins().RemoveDevice(json_string(j, "address")); };
    h["remove_to_first"]        = [](const json& j) { DeviceStore::Ins().remove2FirstGroup(json_string(j, "name")); };
    h["move_to_group"]          = [](const json& j) {
        DeviceStore::Ins().move2Group(json_string(j, "originGroup"), json_string(j, "targetGroup"), json_string(j, "address"));
    };
    h["sort_group"] = [](const json& j) {
        const json order = json::parse(json_string(j, "groupMap"), nullptr, false);
        if (!order.is_array())
            return;
        std::vector<std::string> groups;
        for (const auto& name : order)
            if (name.is_string())
                groups.push_back(name.get<std::string>());
        DeviceStore::Ins().sortGroup(groups);
    };
    auto add_device = [](const json& j, int kind) {
        DeviceStore::Data device{};
        device.address     = json_string(j, "address");
        device.mac         = json_string(j, "mac");
        device.model       = json_string(j, "model");
        device.connectType = j.value("type", 3);
        device.hostType    = 0;
        device.oldPrinter  = j.value("oldPrinter", false);
        if (kind == 0) {
            device.secureConnection = j.value("secureConnection", false);
            device.wssPort          = j.value("wssPort", 0);
            device.videoPort        = j.value("videoPort", 0);
        } else if (kind == 1) {
            device.moonrakerPort = j.value("moonrakerPort", 0);
            device.fluiddPort    = j.value("fluiddPort", 0);
            device.mainsailPort  = j.value("mainsailPort", 0);
        } else {
            device.oldPrinter           = false;
            device.deviceUI             = json_string(j, "deviceUI");
            device.apiKey               = json_string(j, "apiKey");
            device.hostType             = j.value("hostType", 0);
            device.caFile               = json_string(j, "caFile");
            device.ignoreCertRevocation = j.value("ignoreCertRevocation", false);
        }
        remember_host(host_of_url(device.address));
        DeviceStore::Ins().AddDevice(json_string(j, "group"), device);
    };
    h["add_device"]         = [add_device](const json& j) { add_device(j, 0); };
    h["add_device_klipper"] = [add_device](const json& j) { add_device(j, 1); };
    h["add_device_fluidd"]  = [add_device](const json& j) { add_device(j, 2); };
    h["update_device"]      = [this](const json& j) { route_update_device(j); };
    h["reconcile_device_ownership_batch"] = [this](const json& j) { route_reconcile_device_ownership_batch(j); };
    h["probe_lan_device"]                 = [this](const json& j) { route_probe_lan_device(j); };
    h["get_printer_https_info"]           = [this](const json& j) { route_get_printer_https_info(j); };
    h["scan_device"]                      = [this](const json& j) { route_scan_device(j); };
    h["request_device_address_correction"] = [this](const json&) {
        // Creality Print re-runs mDNS here; the page probes stored addresses itself afterwards.
        post_command("device_address_correction_result", {{"accepted", true}, {"completed", true}, {"busy", false}, {"corrections", 0}});
    };
    h["get_webrtc_local_param"]    = [this](const json& j) { route_get_webrtc_local_param(j); };
    h["req_device_move_direction"] = [this](const json& j) { route_req_device_move_direction(j); };
    h["get_model_match"]           = [this](const json& j) { route_get_model_match(j); };
    h["test_fluidd_device"]        = [this](const json&) {
        run_script("window.handleStudioCmd('" +
                   url_encode(json{{"command", "test_fluidd_device_status"}, {"result", 0}, {"info", "Not supported in OrcaSlicer"}}.dump()) +
                   "');");
    };
    h["sync_mappinp_cfs_filament"] = [](const json&) {
        // The panel's "sync to slicer": Orca's sidebar sync with the connected printer.
        wxGetApp().CallAfter([] {
            if (wxGetApp().plater() != nullptr)
                wxGetApp().sidebar().sync_ams_list(true);
        });
    };

    // --- page state ---
    h["request_user_operation_state"] = [this](const json&) { post_command("request_user_operation_state", user_operation_state()); };
    h["save_user_operation_state"]    = [this](const json& j) {
        const json payload = j.contains("data") ? j["data"] : j;
        std::string layout = payload.is_object() ? payload.value("deviceLayout", std::string("card")) :
                             payload.is_string() ? payload.get<std::string>() : std::string("card");
        json state            = user_operation_state();
        state["deviceLayout"] = layout == "list" ? "list" : "card";
        const bool ok         = save_user_operation_state(state);
        post_command("save_user_operation_state", {{"success", ok}, {"deviceLayout", state["deviceLayout"]}});
    };
    h["get_user_custom_color_list"] = [this](const json&) {
        post_command("get_user_custom_color_list", {{"customColorList", user_operation_state()["customColorList"]}});
    };
    h["set_user_custom_color_list"] = [this](const json& j) { route_set_user_custom_color_list(j); };

    // --- cloud, analytics and Creality Print-only features: accepted and dropped ---
    for (const char* command : {"set_device_detail_state", "viewDetialEvent", "print_video_connect", "device_detail_print",
                                "track_learn_about_cfs", "buy_filament_cmd", "set_cloud_device_mqtt", "get_printer_progress",
                                "diagnosis_close_cmd", "forward_device_detail", "set_current_plate_index", "start_time_lapse_share",
                                "cancel_time_lapse_share", "open_login_dialog"})
        h[command] = ignore;
}

// ---------------------------------------------------------------------------------------------
// Ported verbatim (apart from names) from Creality Print's DeviceMgrRoutes.

void CrealityDevicePanel::route_update_device(const json& j)
{
    std::string request_id;
    std::string session_id;
    const bool valid_request_id = j.contains("requestId") &&
        j.at("requestId").is_string() &&
        !j.at("requestId").get_ref<const std::string&>().empty() &&
        j.at("requestId").get_ref<const std::string&>().size() <= 128;
    if (valid_request_id)
        request_id = j.at("requestId").get<std::string>();

    auto post_response = [&](nlohmann::json response) {
        if (!valid_request_id)
            return;
        response["requestId"] = request_id;
        if (!session_id.empty())
            response["sessionId"] = session_id;
        post_command("update_device_result", std::move(response));
    };
    auto post_invalid = [&](const std::string& detail, const std::string& code = "invalid_request") {
        post_response({
            {"status", 1},
            {"ok", false},
            {"code", code},
            {"changed", false},
            {"errorType", code},
            {"error", detail}
        });
    };

    const bool has_expected_address = j.contains("expectedAddress");
    const bool has_expected_revision = j.contains("expectedAddressRevision");
    const bool cas_requested = has_expected_address || has_expected_revision;
    if (cas_requested && !valid_request_id) {
        BOOST_LOG_TRIVIAL(warning) << "update_device CAS rejected: valid requestId is required";
        return;
    }

    bool allow_secure_downgrade = false;
    if (j.contains("allowSecureDowngrade")) {
        if (!j.at("allowSecureDowngrade").is_boolean()) {
            post_invalid("allowSecureDowngrade must be a boolean");
            return;
        }
        allow_secure_downgrade = j.at("allowSecureDowngrade").get<bool>();
    }

    if (j.contains("sessionId")) {
        if (!j.at("sessionId").is_string() ||
            j.at("sessionId").get_ref<const std::string&>().size() > 128) {
            post_invalid("sessionId must be a string of at most 128 characters");
            return;
        }
        session_id = j.at("sessionId").get<std::string>();
    }

    try {
        DeviceStore::UpdatePatch patch;
        std::string new_mac;
        std::string lookup_mac;
        auto read_int = [&](const char* field, int& value) {
            if (!j.contains(field) || !j.at(field).is_number_integer())
                return false;
            const auto& number = j.at(field);
            if (number.is_number_unsigned()) {
                const std::uint64_t unsigned_value = number.get<std::uint64_t>();
                if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                    return false;
                value = static_cast<int>(unsigned_value);
                return true;
            }
            const std::int64_t signed_value = number.get<std::int64_t>();
            if (signed_value < std::numeric_limits<int>::min() ||
                signed_value > std::numeric_limits<int>::max())
                return false;
            value = static_cast<int>(signed_value);
            return true;
        };

        if (j.contains("mac")) {
            if (!j.at("mac").is_string()) {
                post_invalid("mac must be a string");
                return;
            }
            new_mac = j.at("mac").get<std::string>();
            if (!new_mac.empty())
                patch.mac = new_mac;
        }
        lookup_mac = new_mac;
        if (j.contains("lookupMac")) {
            if (!j.at("lookupMac").is_string()) {
                post_invalid("lookupMac must be a string");
                return;
            }
            lookup_mac = j.at("lookupMac").get<std::string>();
        }
        if (j.contains("lookupAddress")) {
            if (!j.at("lookupAddress").is_string()) {
                post_invalid("lookupAddress must be a string");
                return;
            }
            patch.lookupAddress = j.at("lookupAddress").get<std::string>();
        }
        if (j.contains("address")) {
            if (!j.at("address").is_string()) {
                post_invalid("address must be a string");
                return;
            }
            patch.address = j.at("address").get<std::string>();
        }
        if (j.contains("model")) {
            if (!j.at("model").is_string()) {
                post_invalid("model must be a string");
                return;
            }
            patch.model = j.at("model").get<std::string>();
        }
        if (j.contains("type")) {
            int connect_type = 0;
            if (!read_int("type", connect_type)) {
                post_invalid("type must be an integer");
                return;
            }
            patch.connectType = connect_type;
        }
        if (j.contains("secureConnection")) {
            if (!j.at("secureConnection").is_boolean()) {
                post_invalid("secureConnection must be a boolean");
                return;
            }
            patch.connection.secureConnection = j.at("secureConnection").get<bool>();
        }
        if (j.contains("wssPort")) {
            int wss_port = 0;
            if (!read_int("wssPort", wss_port)) {
                post_invalid("wssPort must be an integer");
                return;
            }
            patch.connection.wssPort = wss_port;
        }
        if (j.contains("videoPort")) {
            int video_port = 0;
            if (!read_int("videoPort", video_port)) {
                post_invalid("videoPort must be an integer");
                return;
            }
            patch.connection.videoPort = video_port;
        }

        if (cas_requested) {
            if (!has_expected_address || !has_expected_revision) {
                post_invalid("expectedAddress and expectedAddressRevision must be provided together");
                return;
            }
            if (!j.at("expectedAddress").is_string()) {
                post_invalid("expectedAddress must be a string");
                return;
            }
            if (!j.at("expectedAddressRevision").is_number_integer()) {
                post_invalid("expectedAddressRevision must be a non-negative integer");
                return;
            }
            if (!patch.address) {
                post_invalid("address is required for a CAS update");
                return;
            }

            DeviceStore::AddressUpdateExpectation expectation;
            expectation.expectedAddress = j.at("expectedAddress").get<std::string>();
            const auto& revision_json = j.at("expectedAddressRevision");
            if (revision_json.is_number_unsigned()) {
                expectation.expectedAddressRevision = revision_json.get<std::uint64_t>();
            } else {
                const std::int64_t signed_revision = revision_json.get<std::int64_t>();
                if (signed_revision < 0) {
                    post_invalid("expectedAddressRevision must be non-negative");
                    return;
                }
                expectation.expectedAddressRevision =
                    static_cast<std::uint64_t>(signed_revision);
            }
            const auto result = DeviceStore::Ins().UpdateDevicePatchCas(
                lookup_mac,
                patch,
                expectation,
                allow_secure_downgrade);
            const bool ok = UpdateDeviceSucceeded(result.outcome);
            const std::string code = UpdateDeviceCode(result.outcome);
            nlohmann::json response = {
                {"status", ok ? 0 : 1},
                {"ok", ok},
                {"code", code},
                {"changed", result.changed},
                {"expectedAddress", expectation.expectedAddress},
                {"expectedAddressRevision", expectation.expectedAddressRevision}
            };
            if (result.device) {
                response["mac"] = result.device->mac;
                response["address"] = result.device->address;
                response["addressRevision"] = result.device->addressRevision;
            }
            if (!result.error.empty()) {
                response["errorType"] = code;
                response["error"] = result.error;
            }
            post_response(std::move(response));
            return;
        }

        const bool updated = DeviceStore::Ins().UpdateDevicePatch(
            lookup_mac, patch, allow_secure_downgrade);
        if (!updated)
            BOOST_LOG_TRIVIAL(warning) << "update_device rejected for lookup mac: " << lookup_mac;

        if (valid_request_id) {
            std::optional<DeviceStore::Data> current_device;
            if (updated) {
                const std::string result_mac = patch.mac.value_or(lookup_mac);
                if (!result_mac.empty())
                    current_device = DeviceStore::Ins().FindByMac(result_mac);
                if (!current_device) {
                    const std::string result_address = patch.address.value_or(
                        patch.lookupAddress.value_or(std::string()));
                    if (!result_address.empty())
                        current_device = DeviceStore::Ins().FindByAddress(result_address);
                }
            }

            nlohmann::json response = {
                {"status", updated ? 0 : 1},
                {"ok", updated},
                {"code", updated ? "updated" : "rejected"},
                {"changed", updated}
            };
            if (current_device) {
                response["mac"] = current_device->mac;
                response["address"] = current_device->address;
                response["addressRevision"] = current_device->addressRevision;
            }
            post_response(std::move(response));
        }
    } catch (const std::exception& e) {
        post_invalid(e.what(), cas_requested ? "invalid_request" : "rejected");
    } catch (...) {
        post_invalid("unexpected update_device failure", "internal_error");
    }
    return;
}

void CrealityDevicePanel::route_reconcile_device_ownership_batch(const json& j)
{
    std::string request_id;
    std::string session_id;

    auto post_invalid = [&](const std::string& detail, const std::string& code = "invalid_request") {
        nlohmann::json response = {
            {"requestId", request_id},
            {"status", 1},
            {"ok", false},
            {"code", code},
            {"changed", false},
            {"snapshot", nlohmann::json::array()},
            {"errorType", code},
            {"error", detail}
        };
        if (!session_id.empty())
            response["sessionId"] = session_id;
        post_command(
            "reconcile_device_ownership_batch_result",
            std::move(response));
    };

    try {
        if (!j.is_object()) {
            post_invalid("request data must be an object");
            return;
        }
        if (!j.contains("requestId") || !j.at("requestId").is_string()) {
            post_invalid("requestId is required and must be a string");
            return;
        }
        request_id = j.at("requestId").get<std::string>();
        if (request_id.empty() || request_id.size() > 128) {
            post_invalid("requestId must contain 1 to 128 characters");
            return;
        }

        if (j.contains("sessionId")) {
            if (!j.at("sessionId").is_string()) {
                post_invalid("sessionId must be a string");
                return;
            }
            session_id = j.at("sessionId").get<std::string>();
            if (session_id.size() > 128) {
                post_invalid("sessionId must not exceed 128 characters");
                return;
            }
        }

        if (!j.contains("expectedRecords") ||
            !j.at("expectedRecords").is_array() ||
            j.at("expectedRecords").size() > 4096) {
            post_invalid("expectedRecords must be an array with at most 4096 records");
            return;
        }
        if (!j.contains("probes") ||
            !j.at("probes").is_array() ||
            j.at("probes").size() > 1024) {
            post_invalid("probes must be an array with at most 1024 records");
            return;
        }

        auto read_non_negative = [](const nlohmann::json& value, std::uint64_t& result) {
            if (!value.is_number_integer())
                return false;
            if (value.is_number_unsigned()) {
                result = value.get<std::uint64_t>();
                return true;
            }
            const std::int64_t signed_value = value.get<std::int64_t>();
            if (signed_value < 0)
                return false;
            result = static_cast<std::uint64_t>(signed_value);
            return true;
        };
        auto read_port = [&](const nlohmann::json& value, int& port) {
            std::uint64_t parsed = 0;
            if (!read_non_negative(value, parsed) || parsed > 65535)
                return false;
            port = static_cast<int>(parsed);
            return true;
        };

        DeviceStore::DeviceOwnershipBatchRequest request;
        for (const auto& record_json : j.at("expectedRecords")) {
            if (!record_json.is_object() ||
                !record_json.contains("rawIndex") ||
                !record_json.contains("group") || !record_json.at("group").is_string() ||
                !record_json.contains("mac") || !record_json.at("mac").is_string() ||
                !record_json.contains("address") || !record_json.at("address").is_string() ||
                !record_json.contains("addressRevision")) {
                post_invalid(
                    "each expected record requires rawIndex, group, mac, address and addressRevision");
                return;
            }

            std::uint64_t raw_index = 0;
            std::uint64_t address_revision = 0;
            if (!read_non_negative(record_json.at("rawIndex"), raw_index) ||
                raw_index > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                post_invalid("rawIndex must be a non-negative integer");
                return;
            }
            if (!read_non_negative(record_json.at("addressRevision"), address_revision)) {
                post_invalid("addressRevision must be a non-negative integer");
                return;
            }

            DeviceStore::DeviceOwnershipExpectedRecord expected;
            expected.rawIndex = static_cast<std::size_t>(raw_index);
            expected.group = record_json.at("group").get<std::string>();
            expected.mac = record_json.at("mac").get<std::string>();
            expected.address = record_json.at("address").get<std::string>();
            expected.addressRevision = address_revision;
            request.expectedRecords.push_back(std::move(expected));
        }

        for (const auto& probe_json : j.at("probes")) {
            if (!probe_json.is_object() ||
                !probe_json.contains("address") || !probe_json.at("address").is_string() ||
                !probe_json.contains("ok") || !probe_json.at("ok").is_boolean()) {
                post_invalid("each probe requires address and ok");
                return;
            }

            DeviceStore::DeviceOwnershipProbe probe;
            probe.address = probe_json.at("address").get<std::string>();
            probe.ok = probe_json.at("ok").get<bool>();
            if (probe.ok) {
                if (!probe_json.contains("observedMac") ||
                    !probe_json.at("observedMac").is_string() ||
                    !probe_json.contains("secureConnection") ||
                    !probe_json.at("secureConnection").is_boolean() ||
                    !probe_json.contains("wssPort") ||
                    !probe_json.contains("videoPort")) {
                    post_invalid(
                        "successful probes require observedMac, secureConnection, wssPort and videoPort");
                    return;
                }
                if (probe_json.contains("model") && !probe_json.at("model").is_string()) {
                    post_invalid("probe model must be a string");
                    return;
                }
                if (probe_json.contains("allowSecureDowngrade") &&
                    !probe_json.at("allowSecureDowngrade").is_boolean()) {
                    post_invalid("allowSecureDowngrade must be a boolean");
                    return;
                }
                probe.observedMac = probe_json.at("observedMac").get<std::string>();
                probe.model = probe_json.value("model", std::string());
                probe.secureConnection = probe_json.at("secureConnection").get<bool>();
                probe.allowSecureDowngrade =
                    probe_json.value("allowSecureDowngrade", false);
                if (!read_port(probe_json.at("wssPort"), probe.wssPort) ||
                    !read_port(probe_json.at("videoPort"), probe.videoPort)) {
                    post_invalid("probe ports must be between 0 and 65535");
                    return;
                }
            }
            request.probes.push_back(std::move(probe));
        }

        const auto result = DeviceStore::Ins().ReconcileDeviceOwnershipBatch(request);
        const bool ok = DeviceOwnershipBatchSucceeded(result.outcome);
        const std::string code = DeviceOwnershipBatchCode(result.outcome);
        nlohmann::json response = {
            {"requestId", request_id},
            {"status", ok ? 0 : 1},
            {"ok", ok},
            {"code", code},
            {"changed", result.changed},
            {"snapshot", nlohmann::json::array()}
        };
        if (!session_id.empty())
            response["sessionId"] = session_id;
        for (const auto& device : result.snapshot)
            response["snapshot"].push_back(ModernLanSnapshotToJson(device));
        if (!result.error.empty()) {
            response["errorType"] = code;
            response["error"] = result.error;
        }
        post_command(
            "reconcile_device_ownership_batch_result",
            std::move(response));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "reconcile_device_ownership_batch failed: " << e.what();
        post_invalid(e.what(), "internal_error");
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) <<
            "reconcile_device_ownership_batch failed with unknown error";
        post_invalid("unknown device ownership batch failure", "internal_error");
    }
    return;
}
}} // namespace Slic3r::GUI
