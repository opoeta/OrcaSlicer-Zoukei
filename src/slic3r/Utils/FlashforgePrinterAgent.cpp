// Native "flashforge" printer agent: ZMOD (Moonraker) and stock-firmware (HTTP :8898) Flashforge AD5X printers.
#include "FlashforgePrinterAgent.hpp"

#include "CrealityCfsAgent.hpp"
#include "CrealityPrintAgent.hpp"
#include "Http.hpp"
#include "Spoolman.hpp"
#include "bambu_networking.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/FlashforgeDevicePanel.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace Slic3r {

using json = nlohmann::json;

namespace {

constexpr const char* AGENT_VERSION = "1.0.0";
constexpr const char* STOCK_PORT    = "8898";

// "E91E63", "#e91e63", "0xE91E63" or "E91E63FF" -> "#E91E63"; "" when it is not a colour.
std::string norm_color(const std::string& raw)
{
    std::string c = raw;
    boost::trim(c);
    if (boost::istarts_with(c, "0x"))
        c = c.substr(2);
    else if (!c.empty() && c.front() == '#')
        c = c.substr(1);
    if (c.size() < 6)
        return {};
    c = c.substr(0, 6);
    for (char ch : c)
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return {};
    boost::to_upper(c);
    return "#" + c;
}

// IFS / ZMOD material names -> the base type Orca knows ("SILK" is a PLA variant).
std::string base_type(const std::string& material)
{
    std::string m = material;
    boost::trim(m);
    boost::to_upper(m);
    if (m.empty() || m == "?")
        return {};
    if (m == "SILK")
        return "PLA";
    if (m == "PLA-CF" || m == "PETG-CF")
        return m;
    return CrealityPrintAgent::normalize_filament_type(m);
}

std::string base64(const std::string& in)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    size_t             i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned n = (unsigned char) in[i] << 16 | (unsigned char) in[i + 1] << 8 | (unsigned char) in[i + 2];
        out += tbl[n >> 18 & 63];
        out += tbl[n >> 12 & 63];
        out += tbl[n >> 6 & 63];
        out += tbl[n & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned n = (unsigned char) in[i] << 16;
        out += tbl[n >> 18 & 63];
        out += tbl[n >> 12 & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned n = (unsigned char) in[i] << 16 | (unsigned char) in[i + 1] << 8;
        out += tbl[n >> 18 & 63];
        out += tbl[n >> 12 & 63];
        out += tbl[n >> 6 & 63];
        out += '=';
    }
    return out;
}

double num(const json& j, const char* key, double fallback = 0.)
{
    if (!j.is_object() || !j.contains(key))
        return fallback;
    const json& v = j[key];
    if (v.is_number())
        return v.get<double>();
    if (v.is_string())
        return std::atof(v.get<std::string>().c_str());
    return fallback;
}

std::string str(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

// "My model (1).gcode" -> "My_model__1_.gcode": the name ends up in a G-code command line on ZMOD.
std::string safe_name(std::string name)
{
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
            c = '_';
    if (name.empty() || name == "." || name == "..")
        name = "print.gcode";
    if (!boost::iends_with(name, ".gcode"))
        name += ".gcode";
    return name;
}

struct JobFile
{
    std::string path, name;
};

// The files of a job: extra_options {"files":[{"path","name"}]} from the print dialog, else the single G-code of the params.
std::vector<JobFile> job_files(const PrintParams& params)
{
    std::vector<JobFile> out;
    const json           extra = json::parse(params.extra_options, nullptr, false);
    if (extra.is_object() && extra.contains("files") && extra["files"].is_array())
        for (const json& f : extra["files"])
            out.push_back({str(f, "path"), safe_name(str(f, "name"))});
    if (out.empty()) {
        const std::string path = params.dst_file.empty() ? params.filename : params.dst_file;
        if (!path.empty())
            out.push_back({path, safe_name(boost::filesystem::path(path).filename().string())});
    }
    return out;
}

bool http_get(const std::string& url, const std::string& api_key, std::string& body, int timeout = 6)
{
    bool ok = false;
    auto http = Http::get(url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(3)
        .timeout_max(timeout)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            }
        })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    return ok;
}


// Runs a G-code script through Moonraker and waits for it (long timeout: lessWaste's MD5 check can take a while).
bool http_script(const std::string& base_url, const std::string& api_key, const std::string& script, int timeout = 60)
{
    bool ok   = false;
    auto http = Http::post(base_url + "/printer/gcode/script");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(json{{"script", script}}.dump())
        .timeout_connect(5)
        .timeout_max(timeout)
        .on_complete([&](std::string, unsigned status) { ok = status == 200; })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    return ok;
}

// POST {"namespace":"lane_data","key":key,"value":rec} to Moonraker's database.
bool post_lane_record(const std::string& base_url, const std::string& api_key, const std::string& key, const json& rec, std::string* error = nullptr)
{
    bool ok   = false;
    auto http = Http::post(base_url + "/server/database/item");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(json{{"namespace", "lane_data"}, {"key", key}, {"value", rec}}.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) { ok = status == 200; })
        .on_error([&](std::string, std::string err, unsigned status) {
            if (error != nullptr)
                *error = err + " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();
    return ok;
}

// "1.2.5" -> 10205 for comparing lessWaste versions.
int version_number(const std::string& v)
{
    int parts[3] = {0, 0, 0}, i = 0;
    std::stringstream ss(v);
    std::string       item;
    while (i < 3 && std::getline(ss, item, '.'))
        parts[i++] = std::atoi(item.c_str());
    return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

} // namespace

// ---- construction / registration ----------------------------------------------------------------

FlashforgePrinterAgent::FlashforgePrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

FlashforgePrinterAgent::~FlashforgePrinterAgent() { ++m_generation; }

AgentInfo FlashforgePrinterAgent::get_agent_info_static()
{
    return AgentInfo{FLASHFORGE_PRINTER_AGENT_ID, "Flashforge (LAN)", AGENT_VERSION,
                     "Flashforge AD5X over the LAN: ZMOD (Moonraker) or stock firmware, with IFS"};
}

std::string FlashforgePrinterAgent::current_model_id()
{
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    const Preset&     printer = bundle->printers.get_edited_preset();
    const std::string model   = printer.config.has("printer_model") ? printer.config.opt_string("printer_model") : std::string();
    if (model.empty())
        return {};
    for (const auto& [vendor_id, vendor] : bundle->vendors)
        for (const auto& printer_model : vendor.models)
            if (printer_model.name == model || printer_model.id == model)
                return printer_model.model_id;
    return {};
}

std::vector<FlashforgePrinterAgent::IfsSlot> FlashforgePrinterAgent::ifs_slots() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots;
}

std::string FlashforgePrinterAgent::camera_url() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_camera_url;
}

CameraStreamMode FlashforgePrinterAgent::get_camera_stream_mode() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_camera_snapshot_url.empty() ? CameraStreamMode::http_snapshot : !m_camera_url.empty() ? CameraStreamMode::http : CameraStreamMode::none;
}

std::string FlashforgePrinterAgent::get_camera_url() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_camera_snapshot_url.empty() ? m_camera_snapshot_url : m_camera_url;
}

std::string FlashforgePrinterAgent::make_device_name(const std::string& hostname) const
{
    // Both printers of the owner report the hostname "flashforge": the model is the useful name.
    return device_info.model_name.empty() ? hostname : device_info.model_name;
}

// ---- callbacks ----------------------------------------------------------------------------------

#define FF_SETTER(method, base_method, member, type)         \
    int FlashforgePrinterAgent::method(type fn)              \
    {                                                        \
        {                                                    \
            std::lock_guard<std::mutex> lock(m_mutex);       \
            member = fn;                                     \
        }                                                    \
        return MoonrakerPrinterAgent::base_method(std::move(fn)); \
    }

FF_SETTER(set_on_ssdp_msg_fn, set_on_ssdp_msg_fn, m_on_ssdp, OnMsgArrivedFn)
FF_SETTER(set_on_printer_connected_fn, set_on_printer_connected_fn, m_on_printer_connected, OnPrinterConnectedFn)
FF_SETTER(set_on_message_fn, set_on_message_fn, m_on_message, OnMessageFn)
FF_SETTER(set_on_local_connect_fn, set_on_local_connect_fn, m_on_local_connect, OnLocalConnectedFn)
FF_SETTER(set_on_local_message_fn, set_on_local_message_fn, m_on_local_message, OnMessageFn)
#undef FF_SETTER

int FlashforgePrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (fn) // Orca clears the callbacks of an agent it switches away from; keep the last valid hop
            m_queue_main = fn;
    }
    return MoonrakerPrinterAgent::set_queue_on_main_fn(std::move(fn));
}

void FlashforgePrinterAgent::run_on_main(std::function<void()> fn)
{
    QueueOnMainFn queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queue = m_queue_main;
    }
    if (queue)
        queue(std::move(fn));
    else
        fn();
}

void FlashforgePrinterAgent::publish(const json& message)
{
    OnMessageFn local_fn, cloud_fn;
    std::string dev_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        local_fn = m_on_local_message;
        cloud_fn = m_on_message;
        dev_id   = m_session.dev_id;
    }
    if (dev_id.empty() || (!local_fn && !cloud_fn))
        return;
    const std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    run_on_main([local_fn, cloud_fn, dev_id, text] {
        if (local_fn)
            local_fn(dev_id, text);
        else
            cloud_fn(dev_id, text);
    });
}

// ---- connection ---------------------------------------------------------------------------------

int FlashforgePrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    if (params.dev_id.empty() || params.host.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    const uint64_t generation = ++m_generation;
    m_mode                    = int(Mode::unresolved);
    Session session;
    session.generation = generation;
    session.host       = params.host;
    session.port       = params.port;
    session.use_ssl    = params.use_ssl;
    session.dev_id     = params.dev_id;
    // "SERIAL:CHECKCODE" in the API key field (stock firmware); a plain key is a Moonraker key.
    std::string key = params.password == "88888888" ? std::string() : params.password;
    if (const auto colon = key.find(':'); colon != std::string::npos) {
        session.serial     = key.substr(0, colon);
        session.check_code = key.substr(colon + 1);
        key.clear();
    }
    session.api_key = key;

    std::string model_id, model_name, nozzle;
    if (PresetBundle* bundle = GUI::wxGetApp().preset_bundle) { // UI thread: MachineObject::connect()
        model_id = current_model_id();
        const auto& cfg = bundle->printers.get_edited_preset().config;
        model_name      = cfg.opt_string("printer_model");
        if (session.serial.empty() && cfg.has("printhost_user"))
            session.serial = cfg.opt_string("printhost_user");
        if (const auto* nz = cfg.option<ConfigOptionFloats>("nozzle_diameter"); nz != nullptr && !nz->values.empty() && nz->values.front() > 0.)
            nozzle = (boost::format("%.1f") % nz->values.front()).str();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session            = session;
        m_model_id           = model_id;
        m_model_name         = model_name;
        m_nozzle_diameter    = nozzle;
        m_slots.clear();
        m_slot_signature.clear();
        m_camera_url.clear();
        m_camera_snapshot_url.clear();
        m_detail             = json();
        m_announced          = false;
        m_connected_reported = false;
    }
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    PrinterConnectionParams               copy = params;
    std::thread([weak, copy, generation] {
        if (auto self = weak.lock())
            self->resolve_mode(copy, generation);
    }).detach();
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::disconnect_printer()
{
    ++m_generation;
    m_mode = int(Mode::unresolved);
    m_ui   = int(Ui::unknown);
    return MoonrakerPrinterAgent::disconnect_printer();
}

// Worker thread: decide between ZMOD (Moonraker answers) and the stock API, then run the matching loop.
void FlashforgePrinterAgent::resolve_mode(const PrinterConnectionParams& params, uint64_t generation)
{
    std::string serial, check;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        serial = m_session.serial;
        check  = m_session.check_code;
    }
    std::vector<std::string> ports;
    if (!params.port.empty())
        ports.push_back(params.port);
    else {
        ports.push_back("7125");
        ports.push_back("");
    }
    std::string found_port;
    bool        moonraker = false;
    for (int attempt = 0; attempt < 2 && !moonraker && is_current(generation); ++attempt)
        for (const std::string& port : ports) {
            std::string body;
            if (http_get(normalize_base_url(params.use_ssl, params.host, port) + "/server/info", params.password == "88888888" ? "" : "", body, 4) &&
                body.find("klippy") != std::string::npos) {
                moonraker  = true;
                found_port = port;
                break;
            }
        }
    if (!is_current(generation))
        return;
    const bool stock = !moonraker && !serial.empty() && !check.empty();
    BOOST_LOG_TRIVIAL(info) << "Flashforge agent: " << params.host << " -> " << (moonraker ? "ZMOD/Moonraker port " + found_port : stock ? std::string("stock API") : std::string("no Moonraker, no stock credentials (defaulting to Moonraker)"));
    if (stock) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_session.base_url = normalize_base_url(false, params.host, STOCK_PORT);
            m_camera_url          = "http://" + params.host + ":8080/?action=stream";
            m_camera_snapshot_url = "http://" + params.host + ":8080/?action=snapshot";
        }
        m_mode = int(Mode::stock);
        stock_loop(generation);
        return;
    }
    PrinterConnectionParams p = params;
    if (moonraker)
        p.port = found_port;
    start_zmod(p, generation);
}

// ---- ZMOD -----------------------------------------------------------------------------------------

void FlashforgePrinterAgent::start_zmod(const PrinterConnectionParams& params, uint64_t generation)
{
    m_mode = int(Mode::zmod);
    const std::string key = params.password == "88888888" ? std::string() : params.password;
    const std::string base_url = normalize_base_url(params.use_ssl, params.host, params.port);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_zmod_url = base_url;
        m_zmod_key = key;
    }
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    PrinterConnectionParams               p    = params;
    // The Moonraker agent reads the printer preset: do its connect on the UI thread.
    run_on_main([weak, p, generation] {
        if (auto self = weak.lock(); self && self->is_current(generation))
            self->MoonrakerPrinterAgent::connect_printer(p);
    });
    zmod_loop(generation, base_url, key);
}

bool FlashforgePrinterAgent::fetch_zmod_slots(const std::string& base_url, const std::string& api_key, std::vector<IfsSlot>& slots)
{
    std::string body;
    if (!http_get(base_url + "/printer/objects/query?zmod_color&zmod_ifs&save_variables&print_stats&virtual_sdcard", api_key, body))
        return false;
    const json status = json::parse(body, nullptr, false);
    if (status.is_discarded() || !status.contains("result") || !status["result"].contains("status"))
        return false;
    const json& color = status["result"]["status"].value("zmod_color", json::object());
    if (!color.is_object() || !color.contains("slots") || !color["slots"].is_array())
        return false;

    // Which touchscreen UI runs? ZMOD's DISPLAY_OFF HELIX=1 / GUPPY=1 saves display_off=1 plus helix / guppy; DISPLAY_ON
    // (the original screen) only resets display_off, so "helix" alone proves nothing.
    {
        const json& st   = status["result"]["status"];
        const json  vars = st.contains("save_variables") && st["save_variables"].is_object() ? st["save_variables"].value("variables", json::object()) : json::object();
        Ui          ui   = Ui::unknown;
        if (vars.is_object() && vars.contains("display_off"))
            ui = num(vars, "display_off") != 0. ? (num(vars, "helix") != 0. ? Ui::helix : num(vars, "guppy") != 0. ? Ui::guppy : Ui::original) : Ui::original;
        if (int(ui) != m_ui.exchange(int(ui)))
            BOOST_LOG_TRIVIAL(info) << "Flashforge agent: " << base_url << " runs " << ui_name();
        const json ps = st.value("print_stats", json::object());
        const json vs = st.value("virtual_sdcard", json::object());
        std::string state = str(ps, "state");
        m_busy            = state == "printing" || state == "paused" || (vs.is_object() && vs.value("is_active", false));
        m_display_popup   = color.value("display", false);
        {   // the channel ZMOD feeds from (key names differ between ZMOD builds); 0 = unknown
            int chan = 0;
            const json zi = st.value("zmod_ifs", json::object());
            for (const json* o : {&zi, &color})
                for (const char* k : {"current_channel", "channel", "chan", "current_slot", "active_slot", "current"})
                    if (chan == 0 && o->is_object() && o->contains(k)) {
                        const json& v = (*o)[k];
                        const int   n = v.is_number() ? int(v.get<double>()) : v.is_string() ? std::atoi(v.get<std::string>().c_str()) : 0;
                        chan          = n >= 1 && n <= 4 ? n : 0;
                    }
            m_channel = chan;
        }
        std::vector<std::string> valid;
        if (color.contains("valid_types") && color["valid_types"].is_array())
            for (const json& t : color["valid_types"])
                if (t.is_string() && t.get<std::string>() != "?")
                    valid.push_back(t.get<std::string>());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_valid_types = std::move(valid);
    }

    json lanes = json::object(); // HelixScreen's spool database: vendor / product name / temperatures per lane
    if (http_get(base_url + "/server/database/item?namespace=lane_data", api_key, body)) {
        const json l = json::parse(body, nullptr, false);
        if (!l.is_discarded() && l.contains("result") && l["result"].contains("value") && l["result"]["value"].is_object())
            lanes = l["result"]["value"];
    }
    slots.clear();
    for (const json& s : color["slots"]) {
        IfsSlot slot;
        slot.port         = int(num(s, "ID", 0.)) ;
        if (slot.port == 0 && s.contains("ID") && s["ID"].is_string())
            slot.port = std::atoi(s["ID"].get<std::string>().c_str());
        if (slot.port < 1 || slot.port > 4)
            continue;
        slot.has_filament = s.value("hasFilament", false);
        slot.material     = str(s, "Material");
        slot.color        = norm_color(str(s, "HEX").empty() ? str(s, "Color") : str(s, "HEX"));
        const std::string key = "lane" + std::to_string(slot.port);
        if (lanes.contains(key) && lanes[key].is_object()) {
            const json& lane = lanes[key];
            // Only trust the database entry while it still describes what ZMOD sees in the slot.
            if (norm_color(str(lane, "color")) == slot.color) {
                if (lane.contains("multi_color_hexes") && lane["multi_color_hexes"].is_array()) {
                    std::vector<std::string> cs;
                    for (const json& c : lane["multi_color_hexes"])
                        if (c.is_string() && norm_color(c.get<std::string>()).size() == 7)
                            cs.push_back(norm_color(c.get<std::string>()));
                    if (cs.size() >= 2 && cs.size() <= 4 && cs.front() == slot.color) {
                        slot.colors   = std::move(cs);
                        slot.gradient = str(lane, "color_type") == "gradient";
                    }
                }
                slot.spool_id = int(num(lane, "spool_id")); // the Spoolman binding is shown whatever screen the printer has
                if (ui() != Ui::helix) {
                    slots.push_back(std::move(slot));
                    continue;
                }
                slot.name        = !str(lane, "helix_product_name").empty() ? str(lane, "helix_product_name") : str(lane, "name");
                slot.vendor      = !str(lane, "vendor_name").empty() ? str(lane, "vendor_name") : str(lane, "vendor");
                slot.nozzle_temp = int(num(lane, "nozzle_temp"));
                slot.bed_temp    = int(num(lane, "bed_temp"));
            }
        }
        slots.push_back(std::move(slot));
    }
    std::sort(slots.begin(), slots.end(), [](const IfsSlot& a, const IfsSlot& b) { return a.port < b.port; });
    return !slots.empty();
}

void FlashforgePrinterAgent::zmod_loop(uint64_t generation, std::string base_url, std::string api_key)
{
    bool first = true;
    while (is_current(generation)) {
        if (first) { // the camera: ZMOD serves the MJPEG stream through the web server (port 80) of the printer
            std::string body;
            if (http_get(base_url + "/server/webcams/list", api_key, body)) {
                const json cams = json::parse(body, nullptr, false);
                if (!cams.is_discarded() && cams.contains("result") && cams["result"].contains("webcams"))
                    for (const json& cam : cams["result"]["webcams"])
                        if (cam.value("enabled", false) && str(cam, "service").find("mjpeg") != std::string::npos) {
                            std::string host = base_url;
                            if (const auto scheme = host.find("://"); scheme != std::string::npos) {
                                const auto colon = host.find(':', scheme + 3);
                                if (colon != std::string::npos)
                                    host = host.substr(0, colon);
                            }
                            std::lock_guard<std::mutex> lock(m_mutex);
                            auto absolute = [&host](const std::string& u) { return u.rfind("http", 0) == 0 || u.empty() ? u : host + (u[0] == '/' ? "" : "/") + u; };
                            m_camera_url          = absolute(str(cam, "stream_url"));
                            m_camera_snapshot_url = absolute(str(cam, "snapshot_url"));
                            break;
                        }
            }
            // Spoolman: the printer's [spoolman] server fills the (empty) Preferences field.
            if (const std::string spoolman = Spoolman::discover_from_moonraker(base_url, api_key); !spoolman.empty())
                run_on_main([spoolman] {
                    AppConfig* cfg = GUI::wxGetApp().app_config;
                    if (cfg != nullptr && boost::trim_copy(cfg->get("spoolman_url")).empty()) {
                        cfg->set("spoolman_url", spoolman);
                        cfg->save();
                    }
                });
        }
        std::vector<IfsSlot> slots;
        if (fetch_zmod_slots(base_url, api_key, slots)) {
            sync_active_spool(base_url, api_key, slots);
            apply_zmod_slots(generation, std::move(slots));
        }
        else if (first) { // no zmod_color: other filament systems (Happy Hare, AFC...) keep the Moonraker agent's sync
            std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
            run_on_main([weak, generation] {
                if (auto self = weak.lock(); self && self->is_current(generation))
                    self->MoonrakerPrinterAgent::fetch_filament_info(self->device_info.dev_id);
            });
        }
        first = false;
        for (int i = 0; i < 50 && is_current(generation); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Moonraker's active Spoolman spool follows the IFS channel the printer feeds from: when that lane is bound to a spool
// (lane_data spool_id) and Moonraker's active spool differs, set it. Idle: whenever it differs; printing: only on a channel
// change (tool change). Runs only while Orca is connected; docs/zoukei/ifs-spoolman-macro.cfg does the same on the printer.
void FlashforgePrinterAgent::sync_active_spool(const std::string& base_url, const std::string& api_key, const std::vector<IfsSlot>& slots)
{
    const int chan = m_channel.load();
    if (chan < 1 || chan > 4 || m_writing)
        return;
    const IfsSlot* lane = nullptr;
    for (const IfsSlot& s : slots)
        if (s.port == chan)
            lane = &s;
    if (lane == nullptr || lane->spool_id <= 0)
        return;
    const std::string key = std::to_string(chan) + ":" + std::to_string(lane->spool_id);
    if (key == m_spool_key)
        return;
    std::string body;
    if (m_spoolman < 0) {
        m_spoolman = 0;
        if (http_get(base_url + "/server/config", api_key, body)) {
            const json c = json::parse(body, nullptr, false);
            if (!c.is_discarded() && c.contains("result") && c["result"].contains("config") && c["result"]["config"].contains("spoolman"))
                m_spoolman = 1;
        }
    }
    if (m_spoolman != 1)
        return;
    if (m_busy && !m_spool_key.empty() && m_spool_key.substr(0, m_spool_key.find(':')) == std::to_string(chan))
        return; // printing and no tool change: leave it
    int active = 0;
    if (http_get(base_url + "/server/spoolman/spool_id", api_key, body)) {
        const json r = json::parse(body, nullptr, false);
        if (!r.is_discarded() && r.contains("result") && r["result"].contains("spool_id") && r["result"]["spool_id"].is_number())
            active = r["result"]["spool_id"].get<int>();
    }
    m_spool_key = key;
    if (active == lane->spool_id)
        return;
    bool ok   = false;
    auto http = Http::post(base_url + "/server/spoolman/spool_id");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(json{{"spool_id", lane->spool_id}}.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) { ok = status == 200; })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    BOOST_LOG_TRIVIAL(info) << "Flashforge agent: active spool -> " << lane->spool_id << " (IFS" << chan << ")" << (ok ? "" : " FAILED");
    if (ok)
        CrealityCfsAgent::notify("Flashforge IFS" + std::to_string(chan) + ": active Spoolman spool set to #" + std::to_string(lane->spool_id));
}

void FlashforgePrinterAgent::apply_zmod_slots(uint64_t generation, std::vector<IfsSlot> slots)
{
    std::string signature;
    for (const IfsSlot& s : slots) {
        signature += std::to_string(s.port) + (s.has_filament ? "+" : "-") + s.material + s.color + s.name + s.vendor + std::to_string(s.spool_id) + (s.gradient ? "g" : "") + ";";
        for (const std::string& c : s.colors)
            signature += c;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (signature == m_slot_signature)
            return;
        m_slot_signature = signature;
        m_slots          = slots;
    }
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    run_on_main([weak, generation, slots] {
        auto self = weak.lock();
        if (!self || !self->is_current(generation))
            return;
        std::vector<AmsTrayData> trays;
        for (const IfsSlot& s : slots) {
            AmsTrayData tray;
            tray.slot_index   = s.port - 1;
            tray.has_filament = s.has_filament;
            if (s.has_filament) {
                tray.tray_type     = base_type(s.material);
                tray.tray_color    = s.color.empty() ? "#FFFFFF" : s.color;
                tray.tray_info_idx = self->preset_id_for(s);
                tray.bed_temp      = s.bed_temp;
                tray.nozzle_temp   = s.nozzle_temp;
            }
            trays.push_back(std::move(tray));
        }
        self->build_ams_payload(1, 3, trays);
    });
}

// Tool -> IFS port as ZMOD expects it ("_IFS_VARS tools=[port of T0, port of T1, ...16 entries]", 5 = unused), set
// right before the print is started; ZMOD's own dialog on the printer still shows the result.
void FlashforgePrinterAgent::send_zmod_mapping(const PrintParams& params)
{
    const json mapping = json::parse(params.ams_mapping_info, nullptr, false);
    if (!mapping.is_array() || mapping.empty())
        return;
    int  ports[16];
    std::fill(std::begin(ports), std::end(ports), 5);
    bool any = false, external = false;
    for (size_t i = 0; i < mapping.size() && i < 16; ++i) {
        const json& item = mapping[i];
        const int   tray = item.is_object() && item.contains("ams") && item["ams"].is_number_integer() ? item["ams"].get<int>() : -1;
        if (tray >= 0 && tray <= 3) {
            ports[i] = tray + 1;
            any      = true;
        } else if (tray == 254 || tray == 255) {
            external = true;
        }
    }
    if (!any && !external)
        return;
    if (any) {
        std::string tools = "_IFS_VARS tools=[";
        for (int i = 0; i < 16; ++i)
            tools += (i ? "," : "") + std::to_string(ports[i]);
        tools += "]";
        send_gcode_sync(device_info.dev_id, tools);
    }
    send_gcode_sync(device_info.dev_id, any ? "_IFS_VARS external=0" : "_IFS_VARS external=1");
}

// The ZMOD plugin lessWaste (github.com/Hrybmo/lessWaste, based on bambufy) is installed when its start-dialog macros exist.
bool FlashforgePrinterAgent::has_lesswaste(const std::string& base_url, const std::string& api_key) const
{
    std::string body;
    if (!http_get(base_url + "/printer/objects/list", api_key, body, 8))
        return false;
    return body.find("gcode_macro _IFS_COLORS_PRINT") != std::string::npos && body.find("gcode_macro _IFS_VARS\"") != std::string::npos &&
           body.find("gcode_macro _IFS_COLORS\"") != std::string::npos;
}

// Starts a print the way lessWaste's own start dialog does, minus the touches on the printer:
//  1. _ZSDCARD_PRINT_FILE FILENAME=f - lessWaste's entry (MD5 check, tools reset to T0->IFS1.., then its preprint.py reads the
//     G-code's filament colours / types / feed rates / first-layer box and calls _IFS_COLORS START=1 ..., which fills
//     _IFS_VARS colors / types / e_feedrates and opens the dialog);
//  2. wait for that dialog (_IFS_VARS.start == 1);
//  3. _IFS_VARS tools="[port of T0, ... 16 entries, 5 = unused]" external=0|1 - Orca's mapping replaces the default;
//  4. check the mapped ports hold filament and the "Change filament G-code" is new enough (the dialog's Print button demands both);
//  5. _IFS_COLORS_PRINT - the dialog's Print button: closes the prompt and starts the file.
bool FlashforgePrinterAgent::start_lesswaste(const PrintParams& params, const std::string& filename, WasCancelledFn cancel_fn, std::string& error)
{
    const std::string base = device_info.base_url, key = device_info.api_key;
    auto cancelled = [&] { return cancel_fn && cancel_fn(); };

    int  ports[16];
    std::fill(std::begin(ports), std::end(ports), 5);
    bool any = false, external = false;
    int  used_tools = 0;
    const json mapping = json::parse(params.ams_mapping_info, nullptr, false);
    if (mapping.is_array())
        for (size_t i = 0; i < mapping.size() && i < 16; ++i) {
            const json& item = mapping[i];
            const int   tray = item.is_object() && item.contains("ams") && item["ams"].is_number_integer() ? item["ams"].get<int>() : -1;
            if (tray >= 0 && tray <= 3) {
                ports[i] = tray + 1;
                any      = true;
                ++used_tools;
            } else if (tray == 254 || tray == 255) {
                external = true;
            }
        }

    http_script(base, key, "SET_GCODE_VARIABLE MACRO=_IFS_VARS VARIABLE=start VALUE=0"); // a cancelled earlier dialog leaves it at 1
    http_script(base, key, "_ZSDCARD_PRINT_FILE FILENAME=" + filename, 120); // the MD5 check can outlast the request: the dialog below is what counts
    std::string body;
    json        vars, colors;
    bool        dialog = false;
    for (int i = 0; i < 150 && !dialog; ++i) { // preprint.py scans the whole G-code: seconds for a big file
        if (cancelled())
            return false;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!http_get(base + "/printer/objects/query?gcode_macro%20_IFS_VARS&gcode_macro%20_IFS_COLORS", key, body, 6))
            continue;
        const json st = json::parse(body, nullptr, false);
        if (!st.is_object() || !st.contains("result"))
            continue;
        vars   = st["result"]["status"].value("gcode_macro _IFS_VARS", json::object());
        colors = st["result"]["status"].value("gcode_macro _IFS_COLORS", json::object());
        dialog = num(vars, "start") == 1. && str(colors, "filename") == filename;
    }
    if (!dialog) {
        error = "lessWaste did not open its print dialog for " + filename + " (cancel it on the printer and try again)";
        return false;
    }

    if (any) {
        std::string tools = "_IFS_VARS tools=\"[";
        for (int i = 0; i < 16; ++i)
            tools += (i ? "," : "") + std::to_string(ports[i]);
        tools += "]\"";
        if (!http_script(base, key, tools) || !http_script(base, key, "_IFS_VARS external=0")) {
            error = "could not set the tool-to-IFS mapping";
            return false;
        }
    } else if (external && !http_script(base, key, "_IFS_VARS external=1")) {
        error = "could not switch the IFS off";
        return false;
    }

    // What the dialog's Print button checks.
    if (used_tools > 1 && version_number(str(colors, "version")) < version_number(str(vars, "min_version"))) {
        error = "lessWaste needs a newer 'Change filament G-code' in the slicer profile (this file has v" + str(colors, "version") + ", needs v" + str(vars, "min_version") + ")";
        http_script(base, key, "_IFS_COLORS_CANCEL");
        return false;
    }
    if (any && http_get(base + "/printer/objects/query?filament_switch_sensor%20_ifs_port_sensor_1&filament_switch_sensor%20_ifs_port_sensor_2&filament_switch_sensor%20_ifs_port_sensor_3&filament_switch_sensor%20_ifs_port_sensor_4", key, body, 6)) {
        const json st = json::parse(body, nullptr, false);
        if (st.is_object() && st.contains("result"))
            for (int port = 1; port <= 4; ++port) {
                bool wanted = false;
                for (int i = 0; i < 16; ++i)
                    wanted = wanted || ports[i] == port;
                const json sensor = st["result"]["status"].value("filament_switch_sensor _ifs_port_sensor_" + std::to_string(port), json::object());
                if (wanted && sensor.contains("filament_detected") && sensor["filament_detected"].is_boolean() && !sensor["filament_detected"].get<bool>()) {
                    error = "IFS slot " + std::to_string(port) + " has no filament";
                    http_script(base, key, "_IFS_COLORS_CANCEL");
                    return false;
                }
            }
    }
    if (!http_script(base, key, "_IFS_COLORS_PRINT", 30)) {
        error = "the printer did not accept the print command";
        return false;
    }
    return true;
}

// Upload every file through Moonraker; with `start` the first one is printed (ZMOD asks for the colours on the printer).
int FlashforgePrinterAgent::zmod_job(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, bool start)
{
    auto report = [&](int stage, int code, const std::string& text) {
        if (update_fn)
            update_fn(stage, code, text);
    };
    auto fail = [&](const std::string& text, int code) {
        BOOST_LOG_TRIVIAL(error) << "Flashforge agent: " << text;
        report(PrintingStageERROR, -1, text);
        return code;
    };
    const auto files = job_files(params);
    if (files.empty())
        return fail("no G-code file to send", BAMBU_NETWORK_ERR_FILE_NOT_EXIST);
    for (const JobFile& f : files)
        if (!boost::filesystem::exists(boost::filesystem::path(f.path)))
            return fail("G-code file not found: " + f.path, BAMBU_NETWORK_ERR_FILE_NOT_EXIST);
    report(PrintingStageCreate, 0, "");
    for (size_t k = 0; k < files.size(); ++k) {
        if (cancel_fn && cancel_fn())
            return BAMBU_NETWORK_ERR_CANCELED;
        const size_t total = files.size();
        if (!upload_gcode(files[k].path, files[k].name, device_info.base_url, device_info.api_key,
                          [&](int stage, int code, const std::string& text) { report(stage, int((k * 100 + size_t(std::clamp(code, 0, 100))) / total), text); },
                          cancel_fn))
            return cancel_fn && cancel_fn() ? BAMBU_NETWORK_ERR_CANCELED : fail("upload failed", BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED);
    }
    if (start) {
        report(PrintingStageSending, 0, "");
        std::string lw_error;
        if (has_lesswaste(device_info.base_url, device_info.api_key)) {
            if (!start_lesswaste(params, files.front().name, cancel_fn, lw_error))
                return cancel_fn && cancel_fn() ? BAMBU_NETWORK_ERR_CANCELED : fail(lw_error, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED);
        } else {
            send_zmod_mapping(params);
            if (!send_gcode_sync(device_info.dev_id, "SDCARD_PRINT_FILE FILENAME=" + files.front().name))
                return fail("the printer did not accept the print command", BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED);
        }
    }
    report(PrintingStageFinished, 0, "3");
    return BAMBU_NETWORK_SUCCESS;
}

// ---- project -> printer (ZMOD) --------------------------------------------------------------------

std::string FlashforgePrinterAgent::ui_name() const
{
    switch (ui()) {
    case Ui::helix: return "ZMOD + HelixScreen";
    case Ui::guppy: return "ZMOD + GuppyScreen";
    case Ui::original: return "ZMOD + tela original";
    default: return mode() == Mode::zmod ? "ZMOD" : std::string();
    }
}

namespace {

std::string upper(std::string s)
{
    boost::to_upper(s);
    return s;
}

// Orca filament type (and preset name) -> the nearest entry of ZMOD's valid_types; "" when nothing fits.
std::string map_zmod_type(const std::string& type, const std::string& name, const std::vector<std::string>& valid)
{
    const std::string t = upper(type), n = upper(name);
    std::string       want;
    if (n.find("SILK") != std::string::npos && t.rfind("PLA", 0) == 0)
        want = "SILK";
    else if (t == "PLA-CF" || t == "PETG-CF" || t == "PLA-GF")
        want = t == "PLA-GF" ? "PLA-CF" : t;
    else if (t.rfind("TPU", 0) == 0 || t == "TPE")
        want = "TPU";
    else if (t.rfind("ABS", 0) == 0 || t.rfind("ASA", 0) == 0)
        want = "ABS";
    else if (t.rfind("PETG", 0) == 0 || t == "PET")
        want = "PETG";
    else if (t.rfind("PLA", 0) == 0)
        want = "PLA";
    for (const std::string& v : valid)
        if (upper(v) == want)
            return v;
    return {};
}

} // namespace

std::string FlashforgePrinterAgent::write_block_reason() const
{
    if (mode() != Mode::zmod)
        return "the printer is not a ZMOD printer";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_valid_types.empty() || m_zmod_url.empty())
            return "ZMOD's colour interface is not available";
    }
    if (m_busy)
        return "the printer is printing";
    if (m_writing)
        return "a write is in progress";
    return {};
}

void FlashforgePrinterAgent::write_slots(std::vector<SlotWrite> writes)
{
    if (writes.empty() || m_writing.exchange(true))
        return;
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    std::thread([weak, writes = std::move(writes)]() mutable {
        if (auto self = weak.lock()) {
            self->write_slots_worker(std::move(writes));
            self->m_writing = false;
        }
    }).detach();
}

// lane_data/lane<N> for HelixScreen: merge into the record already there so its helix_* keys (and ownership) survive.
bool FlashforgePrinterAgent::write_lane_data(const std::string& base_url, const std::string& api_key, const SlotWrite& w,
                                             const std::string& zmod_type, const IfsSlot* /*before*/)
{
    const std::string key = "lane" + std::to_string(w.port);
    json              existing = json::object();
    std::string       body;
    if (http_get(base_url + "/server/database/item?namespace=lane_data&key=" + key, api_key, body)) {
        const json r = json::parse(body, nullptr, false);
        if (!r.is_discarded() && r.contains("result") && r["result"].contains("value") && r["result"]["value"].is_object())
            existing = r["result"]["value"];
    }
    json rec = existing;
    const std::string material = base_type(w.type).empty() ? upper(w.type) : base_type(w.type);
    const bool changed_identity = norm_color(str(existing, "color")) != w.color || upper(str(existing, "material")) != upper(material) ||
                                  (!zmod_type.empty() && upper(str(existing, "helix_material")) != upper(zmod_type)); // PLA <-> SILK
    // A "foreign" record on purpose (docs/zoukei/sync-flashforge.md): no helix_declared / helix_locked_* / scan_time and only the
    // vendor_name / name spellings (HelixScreen treats vendor / spool_name / helix_* as its own authorship marks, and a declared
    // colour or material outranks every later firmware reading, so the screen kept the old slot until a reboot).
    for (auto it = rec.begin(); it != rec.end();) {
        const std::string k = it.key();
        if (k.rfind("helix_", 0) == 0 && k != "helix_spoolman_filament_id" || k == "vendor" || k == "spool_name" || k == "scan_time")
            it = rec.erase(it);
        else
            ++it;
    }
    rec["lane"]     = std::to_string(w.port - 1);
    rec["color"]    = w.color;
    rec["material"] = material;
    if (!w.vendor.empty())
        rec["vendor_name"] = w.vendor;
    if (!w.name.empty())
        rec["name"] = w.name;
    if (w.nozzle_temp > 0)
        rec["nozzle_temp"] = w.nozzle_temp;
    if (w.bed_temp > 0)
        rec["bed_temp"] = w.bed_temp;
    if (w.colors.size() >= 2) { // dual / tri / quad / gradient: "color" stays the first colour (what the firmware keeps)
        rec["multi_color_hexes"] = w.colors;
        rec["color_type"]        = w.gradient ? "gradient" : "multi";
    } else {
        rec.erase("multi_color_hexes");
        rec.erase("color_type");
    }
    if (changed_identity)
        rec["color_name"] = ""; // stale HelixScreen colour name (e.g. "Bronze") no longer matches
    if (changed_identity)
        for (const char* stale : {"spool_id", "spoolman_vendor_id", "helix_spoolman_filament_id", "helix_catalog_id",
                                  "remaining_weight_g", "total_weight_g"})
            rec.erase(stale);
    if (w.spool_id > 0) { // the project filament has a Spoolman spool: bind the lane (HelixScreen reads the rest from Spoolman)
        rec["spool_id"] = w.spool_id;
        if (w.spool_vendor_id > 0)
            rec["spoolman_vendor_id"] = w.spool_vendor_id;
        if (w.spool_filament_id > 0)
            rec["helix_spoolman_filament_id"] = w.spool_filament_id;
        if (!w.spool_vendor.empty())
            rec["vendor_name"] = w.spool_vendor;
        if (!w.spool_name.empty())
            rec["name"] = w.spool_name;
    } else if (w.spool_id == 0) {
        for (const char* stale : {"spool_id", "spoolman_vendor_id", "helix_spoolman_filament_id", "remaining_weight_g", "total_weight_g"})
            rec.erase(stale);
    }
    if (rec == existing)
        return true;

    bool        ok = false;
    std::string error;
    auto        http = Http::post(base_url + "/server/database/item");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(json{{"namespace", "lane_data"}, {"key", key}, {"value", rec}}.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) { ok = status == 200; })
        .on_error([&](std::string, std::string err, unsigned status) { error = err + " (HTTP " + std::to_string(status) + ")"; })
        .perform_sync();
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "Flashforge agent: lane_data write failed for " << key << ": " << error;
    return ok;
}

// Owner-initiated repair: a lane_data record that contradicts what the firmware reports (colour / material) is rewritten from the firmware,
// with the stale spool identity (Spoolman binding, name, vendor) dropped. HelixScreen ranks a declaring or spool-bound record above the firmware,
// so an old pink "SILK" spool would otherwise keep painting the slot. Lanes that agree are not touched.
void FlashforgePrinterAgent::repair_lane_data()
{
    if (m_writing.exchange(true))
        return;
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    std::thread([weak]() {
        auto self = weak.lock();
        if (!self)
            return;
        std::string base_url, api_key;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            base_url = self->m_zmod_url;
            api_key  = self->m_zmod_key;
        }
        std::string          fixed, failed;
        std::vector<IfsSlot> live;
        if (self->ui() != Ui::helix || base_url.empty() || self->m_busy || !self->fetch_zmod_slots(base_url, api_key, live)) {
            CrealityCfsAgent::notify("Flashforge IFS: nothing to repair (needs ZMOD + HelixScreen, idle, reachable).", true);
            self->m_writing = false;
            return;
        }
        for (const IfsSlot& s : live) {
            const std::string key = "lane" + std::to_string(s.port);
            std::string       body;
            if (!http_get(base_url + "/server/database/item?namespace=lane_data&key=" + key, api_key, body))
                continue;
            const json r = json::parse(body, nullptr, false);
            if (r.is_discarded() || !r.contains("result") || !r["result"].contains("value") || !r["result"]["value"].is_object())
                continue;
            json              rec      = r["result"]["value"];
            const std::string rec_type = rec.contains("helix_material") ? str(rec, "helix_material") : str(rec, "material");
            const bool        color_bad = !s.color.empty() && norm_color(str(rec, "color")) != s.color;
            const bool        type_bad  = !s.material.empty() && upper(rec_type) != upper(s.material) && upper(str(rec, "material")) != upper(s.material);
            if (!color_bad && !type_bad)
                continue;
            if (!s.color.empty())
                rec["color"] = s.color;
            if (!s.material.empty())
                rec["material"] = base_type(s.material).empty() ? upper(s.material) : base_type(s.material);
            rec["color_name"] = "";
            for (const char* stale : {"spool_id", "spoolman_vendor_id", "helix_spoolman_filament_id", "helix_catalog_id", "helix_product_name",
                                      "remaining_weight_g", "total_weight_g", "name", "spool_name", "vendor", "vendor_name", "scan_time"})
                rec.erase(stale);
            for (auto it = rec.begin(); it != rec.end();) // foreign record, see write_lane_data
                it = it.key().rfind("helix_", 0) == 0 ? rec.erase(it) : std::next(it);
            bool ok   = false;
            auto http = Http::post(base_url + "/server/database/item");
            if (!api_key.empty())
                http.header("X-Api-Key", api_key);
            http.header("Content-Type", "application/json")
                .set_post_body(json{{"namespace", "lane_data"}, {"key", key}, {"value", rec}}.dump())
                .timeout_connect(5)
                .timeout_max(10)
                .on_complete([&](std::string, unsigned status) { ok = status == 200; })
                .on_error([](std::string, std::string, unsigned) {})
                .perform_sync();
            std::string& list = ok ? fixed : failed;
            list += (list.empty() ? "" : ", ") + std::string("IFS") + std::to_string(s.port);
        }
        if (!failed.empty())
            CrealityCfsAgent::notify("Flashforge IFS: HelixScreen data not repaired for " + failed, true);
        else
            CrealityCfsAgent::notify(fixed.empty() ? std::string("Flashforge IFS: HelixScreen data already matches the printer.") :
                                                      "Flashforge IFS: HelixScreen data repaired for " + fixed + ".");
        self->m_writing = false;
    }).detach();
}

void FlashforgePrinterAgent::write_slots_worker(std::vector<SlotWrite> writes)
{
    std::string base_url, api_key;
    std::vector<std::string> valid;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        base_url = m_zmod_url;
        api_key  = m_zmod_key;
        valid    = m_valid_types;
    }
    std::string ok_list, bad_list;
    bool        any_cmd = false;
    auto add = [](std::string& list, const std::string& item) { list += (list.empty() ? "" : ", ") + item; };
    for (const SlotWrite& w : writes) {
        const std::string label = "IFS" + std::to_string(w.port);
        std::vector<IfsSlot> live;
        if (!fetch_zmod_slots(base_url, api_key, live)) {
            add(bad_list, label + ": the printer did not answer");
            continue;
        }
        if (m_busy) {
            add(bad_list, label + ": the printer is printing");
            continue;
        }
        const IfsSlot* cur = nullptr;
        for (const IfsSlot& s : live)
            if (s.port == w.port)
                cur = &s;
        const std::string type = map_zmod_type(w.type, w.name, valid);
        const std::string hex  = w.color.size() == 7 ? w.color.substr(1) : std::string("161616"); // never empty: CHANGE_ZCOLOR fails
        std::string       warn;
        if (type.empty())
            warn = " (no ZMOD material for " + w.type + ", only the colour was written)";
        const bool type_differs  = !type.empty() && (cur == nullptr || upper(cur->material) != upper(type));
        const bool color_differs = cur == nullptr || cur->color != "#" + hex;
        bool       ok            = true;
        if (type_differs || color_differs) {
            std::string script = "CHANGE_ZCOLOR SLOT=" + std::to_string(w.port);
            if (!type.empty())
                script += " TYPE=" + type;
            script += " HEX=" + hex;
            ok      = send_gcode_sync(device_info.dev_id, script);
            any_cmd = true;
            // ZMOD opens its "Material carregado / Selecione uma bobina" prompt (Mainsail/Fluidd) from inside CHANGE_ZCOLOR itself, there is no
            // silent parameter (the console shows prompt_end + prompt_begin..prompt_show during the command). Closing it afterwards only
            // dismisses the web dialog; the slot data is already saved.
            if (ok)
                send_gcode_sync(device_info.dev_id, "RESPOND TYPE=command MSG=action:prompt_end");
            BOOST_LOG_TRIVIAL(info) << "Flashforge agent: " << script << (ok ? "" : " FAILED");
        }
        if (ok && ui() == Ui::helix)
            write_lane_data(base_url, api_key, w, type, cur);
        else if (ok && (w.colors.size() >= 2 || (cur != nullptr && (cur->colors.size() >= 2 || cur->spool_id > 0)) || w.spool_id > 0))
            write_lane_data(base_url, api_key, w, type, cur); // other screens: only to store / clear the multi-colour info and the spool binding
        if (ok)
            add(ok_list, label + " " + (type.empty() ? w.type : type) + " " + w.color + warn);
        else
            add(bad_list, label + ": CHANGE_ZCOLOR failed");
    }
    // Read back what the firmware reports (the screens show it) for up to 10 s.
    if (any_cmd && bad_list.empty()) {
        bool matches = false;
        for (int attempt = 0; attempt < 10 && !matches; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::vector<IfsSlot> live;
            if (!fetch_zmod_slots(base_url, api_key, live))
                continue;
            matches = true;
            for (const SlotWrite& w : writes) {
                const std::string hex = w.color.size() == 7 ? w.color : std::string("#161616");
                bool found = false;
                for (const IfsSlot& s : live)
                    if (s.port == w.port)
                        found = s.color == hex;
                matches = matches && found;
            }
        }
        if (!matches)
            add(bad_list, "the printer still reports other colours (check its screen)");
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slot_signature.clear(); // rebuild the trays on the next poll
    }
    m_spool_key.clear(); // a lane's spool binding may have changed: re-evaluate Moonraker's active spool
    if (wxTheApp != nullptr)
        wxTheApp->CallAfter([] {
            if (auto* panel = GUI::FlashforgeDevicePanel::if_built())
                panel->notify_slots_changed();
        });
    if (!ok_list.empty())
        CrealityCfsAgent::notify("Flashforge IFS updated (" + ui_name() + "): " + ok_list +
                                 (ui() == Ui::original && m_display_popup ? " - the printer's screen may ask you to confirm" : std::string()));
    if (!bad_list.empty())
        CrealityCfsAgent::notify("Flashforge IFS sync failed: " + bad_list, true);
}

std::pair<std::string, std::string> FlashforgePrinterAgent::moonraker_endpoint() const
{
    if (mode() != Mode::zmod)
        return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_zmod_url, m_zmod_key};
}

// Spoolman spool <-> IFS lane: only the lane_data record is touched (spool_id and the names), never the firmware's colour / material.
// A lane without a record gets one (lane, colour, material) so Orca and HelixScreen can read it. Read-only towards Spoolman itself.
void FlashforgePrinterAgent::link_spools(std::vector<SlotWrite> links)
{
    if (links.empty() || mode() != Mode::zmod)
        return;
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    std::thread([weak, links = std::move(links)]() mutable {
        auto self = weak.lock();
        if (!self)
            return;
        for (int tries = 0; self->m_writing.exchange(true); ++tries) { // one lane_data writer at a time
            if (tries > 100) {
                CrealityCfsAgent::notify("Flashforge IFS: Spoolman link not written, another write is in progress.", true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        self->link_spools_worker(std::move(links));
        self->m_writing = false;
    }).detach();
}

void FlashforgePrinterAgent::link_spools_worker(std::vector<SlotWrite> links)
{
    std::string base_url, api_key;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        base_url = m_zmod_url;
        api_key  = m_zmod_key;
    }
    std::string ok_list, bad_list;
    auto add = [](std::string& list, const std::string& item) { list += (list.empty() ? "" : ", ") + item; };
    if (base_url.empty()) {
        CrealityCfsAgent::notify("Flashforge IFS: Spoolman link not written, the printer is not connected.", true);
        return;
    }
    for (const SlotWrite& w : links) {
        const std::string label = "IFS" + std::to_string(w.port);
        if (w.port < 1 || w.port > 4 || w.spool_id < 0)
            continue;
        json        lanes = json::object();
        std::string body;
        if (http_get(base_url + "/server/database/item?namespace=lane_data", api_key, body)) {
            const json l = json::parse(body, nullptr, false);
            if (!l.is_discarded() && l.contains("result") && l["result"].contains("value") && l["result"]["value"].is_object())
                lanes = l["result"]["value"];
        }
        static const char* spool_keys[] = {"spool_id", "spoolman_vendor_id", "helix_spoolman_filament_id", "remaining_weight_g", "total_weight_g"};
        const std::string  key = "lane" + std::to_string(w.port);
        if (w.spool_id > 0) // a spool sits in one lane only
            for (auto it = lanes.begin(); it != lanes.end(); ++it) {
                if (it.key() == key || !it.value().is_object() || int(num(it.value(), "spool_id")) != w.spool_id)
                    continue;
                json other = it.value();
                for (const char* k : spool_keys)
                    other.erase(k);
                post_lane_record(base_url, api_key, it.key(), other);
            }
        json existing = lanes.contains(key) && lanes[key].is_object() ? lanes[key] : json::object();
        json rec      = existing;
        if (rec.empty() && w.spool_id > 0) {
            rec["lane"]     = std::to_string(w.port - 1);
            rec["color"]    = w.color.empty() ? std::string("#161616") : w.color;
            rec["material"] = base_type(w.type).empty() ? upper(w.type) : base_type(w.type);
            if (w.colors.size() >= 2) {
                rec["multi_color_hexes"] = w.colors;
                rec["color_type"]        = w.gradient ? "gradient" : "multi";
            }
        }
        if (w.spool_id > 0) {
            rec["spool_id"] = w.spool_id;
            if (w.spool_vendor_id > 0)
                rec["spoolman_vendor_id"] = w.spool_vendor_id;
            if (w.spool_filament_id > 0)
                rec["helix_spoolman_filament_id"] = w.spool_filament_id;
            if (!w.spool_vendor.empty())
                rec["vendor_name"] = w.spool_vendor;
            if (!w.spool_name.empty())
                rec["name"] = w.spool_name;
        } else
            for (const char* k : spool_keys)
                rec.erase(k);
        if (rec == existing) {
            add(ok_list, label + (w.spool_id > 0 ? " #" + std::to_string(w.spool_id) : std::string(" (no spool)")));
            continue;
        }
        if (rec.empty()) { // nothing to unbind in a lane without a record
            add(ok_list, label + " (no spool)");
            continue;
        }
        std::string error;
        if (post_lane_record(base_url, api_key, key, rec, &error))
            add(ok_list, label + (w.spool_id > 0 ? " #" + std::to_string(w.spool_id) : std::string(" (no spool)")));
        else {
            BOOST_LOG_TRIVIAL(warning) << "Flashforge agent: spool link write failed for " << key << ": " << error;
            add(bad_list, label);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slot_signature.clear(); // the trays show the spool: rebuild them on the next poll
    }
    m_spool_key.clear(); // Moonraker's active spool follows the lane that is loaded: re-evaluate it
    if (wxTheApp != nullptr)
        wxTheApp->CallAfter([] {
            if (auto* panel = GUI::FlashforgeDevicePanel::if_built())
                panel->notify_slots_changed();
        });
    if (!ok_list.empty())
        CrealityCfsAgent::notify("Flashforge IFS: Spoolman spool linked: " + ok_list);
    if (!bad_list.empty())
        CrealityCfsAgent::notify("Flashforge IFS: Spoolman link not written for " + bad_list, true);
}

// The AMS dialog of the sidebar (edit a slot's filament / colour) -> the same write.
int FlashforgePrinterAgent::handle_ams_setting(const json& cmd)
{
    auto field = [&](const char* k) { return cmd.contains(k) ? cmd[k] : json(); };
    const json slot = cmd.contains("slot_id") ? cmd["slot_id"] : field("tray_id");
    const int  slot_no = slot.is_number() ? slot.get<int>() : slot.is_string() ? std::atoi(slot.get<std::string>().c_str()) : -1;
    const std::string type = str(cmd, "tray_type");
    if (slot_no < 0 || slot_no > 3 || type.empty())
        return BAMBU_NETWORK_SUCCESS; // the dialog's reset: nothing to write
    if (const std::string reason = write_block_reason(); !reason.empty()) {
        CrealityCfsAgent::notify("Flashforge IFS: not written, " + reason + ".", true);
        return BAMBU_NETWORK_SUCCESS;
    }
    SlotWrite w;
    w.port  = slot_no + 1;
    w.type  = type;
    w.color = norm_color(str(cmd, "tray_color"));
    if (PresetBundle* bundle = GUI::wxGetApp().preset_bundle) { // UI thread
        const std::string id = str(cmd, "tray_info_idx");
        for (const Preset& p : bundle->filaments.get_presets())
            if (!id.empty() && !p.is_default && p.filament_id == id) {
                w.name = p.name;
                if (const auto* v = p.config.option<ConfigOptionStrings>("filament_vendor"); v != nullptr && !v->values.empty())
                    w.vendor = v->values.front();
                if (const auto* t = p.config.option<ConfigOptionInts>("nozzle_temperature"); t != nullptr && !t->values.empty())
                    w.nozzle_temp = t->values.front();
                if (const auto* t = p.config.option<ConfigOptionInts>("hot_plate_temp"); t != nullptr && !t->values.empty())
                    w.bed_temp = t->values.front();
                break;
            }
    }
    write_slots({w});
    return BAMBU_NETWORK_SUCCESS;
}

// ---- stock firmware -------------------------------------------------------------------------------

bool FlashforgePrinterAgent::stock_post(const std::string& path, const json& body, json& reply, std::string& error, int timeout) const
{
    std::string base, serial, check;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        base   = m_session.base_url;
        serial = m_session.serial;
        check  = m_session.check_code;
    }
    json payload = body;
    payload["serialNumber"] = serial;
    payload["checkCode"]    = check;
    bool        ok = false;
    std::string text;
    Http::post(base + path)
        .header("Content-Type", "application/json")
        .set_post_body(payload.dump())
        .timeout_connect(3)
        .timeout_max(timeout)
        .on_complete([&](std::string b, unsigned status) {
            text = std::move(b);
            ok   = status == 200;
            if (!ok)
                error = "HTTP " + std::to_string(status);
        })
        .on_error([&](std::string, std::string err, unsigned status) { error = err + (status ? " (HTTP " + std::to_string(status) + ")" : std::string()); })
        .perform_sync();
    if (!ok)
        return false;
    reply = json::parse(text, nullptr, false);
    if (reply.is_discarded() || !reply.is_object()) {
        error = "invalid reply";
        return false;
    }
    if (num(reply, "code", -1.) != 0. || str(reply, "message") != "Success") {
        error = "API: " + str(reply, "message");
        return false;
    }
    return true;
}

bool FlashforgePrinterAgent::stock_control(const std::string& cmd, const json& args)
{
    json        reply;
    std::string error;
    const bool  ok = stock_post("/control", {{"payload", {{"cmd", cmd}, {"args", args}}}}, reply, error);
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "Flashforge agent: " << cmd << " failed: " << error;
    return ok;
}

void FlashforgePrinterAgent::stock_loop(uint64_t generation)
{
    int  fails      = 0;
    bool was_online = false;
    std::weak_ptr<FlashforgePrinterAgent> weak = weak_from_this();
    while (is_current(generation)) {
        json        reply;
        std::string error;
        if (stock_post("/detail", json::object(), reply, error) && reply.contains("detail") && reply["detail"].is_object()) {
            fails      = 0;
            was_online = true;
            json detail = reply["detail"];
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_detail = detail;
                if (detail.contains("cameraStreamUrl") && detail["cameraStreamUrl"].is_string() && !detail["cameraStreamUrl"].get<std::string>().empty())
                    m_camera_url = detail["cameraStreamUrl"].get<std::string>();
            }
            run_on_main([weak, generation, detail] {
                if (auto self = weak.lock(); self && self->is_current(generation))
                    self->publish_stock(generation, detail);
            });
        } else {
            if (fails == 0)
                BOOST_LOG_TRIVIAL(warning) << "Flashforge agent: stock /detail failed: " << error;
            if (++fails == 3 && was_online) {
                was_online = false;
                std::string dev_id;
                OnLocalConnectedFn fn;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    dev_id               = m_session.dev_id;
                    fn                   = m_on_local_connect;
                    m_connected_reported = false;
                }
                if (fn)
                    run_on_main([fn, dev_id] { fn(ConnectStatusLost, dev_id, "timeout"); });
            }
            if (fails == 3 && !was_online && error.rfind("API:", 0) == 0)
                CrealityCfsAgent::notify("Flashforge: " + error.substr(5) + " (check the serial number and check code, API key field = SERIAL:CHECKCODE)", true);
        }
        for (int i = 0; i < 20 && is_current(generation); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string FlashforgePrinterAgent::preset_id_for(const IfsSlot& slot) const
{
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    return CrealityPrintAgent::match_filament_preset(bundle->filaments, slot.vendor, slot.name.empty() ? slot.material : slot.name, base_type(slot.material));
}

json FlashforgePrinterAgent::tray_json(int slot_index, const IfsSlot* slot) const
{
    json tray = {{"id", std::to_string(slot_index)}, {"tag_uid", "0000000000000000"}};
    if (slot != nullptr && slot->has_filament) {
        const std::string type = base_type(slot->material);
        const std::string rgb  = slot->color.empty() ? std::string("FFFFFF") : slot->color.substr(1);
        tray["tray_info_idx"]   = preset_id_for(*slot);
        tray["tray_type"]       = type;
        tray["tray_sub_brands"] = slot->name.empty() ? slot->material : slot->name;
        tray["tray_color"]      = rgb + "FF";
        tray["tray_diameter"]   = "1.75";
        tray["tray_weight"]     = "";
        tray["remain"]          = -1;
        tray["cols"]            = json::array({tray["tray_color"]});
        tray["ctype"]           = 0;
    } else {
        tray["tray_info_idx"]         = "";
        tray["tray_type"]             = "";
        tray["tray_color"]            = "00000000";
        tray["tray_slot_placeholder"] = "1";
    }
    return tray;
}

json FlashforgePrinterAgent::build_stock_status(const json& d)
{
    std::string state = str(d, "status");
    boost::to_lower(state);
    std::string gcode_state = "IDLE";
    if (state == "printing" || state == "heating" || state == "calibrate_doing" || state == "busy" || state == "downloading")
        gcode_state = "RUNNING";
    else if (state == "paused" || state == "pause" || state == "pausing")
        gcode_state = "PAUSE";
    else if (state == "completed")
        gcode_state = "FINISH";
    else if (state == "error" || state == "cancel")
        gcode_state = "FAILED";

    json p = {{"command", "push_status"},
              {"msg", 0},
              {"sequence_id", std::to_string(m_sequence++)},
              {"support_mqtt_alive", true},
              {"gcode_state", gcode_state},
              {"mc_print_stage", gcode_state == "RUNNING" ? "2" : "1"},
              {"mc_print_error_code", "0"},
              {"print_error", 0},
              {"home_flag", 256}, // bits 8-9 = storage state 1 (normal)
              {"support_bed_leveling", true},
              {"support_send_to_sd", true},
              {"sdcard", true}};

    // IFS: slot 1..4 -> tray 0..3
    std::vector<IfsSlot> slots;
    if (d.contains("matlStationInfo") && d["matlStationInfo"].is_object() && d["matlStationInfo"].contains("slotInfos") &&
        d["matlStationInfo"]["slotInfos"].is_array())
        for (const json& s : d["matlStationInfo"]["slotInfos"]) {
            IfsSlot slot;
            slot.port         = int(num(s, "slotId"));
            slot.has_filament = s.value("hasFilament", false);
            slot.material     = str(s, "materialName");
            slot.color        = norm_color(str(s, "materialColor"));
            if (slot.port >= 1 && slot.port <= 4)
                slots.push_back(std::move(slot));
        }
    std::sort(slots.begin(), slots.end(), [](const IfsSlot& a, const IfsSlot& b) { return a.port < b.port; });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slots = slots;
    }
    if (!slots.empty()) {
        json               trays = json::array();
        unsigned           exist = 0;
        for (int i = 0; i < 4; ++i) {
            const IfsSlot* slot = nullptr;
            for (const IfsSlot& s : slots)
                if (s.port == i + 1)
                    slot = &s;
            if (slot != nullptr && slot->has_filament)
                exist |= 1u << i;
            trays.push_back(tray_json(i, slot));
        }
        char bits[16];
        std::snprintf(bits, sizeof(bits), "%X", exist);
        p["ams"] = {{"ams", json::array({{{"id", "0"}, {"info", "0002"}, {"tray", std::move(trays)}}})},
                    {"ams_exist_bits", "1"},
                    {"tray_exist_bits", bits},
                    {"tray_is_bbl_bits", "0"},
                    {"tray_read_done_bits", bits},
                    {"tray_reading_bits", "0"},
                    {"version", 1},
                    {"insert_flag", true},
                    {"power_on_flag", false}};
    }
    // The external spool holder as Orca's "Ext" tray.
    IfsSlot ext;
    if (d.contains("indepMatlInfo") && d["indepMatlInfo"].is_object()) {
        ext.material     = str(d["indepMatlInfo"], "materialName");
        ext.color        = norm_color(str(d["indepMatlInfo"], "materialColor"));
        ext.has_filament = !ext.material.empty() && ext.material != "?";
    }
    p["vt_tray"] = tray_json(255, &ext);

    p["nozzle_temper"]        = num(d, "rightTemp");
    p["nozzle_target_temper"] = num(d, "rightTargetTemp");
    p["bed_temper"]           = num(d, "platTemp");
    p["bed_target_temper"]    = num(d, "platTargetTemp");
    if (d.contains("chamberTemp"))
        p["chamber_temper"] = num(d, "chamberTemp");
    if (d.contains("printProgress"))
        p["mc_percent"] = std::clamp(int(num(d, "printProgress") * 100. + 0.5), 0, 100);
    if (gcode_state == "RUNNING" || gcode_state == "PAUSE")
        p["mc_remaining_time"] = std::max(0, int((num(d, "estimatedTime") - num(d, "printDuration")) / 60.));
    if (d.contains("printLayer"))
        p["layer_num"] = int(num(d, "printLayer"));
    if (d.contains("targetPrintLayer"))
        p["total_layer_num"] = int(num(d, "targetPrintLayer"));
    const std::string file = str(d, "printFileName");
    if (!file.empty()) {
        p["gcode_file"] = file;
        const auto dot  = file.find_last_of('.');
        p["subtask_name"] = dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
    }
    std::string nozzle;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        nozzle = m_nozzle_diameter;
    }
    if (!nozzle.empty()) { // lets Orca show the nozzle in the printer card instead of "Unknown"
        p["nozzle_diameter"] = nozzle;
        p["nozzle_type"]     = "N/A";
    }
    return {{"print", std::move(p)}};
}

void FlashforgePrinterAgent::announce_stock(const json& d)
{
    OnMsgArrivedFn ssdp;
    json           payload;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_on_ssdp || m_announced)
            return;
        const std::string name = !m_model_name.empty() ? m_model_name : !str(d, "name").empty() ? str(d, "name") : std::string("Flashforge");
        ssdp    = m_on_ssdp;
        payload = {{"dev_name", name},
                   {"dev_id", m_session.dev_id},
                   {"dev_ip", m_session.host},
                   {"dev_type", !m_model_id.empty() ? m_model_id : name},
                   {"dev_signal", "0"},
                   {"connect_type", "lan"},
                   {"bind_state", "free"},
                   {"sec_link", "secure"},
                   {"ssdp_version", "v1"}};
        m_announced = true;
        if (m_selected_machine.empty())
            m_selected_machine = m_session.dev_id;
    }
    ssdp(payload.dump());
}

// Main thread.
void FlashforgePrinterAgent::publish_stock(uint64_t generation, json detail)
{
    if (!is_current(generation))
        return;
    bool               first = false;
    std::string        dev_id;
    OnLocalConnectedFn local_connect;
    OnPrinterConnectedFn connected;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        first = !m_connected_reported;
        m_connected_reported = true;
        dev_id        = m_session.dev_id;
        local_connect = m_on_local_connect;
        connected     = m_on_printer_connected;
    }
    if (first) {
        announce_stock(detail);
        if (local_connect)
            local_connect(ConnectStatusOk, dev_id, "0");
        if (connected)
            connected(dev_id);
    }
    publish(build_stock_status(detail));
}

// ---- commands -------------------------------------------------------------------------------------

int FlashforgePrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

int FlashforgePrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (mode() != Mode::stock) {
        const json zmsg = mode() == Mode::zmod ? json::parse(json_str, nullptr, false) : json();
        if (zmsg.is_object() && zmsg.contains("print") && zmsg["print"].is_object() && zmsg["print"].value("command", std::string()) == "ams_filament_setting")
            return handle_ams_setting(zmsg["print"]);
        return MoonrakerPrinterAgent::send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
    }
    const json msg = json::parse(json_str, nullptr, false);
    if (msg.is_discarded() || !msg.is_object())
        return -1;
    if (msg.contains("pushing")) {
        json detail;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            detail = m_detail;
        }
        if (detail.is_object() && !detail.empty())
            publish(build_stock_status(detail));
        return BAMBU_NETWORK_SUCCESS;
    }
    if (msg.contains("info") && msg["info"].is_object() && msg["info"].value("command", std::string()) == "get_version") {
        json detail;
        std::string serial;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            detail = m_detail;
            serial = m_session.serial;
        }
        std::string fw = str(detail, "firmwareVersion");
        publish({{"info",
                  {{"command", "get_version"},
                   {"sequence_id", std::to_string(m_sequence++)},
                   {"module", json::array({{{"name", "ota"}, {"project_name", str(detail, "model")}, {"sw_ver", fw.empty() ? std::string("1.0.0") : fw},
                                            {"hw_ver", str(detail, "model")}, {"sn", serial}}})}}}});
        return BAMBU_NETWORK_SUCCESS;
    }
    if (msg.contains("print") && msg["print"].is_object())
        return handle_stock_command(msg["print"]);
    if (msg.contains("system") && msg["system"].is_object()) {
        const json& sys = msg["system"];
        if (sys.value("command", std::string()) == "ledctrl") {
            const std::string mode = sys.value("led_mode", std::string());
            if (mode == "on" || mode == "off")
                stock_control("lightControl_cmd", {{"status", mode == "on" ? "open" : "close"}});
        }
        return BAMBU_NETWORK_SUCCESS;
    }
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::handle_stock_command(const json& cmd)
{
    const std::string command = cmd.value("command", std::string());
    auto temp_of = [&](const char* key) {
        return cmd.contains(key) && cmd[key].is_number() ? cmd[key].get<int>() : cmd.contains(key) && cmd[key].is_string() ? std::atoi(cmd[key].get<std::string>().c_str()) : 0;
    };
    if (command == "pause" || command == "resume" || command == "stop") {
        const char* action = command == "pause" ? "pause" : command == "resume" ? "continue" : "cancel";
        return stock_control("jobCtl_cmd", {{"jobID", ""}, {"action", action}}) ? BAMBU_NETWORK_SUCCESS : -1;
    }
    if (command == "set_bed_temp" || command == "set_nozzle_temp") {
        const int t = command == "set_bed_temp" ? temp_of("temp") : (cmd.contains("target_temp") ? temp_of("target_temp") : temp_of("temp"));
        json args = {{"rightNozzle", -200}, {"leftNozzle", -200}, {"platform", -200}, {"chamber", -200}};
        args[command == "set_bed_temp" ? "platform" : "rightNozzle"] = t <= 0 ? -100 : t; // -200 = unchanged, -100 = off
        return stock_control("temperatureCtl_cmd", args) ? BAMBU_NETWORK_SUCCESS : -1;
    }
    if (command == "ams_filament_setting") { // write a slot's material / colour to the IFS
        auto field = [&](const char* k) { return cmd.contains(k) ? cmd[k] : json(); };
        json slot = cmd.contains("slot_id") ? cmd["slot_id"] : field("tray_id");
        const int slot_no = slot.is_number() ? slot.get<int>() : slot.is_string() ? std::atoi(slot.get<std::string>().c_str()) : -1;
        const std::string color = norm_color(str(cmd, "tray_color"));
        std::string type = str(cmd, "tray_type");
        if (slot_no < 0 || slot_no > 3 || color.empty() || type.empty())
            return -1;
        return stock_control("msConfig_cmd", {{"slot", slot_no + 1}, {"mt", type}, {"rgb", color}}) ? BAMBU_NETWORK_SUCCESS : -1;
    }
    if (command == "push_status" || command == "get_version") {
        json detail;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            detail = m_detail;
        }
        if (detail.is_object() && !detail.empty())
            publish(build_stock_status(detail));
        return BAMBU_NETWORK_SUCCESS;
    }
    BOOST_LOG_TRIVIAL(info) << "Flashforge agent: print command not supported on the stock firmware: " << command;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

// ---- print jobs -----------------------------------------------------------------------------------

int FlashforgePrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (mode() == Mode::stock)
        return stock_upload(std::move(params), std::move(update_fn), std::move(cancel_fn), true);
    return zmod_job(std::move(params), std::move(update_fn), std::move(cancel_fn), true);
}

int FlashforgePrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (mode() == Mode::stock)
        return stock_upload(std::move(params), std::move(update_fn), std::move(cancel_fn), false);
    if (params.extra_options.find("\"files\"") != std::string::npos) // from the print dialog's "Send" button
        return zmod_job(std::move(params), std::move(update_fn), std::move(cancel_fn), false);
    return MoonrakerPrinterAgent::start_send_gcode_to_sdcard(std::move(params), std::move(update_fn), std::move(cancel_fn), std::move(wait_fn));
}

int FlashforgePrinterAgent::stock_upload(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, bool start)
{
    auto report = [&](int stage, int code, const std::string& text) {
        if (update_fn)
            update_fn(stage, code, text);
    };
    auto fail = [&](const std::string& text) {
        BOOST_LOG_TRIVIAL(error) << "Flashforge agent: " << text;
        report(PrintingStageERROR, -1, text);
        return -1;
    };
    std::string base, serial, check;
    json        detail;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        base   = m_session.base_url;
        serial = m_session.serial;
        check  = m_session.check_code;
        detail = m_detail;
    }
    if (base.empty())
        return fail("not connected to the printer");
    report(PrintingStageCreate, 0, "");

    namespace fs = boost::filesystem;
    const auto files = job_files(params);
    if (files.empty())
        return fail("no G-code file to send");
    for (const JobFile& f : files)
        if (!fs::exists(fs::path(f.path)))
            return fail("G-code file not found: " + f.path);

    // Every project filament -> the IFS slot the dialog mapped it to (toolId 0-3, slotId 1-4).
    json mappings = json::array();
    if (start) {
        const json mapping = json::parse(params.ams_mapping_info, nullptr, false);
        if (mapping.is_array())
            for (size_t i = 0; i < mapping.size(); ++i) {
                const json& item = mapping[i];
                const int   tray = item.is_object() && item.contains("ams") && item["ams"].is_number_integer() ? item["ams"].get<int>() : -1;
                if (tray < 0 || tray > 3)
                    continue; // unmapped, or the external spool (a print without the IFS)
                if (i > 3)
                    return fail("the printer's IFS supports at most 4 filaments per print");
                std::string material = item.value("filamentType", std::string("PLA"));
                std::string slot_color;
                if (detail.contains("matlStationInfo"))
                    for (const json& s : detail["matlStationInfo"].value("slotInfos", json::array()))
                        if (int(num(s, "slotId")) == tray + 1) {
                            if (!str(s, "materialName").empty())
                                material = str(s, "materialName");
                            slot_color = norm_color(str(s, "materialColor"));
                        }
                std::string tool_color = norm_color(item.value("sourceColor", std::string("#FFFFFF")));
                if (tool_color.empty())
                    tool_color = "#FFFFFF";
                mappings.push_back({{"toolId", int(i)}, {"slotId", tray + 1}, {"materialName", material},
                                    {"toolMaterialColor", tool_color}, {"slotMaterialColor", slot_color.empty() ? tool_color : slot_color}});
            }
    }
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };

    for (size_t k = 0; k < files.size(); ++k) {
        const bool   first    = k == 0;
        const bool   use_ifs  = first && start && !mappings.empty();
        const bool   print_it = first && start;
        const size_t total    = files.size();
        bool         ok = true, cancelled = false;
        std::string  error, reply_text;
        Http::post(base + "/uploadGcode")
            .header("serialNumber", serial)
            .header("checkCode", check)
            .header("fileSize", std::to_string(fs::file_size(fs::path(files[k].path))))
            .header("printNow", b(print_it))
            .header("levelingBeforePrint", b(params.task_bed_leveling))
            .header("flowCalibration", b(params.task_flow_cali))
            .header("firstLayerInspection", b(params.task_layer_inspect))
            .header("timeLapseVideo", b(params.task_record_timelapse))
            .header("useMatlStation", b(use_ifs))
            .header("gcodeToolCnt", std::to_string(use_ifs ? mappings.size() : size_t(0)))
            .header("materialMappings", base64(use_ifs ? mappings.dump() : std::string("[]")))
            .form_add_file("gcodeFile", fs::path(files[k].path).string(), files[k].name)
            .timeout_connect(5)
            .timeout_max(600)
            .on_complete([&](std::string body, unsigned status) {
                reply_text = body;
                if (status != 200) {
                    ok    = false;
                    error = "HTTP " + std::to_string(status);
                }
            })
            .on_error([&](std::string, std::string err, unsigned status) {
                ok    = false;
                error = err + (status ? " (HTTP " + std::to_string(status) + ")" : std::string());
            })
            .on_progress([&](Http::Progress progress, bool& cancel) {
                if (cancel_fn && cancel_fn()) {
                    cancel = cancelled = true;
                    return;
                }
                if (progress.ultotal > 0) {
                    const int pct = std::min(100, int(100. * double(progress.ulnow) / double(progress.ultotal)));
                    report(PrintingStageUpload, int((k * 100 + size_t(pct)) / total), std::to_string(pct) + "%");
                }
            })
            .perform_sync();
        if (cancelled || (cancel_fn && cancel_fn()))
            return BAMBU_NETWORK_ERR_CANCELED;
        if (ok) {
            const json reply = json::parse(reply_text, nullptr, false);
            if (reply.is_discarded() || num(reply, "code", -1.) != 0.) {
                ok    = false;
                error = reply.is_discarded() ? "invalid reply" : str(reply, "message");
            }
        }
        if (!ok)
            return fail(error.empty() ? std::string("upload failed") : error);
    }
    report(PrintingStageFinished, 0, "3");
    return BAMBU_NETWORK_SUCCESS;
}

// ---- discovery / selection ------------------------------------------------------------------------

bool FlashforgePrinterAgent::start_discovery(bool start, bool sending)
{
    if (mode() != Mode::stock)
        return MoonrakerPrinterAgent::start_discovery(start, sending);
    json detail;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        detail = m_detail;
    }
    if (start && detail.is_object() && !detail.empty())
        announce_stock(detail);
    return true;
}

int FlashforgePrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    if (mode() != Mode::stock)
        return MoonrakerPrinterAgent::bind_detect(std::move(dev_ip), std::move(sec_link), detect);
    std::lock_guard<std::mutex> lock(m_mutex);
    detect.dev_id       = m_session.dev_id.empty() ? dev_ip : m_session.dev_id;
    detect.model_id     = m_model_id;
    detect.dev_name     = m_model_name.empty() ? dev_ip : m_model_name;
    detect.version      = str(m_detail, "firmwareVersion");
    detect.connect_type = "lan";
    detect.bind_state   = "free";
    return BAMBU_NETWORK_SUCCESS;
}

std::string FlashforgePrinterAgent::get_user_selected_machine()
{
    if (mode() != Mode::stock)
        return MoonrakerPrinterAgent::get_user_selected_machine();
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected_machine.empty() ? m_session.dev_id : m_selected_machine;
}

int FlashforgePrinterAgent::set_user_selected_machine(std::string dev_id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_selected_machine = dev_id;
    }
    return MoonrakerPrinterAgent::set_user_selected_machine(std::move(dev_id));
}

bool FlashforgePrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode)
{
    if (mode() == Mode::stock) {
        json detail;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            detail = m_detail;
        }
        if (detail.is_object() && !detail.empty())
            publish(build_stock_status(detail));
        return !detail.empty();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slot_signature.clear(); // the next poll (a few seconds) rebuilds the trays
    }
    return !ifs_slots().empty() || MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id), sync_mode);
}

} // namespace Slic3r
