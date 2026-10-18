// Native Snapmaker U1 agent (Klipper + Moonraker): see the header for the protocol summary.
#include "SnapmakerPrinterAgent.hpp"
#include "CrealityCfsAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgent.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/DeviceManager.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "libslic3r/Preset.hpp"

namespace Slic3r {

using json = nlohmann::json;

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.1.0";
constexpr int         LOGICAL_EXTRUDERS       = 32; // print_task_config.py LOGICAL_EXTRUDER_NUM

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

std::string find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                         const std::string&      vendor_name,
                                                         const std::string&      filament_type,
                                                         const std::string&      color_rgba)
{
    std::string  best_match_id       = "";
    unsigned int best_color_distance = 0xffffffffu;
    unsigned int target_color_value  = 0;
    try {
        // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
        // so we must remove # and ignore alpha channel for distance calculation
        target_color_value = std::stoul(color_rgba.substr(0, color_rgba.length() - 2), nullptr, 16);
    } catch (...) {
        return best_match_id;
    }

    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible &&
            // Filament profile must be detached from parent to be considered for matching
            filaments.get_preset_base(p) == &p && p.config.opt_string("filament_vendor", 0u) == vendor_name &&
            p.config.opt_string("filament_type", 0u) == filament_type) {
            std::string  p_color = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value = 0; // no colour in the profile: assume other profiles might be a closer match
            if (!p_color.empty()) {
                try {
                    size_t hash_pos = p_color.find("#");
                    p_color_value   = std::stoul(p_color.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0), nullptr, 16);
                } catch (...) {
                    p_color_value = 0;
                }
            }

            // Euclidean color distance in RGB space (the numbers are 0xRRGGBB: the channel order does not matter here)
            const int dr = int(target_color_value & 0xff) - int(p_color_value & 0xff);
            const int dg = int((target_color_value >> 8) & 0xff) - int((p_color_value >> 8) & 0xff);
            const int db = int((target_color_value >> 16) & 0xff) - int((p_color_value >> 16) & 0xff);
            const unsigned int distance = unsigned(dr * dr + dg * dg + db * db);

            if (distance < best_color_distance) {
                best_color_distance = distance;
                best_match_id       = p.filament_id;
            }
        }
    }
    return best_match_id;
}

std::string to_up(std::string s)
{
    boost::to_upper(s);
    return s;
}

std::string text_of(const json* j)
{
    return j != nullptr && j->is_string() ? j->get<std::string>() : std::string();
}

bool bool_of(const json* j, bool fallback = false)
{
    if (j == nullptr)
        return fallback;
    if (j->is_boolean())
        return j->get<bool>();
    if (j->is_number())
        return j->get<double>() != 0.;
    return fallback;
}

double number_of(const json* j, double fallback = 0.)
{
    return j != nullptr && j->is_number() ? j->get<double>() : fallback;
}

const json* jat(const json& obj, const char* key)
{
    if (!obj.is_object())
        return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

const json* jitem(const json* arr, int i)
{
    return arr != nullptr && arr->is_array() && i >= 0 && i < int(arr->size()) ? &(*arr)[size_t(i)] : nullptr;
}

std::string none_to_empty(const std::string& s)
{
    std::string t = boost::trim_copy(s);
    return boost::iequals(t, "NONE") ? std::string() : t;
}

bool all_hex(const std::string& s, size_t n)
{
    return s.size() == n && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// "#RRGGBB", "RRGGBB" or "RRGGBBAA" -> "RRGGBB" (upper case), "" when it is none of them.
std::string hex6(std::string v)
{
    boost::trim(v);
    if (!v.empty() && v[0] == '#')
        v.erase(0, 1);
    if (v.size() == 8 && all_hex(v, 8))
        v.resize(6);
    return all_hex(v, 6) ? to_up(v) : std::string();
}

// A G-code parameter value for Klipper (arguments are split like a shell does: quotes keep a space together).
std::string gcode_param(const std::string& value, const std::string& fallback)
{
    std::string out;
    for (const char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == '"' || c == '\'' || c == ';' || c == '#' || c == '\\' || c == '=' || c == '*')
            continue;
        out += c;
    }
    boost::trim(out);
    if (out.empty())
        out = fallback;
    if (out.find(' ') != std::string::npos)
        out = "\"" + out + "\"";
    return out;
}

// feed states that are a work in progress (the others are idle / finished / failed)
bool transient_feed_state(const std::string& s)
{
    if (s.empty())
        return false;
    static const char* idle[] = {"none", "inited", "wait_insert", "test"};
    for (const char* i : idle)
        if (s == i)
            return false;
    return !(boost::ends_with(s, "finish") || boost::ends_with(s, "fail"));
}

bool http_error_text(const std::string& body, std::string& message)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") && j["error"]["message"].is_string()) {
        message = j["error"]["message"].get<std::string>();
        return true;
    }
    return false;
}

} // anonymous namespace

// ---- value types ------------------------------------------------------------------------------------

std::string SnapmakerPrinterAgent::U1Head::display_type() const
{
    return type.empty() ? std::string() : SnapmakerPrinterAgent::combine_filament_type(type, sub_type);
}

bool SnapmakerPrinterAgent::U1State::feed_busy() const
{
    return std::any_of(heads.begin(), heads.end(), [](const U1Head& h) {
        return (!h.feed_action.empty() && h.feed_action != "none") || transient_feed_state(h.feed_state);
    });
}

// ---- agent ------------------------------------------------------------------------------------------

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)), m_ctl(std::make_shared<Control>()) {}

SnapmakerPrinterAgent::~SnapmakerPrinterAgent()
{
    if (m_ctl) {
        ++m_ctl->generation;
        m_ctl->cv.notify_all();
    }
}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{SNAPMAKER_PRINTER_AGENT_ID, "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::shared_ptr<SnapmakerPrinterAgent> SnapmakerPrinterAgent::current()
{
    NetworkAgent* agent = GUI::wxGetApp().getAgent();
    return agent != nullptr ? std::dynamic_pointer_cast<SnapmakerPrinterAgent>(agent->get_printer_agent()) : nullptr;
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    if (sub == "CF")
        return base + "-CF";
    if (sub == "GF")
        return base + "-GF";
    if (sub == "SNAPSPEED" || sub == "HS")
        return base + " HIGH SPEED";
    if (sub == "SILK")
        return base + " SILK";
    if (sub == "WOOD")
        return base + " WOOD";
    if (sub == "MATTE")
        return base + " MATTE";
    if (sub == "MARBLE")
        return base + " MARBLE";

    // Unrecognized sub-type (brand names like Polylite, Basic, etc.) -- use base type only
    return base;
}

int SnapmakerPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    {
        std::lock_guard<std::mutex> lock(m_u1_mutex);
        if (fn) // Orca clears the callbacks of an agent it switches away from; keep the last valid hop
            m_queue_main = fn;
    }
    return MoonrakerPrinterAgent::set_queue_on_main_fn(std::move(fn));
}

void SnapmakerPrinterAgent::run_on_main(std::function<void()> fn)
{
    QueueOnMainFn queue;
    {
        std::lock_guard<std::mutex> lock(m_u1_mutex);
        queue = m_queue_main;
    }
    if (queue)
        queue(std::move(fn));
    else if (wxTheApp != nullptr)
        GUI::wxGetApp().CallAfter(std::move(fn));
}

// ---- parsing ----------------------------------------------------------------------------------------

bool SnapmakerPrinterAgent::parse_state(const json& status, U1State& out)
{
    try {
        const json* ptc = jat(status, "print_task_config");
        if (ptc == nullptr || !ptc->is_object())
            return false;
        U1State s;
        s.valid = true;
        if (const json* ps = jat(status, "print_stats"))
            s.print_state = text_of(jat(*ps, "state"));
        if (const json* wh = jat(status, "webhooks"))
            s.klippy_state = text_of(jat(*wh, "state"));
        if (const json* th = jat(status, "toolhead"))
            s.active_extruder = text_of(jat(*th, "extruder"));
        if (const json* bed = jat(status, "heater_bed")) {
            s.bed_now    = number_of(jat(*bed, "temperature"));
            s.bed_target = number_of(jat(*bed, "target"));
        }
        if (const json* map = jat(*ptc, "extruder_map_table"); map != nullptr && map->is_array())
            for (const json& v : *map)
                s.map_table.push_back(v.is_number_integer() ? v.get<int>() : 0);
        if (const json* used = jat(*ptc, "extruders_used"); used != nullptr && used->is_array())
            for (const json& v : *used)
                s.used.push_back(bool_of(&v));

        const json* detect_info = nullptr;
        if (const json* fd = jat(status, "filament_detect"))
            detect_info = jat(*fd, "info");

        for (int i = 0; i < HEAD_COUNT; ++i) {
            U1Head h;
            h.index    = i;
            h.present  = bool_of(jitem(jat(*ptc, "filament_exist"), i));
            h.official = bool_of(jitem(jat(*ptc, "filament_official"), i));
            h.editable = bool_of(jitem(jat(*ptc, "filament_edit"), i));
            h.soft     = bool_of(jitem(jat(*ptc, "filament_soft"), i));
            h.vendor   = none_to_empty(text_of(jitem(jat(*ptc, "filament_vendor"), i)));
            h.type     = none_to_empty(text_of(jitem(jat(*ptc, "filament_type"), i)));
            h.sub_type = none_to_empty(text_of(jitem(jat(*ptc, "filament_sub_type"), i)));

            const std::string rgba = text_of(jitem(jat(*ptc, "filament_color_rgba"), i));
            const std::string c6   = hex6(rgba);
            h.color                = c6.empty() ? std::string("#FFFFFF") : "#" + c6;
            if (all_hex(rgba, 8))
                h.alpha = std::stoi(rgba.substr(6, 2), nullptr, 16);
            if (const json* multi = jitem(jat(*ptc, "filament_color_multi"), i); multi != nullptr && multi->is_object()) {
                h.color_mode = multi->contains("mode") && (*multi)["mode"].is_number_integer() ? (*multi)["mode"].get<int>() : 0;
                std::vector<std::string> cols;
                if (const json* list = jat(*multi, "colors"); list != nullptr && list->is_array())
                    for (const json& c : *list)
                        if (const std::string v = hex6(text_of(&c)); !v.empty())
                            cols.push_back("#" + v);
                if (cols.size() >= 2 && cols.front() == h.color)
                    h.colors = cols;
            }
            if (const json* info = jitem(detect_info, i); info != nullptr && info->is_object()) {
                if (none_to_empty(text_of(jat(*info, "VENDOR"))).size() > 0) {
                    h.bed_temp    = int(number_of(jat(*info, "BED_TEMP")));
                    h.nozzle_temp = int(number_of(jat(*info, "FIRST_LAYER_TEMP")));
                }
            }
            // Klipper names the first extruder "extruder", the others "extruder1" .. "extruder3"
            if (const json* ex = jat(status, i == 0 ? "extruder" : ("extruder" + std::to_string(i)).c_str())) {
                h.nozzle_now    = number_of(jat(*ex, "temperature"));
                h.nozzle_target = number_of(jat(*ex, "target"));
            }
            for (const char* feed_name : {"filament_feed left", "filament_feed right"})
                if (const json* feed = jat(status, feed_name))
                    if (const json* ch = jat(*feed, ("extruder" + std::to_string(i)).c_str()); ch != nullptr && ch->is_object()) {
                        h.feed_module   = bool_of(jat(*ch, "module_exist"));
                        h.feed_detected = bool_of(jat(*ch, "filament_detected"));
                        h.auto_feed     = !bool_of(jat(*ch, "disable_auto"));
                        h.feed_state    = text_of(jat(*ch, "channel_state"));
                        h.feed_action   = text_of(jat(*ch, "channel_action_state"));
                        h.feed_error    = text_of(jat(*ch, "channel_error"));
                    }
            for (int j = 0; j < HEAD_COUNT && j < int(s.map_table.size()); ++j)
                if (s.map_table[size_t(j)] == i) {
                    h.mapped_from = j;
                    break;
                }
            s.heads.push_back(std::move(h));
        }
        out = std::move(s);
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent: unexpected status layout: " << e.what();
        return false;
    }
}

std::string SnapmakerPrinterAgent::state_signature(const U1State& s)
{
    std::ostringstream o;
    o << s.valid << '|' << s.print_state << '|' << s.klippy_state;
    for (int v : s.map_table)
        o << ',' << v;
    for (const U1Head& h : s.heads) {
        o << '|' << h.present << h.official << h.editable << h.soft << ';' << h.vendor << ';' << h.type << ';' << h.sub_type << ';' << h.color << ';'
          << h.alpha << ';' << h.color_mode << ';' << h.feed_detected << ';' << h.feed_state << ';' << h.feed_action << ';' << h.feed_error;
        for (const std::string& c : h.colors)
            o << ';' << c;
    }
    return o.str();
}

// ---- HTTP -------------------------------------------------------------------------------------------

bool SnapmakerPrinterAgent::query_u1(const std::string& base_url, const std::string& api_key, U1State& out, std::string& error) const
{
    // Only the objects the integration needs. Klipper ignores a name the printer does not have.
    json objects = json::object();
    objects["print_task_config"]  = nullptr;
    objects["filament_detect"]    = nullptr;
    objects["filament_feed left"]  = nullptr;
    objects["filament_feed right"] = nullptr;
    objects["print_stats"]        = json::array({"state"});
    objects["webhooks"]           = json::array({"state"});
    objects["toolhead"]           = json::array({"extruder"});
    objects["heater_bed"]         = json::array({"temperature", "target"});
    for (const char* name : {"extruder", "extruder1", "extruder2", "extruder3"})
        objects[name] = json::array({"temperature", "target"});
    std::string body;
    bool        ok = false;
    auto        http = Http::post(join_url(base_url, "/printer/objects/query"));
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(json{{"objects", std::move(objects)}}.dump())
        .timeout_connect(4)
        .timeout_max(8)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            } else {
                error = "HTTP " + std::to_string(status);
            }
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            error = err;
            if (status > 0)
                error += " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();
    if (!ok)
        return false;
    const json reply = json::parse(body, nullptr, false, true);
    if (reply.is_discarded() || !reply.contains("result") || !reply["result"].contains("status")) {
        error = "unexpected reply";
        return false;
    }
    if (!parse_state(reply["result"]["status"], out)) {
        error = "no print_task_config (not a Snapmaker U1?)";
        return false;
    }
    return true;
}

bool SnapmakerPrinterAgent::post_script(const std::string& base_url, const std::string& api_key, const std::string& script, int timeout_s, std::string& error)
{
    bool ok   = false;
    auto http = Http::post(base_url + "/printer/gcode/script");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(json{{"script", script}}.dump())
        .timeout_connect(5)
        .timeout_max(timeout_s)
        .on_complete([&](std::string, unsigned status) {
            ok = status == 200;
            if (!ok)
                error = "HTTP " + std::to_string(status);
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            // Klipper answers a refused command with {"error": {"message": "..."}}: that text is for the user
            if (!http_error_text(body, error)) {
                error = err;
                if (status > 0)
                    error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();
    return ok;
}

// ---- polling ----------------------------------------------------------------------------------------

int SnapmakerPrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    const int result = MoonrakerPrinterAgent::connect_printer(params);
    if (result != BAMBU_NETWORK_SUCCESS)
        return result;
    const uint64_t generation = ++m_ctl->generation;
    {
        std::lock_guard<std::mutex> lock(m_u1_mutex);
        m_u1 = U1State{};
        m_u1_signature.clear();
    }
    std::weak_ptr<SnapmakerPrinterAgent> weak = weak_from_this();
    if (weak.expired()) // not owned by a shared_ptr (should not happen): no background polling
        return result;
    std::thread(&SnapmakerPrinterAgent::poll_main, weak, m_ctl, generation, device_info.base_url, device_info.api_key).detach();
    return result;
}

int SnapmakerPrinterAgent::disconnect_printer()
{
    ++m_ctl->generation;
    m_ctl->cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(m_u1_mutex);
        m_u1 = U1State{};
        m_u1_signature.clear();
    }
    return MoonrakerPrinterAgent::disconnect_printer();
}

void SnapmakerPrinterAgent::poll_main(std::weak_ptr<SnapmakerPrinterAgent> weak, std::shared_ptr<Control> ctl, uint64_t generation, std::string base_url, std::string api_key)
{
    int fails = 0;
    while (ctl->generation.load() == generation) {
        bool ok = false;
        if (auto self = weak.lock()) {
            self->poll_once(generation, base_url, api_key, fails);
            ok = fails == 0;
        } else {
            return;
        }
        std::unique_lock<std::mutex> lock(ctl->mutex);
        ctl->cv.wait_for(lock, std::chrono::milliseconds(ok ? 2500 : 5000), [&] { return ctl->wake.load() || ctl->generation.load() != generation; });
        ctl->wake = false;
    }
}

void SnapmakerPrinterAgent::poll_once(uint64_t generation, const std::string& base_url, const std::string& api_key, int& fails)
{
    U1State     state;
    std::string error;
    if (!query_u1(base_url, api_key, state, error)) {
        BOOST_LOG_TRIVIAL(debug) << "SnapmakerPrinterAgent: poll failed: " << error;
        if (++fails >= 3 && m_ctl->generation.load() == generation) {
            std::lock_guard<std::mutex> lock(m_u1_mutex);
            m_u1 = U1State{};
            m_u1_signature.clear();
        }
        return;
    }
    fails = 0;
    store_state(state, generation);
}

void SnapmakerPrinterAgent::store_state(const U1State& state, uint64_t generation)
{
    if (m_ctl->generation.load() != generation)
        return;
    const std::string signature = state_signature(state);
    bool              changed   = false;
    {
        std::lock_guard<std::mutex> lock(m_u1_mutex);
        changed        = signature != m_u1_signature;
        m_u1           = state;
        m_u1_signature = signature;
    }
    if (!changed)
        return;
    // the print dialog's tray list follows the heads (the Bambu-style AMS data is built on the UI thread)
    std::weak_ptr<SnapmakerPrinterAgent> weak = weak_from_this();
    run_on_main([weak, state, generation] {
        auto self = weak.lock();
        if (!self || self->m_ctl->generation.load() != generation)
            return;
        self->publish_trays(state);
    });
}

SnapmakerPrinterAgent::U1State SnapmakerPrinterAgent::u1_state() const
{
    std::lock_guard<std::mutex> lock(m_u1_mutex);
    return m_u1;
}

void SnapmakerPrinterAgent::refresh_soon()
{
    m_ctl->wake = true;
    m_ctl->cv.notify_all();
}

// ---- filament info for the print dialog -------------------------------------------------------------

void SnapmakerPrinterAgent::publish_trays(const U1State& state)
{
    if (!state.valid || state.heads.empty())
        return;
    auto* bundle = GUI::wxGetApp().preset_bundle;

    std::vector<AmsTrayData> trays;
    trays.reserve(state.heads.size());
    for (const U1Head& head : state.heads) {
        AmsTrayData tray;
        tray.slot_index   = head.index;
        tray.has_filament = head.present;
        if (tray.has_filament) {
            tray.tray_type = combine_filament_type(head.type, head.sub_type);
            char rgba[16];
            std::snprintf(rgba, sizeof(rgba), "%s%02X", head.color.size() == 7 ? head.color.substr(1).c_str() : "FFFFFF", head.alpha & 0xFF);
            tray.tray_color = rgba;

            // Try to find a matching preset for this filament based on vendor, type and colour.
            // If not found, default to traditional search by type only or generic type mapping.
            if (bundle) {
                const std::string filament_id = find_closest_color_preset_by_vendor_and_type(bundle->filaments, head.vendor, tray.tray_type, tray.tray_color);
                tray.tray_info_idx = filament_id.empty() ? bundle->filaments.filament_id_by_type(tray.tray_type) : filament_id;
            } else {
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            }
            tray.bed_temp    = head.bed_temp;
            tray.nozzle_temp = head.nozzle_temp;
        }
        trays.emplace_back(std::move(tray));
    }
    build_ams_payload(1, int(trays.size()) - 1, trays);
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/)
{
    U1State     state;
    std::string error;
    if (!query_u1(device_info.base_url, device_info.api_key, state, error)) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: " << error;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_u1_mutex);
        m_u1           = state;
        m_u1_signature = state_signature(state);
    }
    publish_trays(state);
    return true;
}

// ---- G-code builders --------------------------------------------------------------------------------

std::pair<std::string, std::string> SnapmakerPrinterAgent::firmware_type(const std::string& orca_type, const std::string& preset_name)
{
    std::string type = to_up(boost::trim_copy(orca_type));
    const std::string name = boost::to_lower_copy(preset_name);

    // Orca's finer type names -> the firmware's main types (filament_protocol.py FILAMENT_PROTO_MAIN_TYPE_MAPPING)
    if (type.rfind("PA", 0) == 0 && type != "PA" && type != "PA-CF" && type != "PA-GF") {
        if (type.find("CF") != std::string::npos)
            type = "PA-CF";
        else if (type.find("GF") != std::string::npos)
            type = "PA-GF";
        else
            type = "PA";
    } else if (type == "PET" || type == "PETT") {
        type = "PETG";
    } else if (type == "PET-CF") {
        type = "PETG-CF";
    } else if (type.empty()) {
        type = "PLA";
    }

    std::string sub = "NONE";
    if (name.find("silk") != std::string::npos)
        sub = "Silk";
    else if (name.find("matte") != std::string::npos)
        sub = "Matte";
    else if (name.find("wood") != std::string::npos)
        sub = "Wood";
    else if (name.find("marble") != std::string::npos)
        sub = "Marble";
    else if (name.find("snapspeed") != std::string::npos || name.find("high speed") != std::string::npos || name.find("high-speed") != std::string::npos)
        sub = "SnapSpeed";
    else if (name.find("support") != std::string::npos)
        sub = "Support";
    return {type, sub};
}

std::string SnapmakerPrinterAgent::build_filament_config_gcode(const HeadWrite& w, bool force)
{
    std::ostringstream g;
    g << "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=" << w.head;
    if (w.set_type) {
        const auto type = firmware_type(w.type, w.name);
        g << " VENDOR=" << gcode_param(w.vendor, "Generic") << " FILAMENT_TYPE=" << gcode_param(type.first, "PLA")
          << " FILAMENT_SUBTYPE=" << gcode_param(type.second, "NONE");
    }
    std::vector<std::string> cols;
    for (const std::string& c : w.colors)
        if (const std::string v = hex6(c); !v.empty())
            cols.push_back(v);
    if (cols.size() >= 2 && cols.size() == w.colors.size()) {
        if (cols.size() > 4)
            cols.resize(4);
        g << " COLOR_NUMS=" << cols.size() << " COLORS=" << boost::join(cols, ",") << " MULTI_MODE=" << (w.gradient ? 1 : 0) << " ALPHA=255";
    } else if (const std::string c = hex6(w.color); !c.empty()) {
        g << " FILAMENT_COLOR_RGBA=" << c << "FF";
    }
    if (force)
        g << " FORCE=1";
    return g.str();
}

std::string SnapmakerPrinterAgent::build_extruder_map_gcode(const std::vector<int>& heads)
{
    std::ostringstream g;
    std::set<int>      used;
    for (size_t i = 0; i < heads.size() && int(i) < LOGICAL_EXTRUDERS; ++i)
        if (heads[i] >= 0 && heads[i] < HEAD_COUNT) {
            g << "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=" << i << " MAP_EXTRUDER=" << heads[i] << "\n";
            used.insert(heads[i]);
        }
    if (!used.empty()) {
        std::vector<std::string> list;
        for (int h : used)
            list.push_back(std::to_string(h));
        g << "SET_PRINT_USED_EXTRUDERS EXTRUDERS=" << boost::join(list, ",") << "\n";
    }
    return g.str();
}

std::vector<int> SnapmakerPrinterAgent::heads_from_mapping(const std::string& ams_mapping_info)
{
    std::vector<int> out;
    const json       mapping = json::parse(ams_mapping_info, nullptr, false);
    if (!mapping.is_array())
        return out;
    for (const json& it : mapping) {
        int head = -1;
        if (it.is_object() && it.contains("ams") && it["ams"].is_number_integer()) {
            const int tray = it["ams"].get<int>();
            if (tray >= 0 && tray < HEAD_COUNT)
                head = tray;
        }
        out.push_back(head);
    }
    return out;
}

// ---- writes -----------------------------------------------------------------------------------------

std::string SnapmakerPrinterAgent::write_block_reason() const
{
    const U1State s = u1_state();
    if (!s.valid)
        return _u8L("the U1 has not answered yet");
    if (s.busy_printing())
        return _u8L("a print is running");
    if (!s.klippy_state.empty() && s.klippy_state != "ready")
        return _u8L("the printer is not ready") + " (" + s.klippy_state + ")";
    if (m_ctl->write_busy.load())
        return _u8L("another change is still being written");
    return {};
}

void SnapmakerPrinterAgent::run_write(std::string script, int timeout_s, std::string ok_text, std::string fail_text)
{
    const std::string base_url = device_info.base_url, api_key = device_info.api_key;
    auto              ctl = m_ctl;
    ctl->write_busy       = true;
    std::thread([base_url, api_key, script = std::move(script), timeout_s, ok_text = std::move(ok_text), fail_text = std::move(fail_text), ctl] {
        std::string error;
        const bool  ok = post_script(base_url, api_key, script, timeout_s, error);
        ctl->write_busy = false;
        ctl->wake       = true;
        ctl->cv.notify_all();
        if (ok) {
            if (!ok_text.empty())
                CrealityCfsAgent::notify(ok_text);
        } else {
            CrealityCfsAgent::notify(fail_text + (error.empty() ? std::string() : ": " + error), true);
        }
    }).detach();
}

std::string SnapmakerPrinterAgent::write_head(const HeadWrite& w, bool force_confirmed)
{
    if (const std::string reason = write_block_reason(); !reason.empty())
        return reason;
    if (w.head < 0 || w.head >= HEAD_COUNT)
        return _u8L("invalid head");
    const U1State s = u1_state();
    const U1Head& h = s.heads[size_t(w.head)];
    if (!h.present)
        return _u8L("no filament in this head");
    if (h.official && !force_confirmed)
        return _u8L("official Snapmaker spool: it needs the user's confirmation");
    const bool        force  = h.official; // FORCE=1 only for a locked head, and only after the caller confirmed
    const std::string script = build_filament_config_gcode(w, force);
    BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent: " << script;
    run_write(script, 20, _u8L("Snapmaker U1") + " T" + std::to_string(w.head + 1) + ": " + _u8L("filament updated on the printer"),
              _u8L("Snapmaker U1") + " T" + std::to_string(w.head + 1) + ": " + _u8L("the printer did not accept the change"));
    return {};
}

std::string SnapmakerPrinterAgent::write_heads(const std::vector<HeadWrite>& writes, int* written)
{
    if (written != nullptr)
        *written = 0;
    if (const std::string reason = write_block_reason(); !reason.empty())
        return reason;
    const U1State s = u1_state();
    std::string   script, names;
    int           count = 0;
    for (const HeadWrite& w : writes) {
        if (w.head < 0 || w.head >= HEAD_COUNT)
            continue;
        const U1Head& h = s.heads[size_t(w.head)];
        if (!h.present || h.official) // an official spool is only ever overridden by an explicit, confirmed write_head()
            continue;
        script += build_filament_config_gcode(w, false) + "\n";
        names += (names.empty() ? "T" : ", T") + std::to_string(w.head + 1);
        ++count;
    }
    if (count == 0)
        return {};
    BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent: " << script;
    run_write(script, 30, _u8L("Snapmaker U1") + " " + names + ": " + _u8L("filament updated on the printer"),
              _u8L("Snapmaker U1") + " " + names + ": " + _u8L("the printer did not accept the change"));
    if (written != nullptr)
        *written = count;
    return {};
}

std::string SnapmakerPrinterAgent::feed_head(int head, FeedAction action)
{
    if (const std::string reason = write_block_reason(); !reason.empty())
        return reason;
    if (head < 0 || head >= HEAD_COUNT)
        return _u8L("invalid head");
    const U1State s = u1_state();
    const U1Head& h = s.heads[size_t(head)];
    if (s.feed_busy())
        return _u8L("filament is already moving");
    if (action == FeedAction::load && !h.feed_detected)
        return _u8L("no filament detected at the feeder of this head");
    if (action == FeedAction::unload && !h.present && !h.feed_detected)
        return _u8L("no filament in this head");
    const std::string script = "AUTO_FEEDING EXTRUDER=" + std::to_string(head) + (action == FeedAction::load ? " LOAD=1" : " UNLOAD=1");
    BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent: " << script;
    const std::string label = _u8L("Snapmaker U1") + " T" + std::to_string(head + 1) + ": ";
    // Moonraker answers when the move is over (minutes): long timeout, the poll loop shows the progress meanwhile.
    run_write(script, 900, label + (action == FeedAction::load ? _u8L("filament loaded") : _u8L("filament unloaded")),
              label + (action == FeedAction::load ? _u8L("loading failed") : _u8L("unloading failed")));
    return {};
}

// ---- printing ---------------------------------------------------------------------------------------

int SnapmakerPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // The print dialog's mapping (project filament -> tray = head) becomes the printer's extruder map before the file is
    // uploaded and started. Without a mapping the printer keeps the map it has.
    const std::vector<int> heads = heads_from_mapping(params.ams_mapping_info);
    if (std::any_of(heads.begin(), heads.end(), [](int h) { return h >= 0; })) {
        U1State     state;
        std::string error;
        if (!query_u1(device_info.base_url, device_info.api_key, state, error)) {
            CrealityCfsAgent::notify(_u8L("Snapmaker U1") + ": " + _u8L("could not read the printer, the print was not sent") + " (" + error + ")", true);
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
        if (state.busy_printing()) {
            CrealityCfsAgent::notify(_u8L("Snapmaker U1") + ": " + _u8L("a print is running"), true);
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
        if (update_fn)
            update_fn(PrintingStageUpload, 0, _u8L("Setting the extruder map..."));
        const std::string script = build_extruder_map_gcode(heads);
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent: extruder map " << script;
        if (!post_script(device_info.base_url, device_info.api_key, script, 15, error)) {
            CrealityCfsAgent::notify(_u8L("Snapmaker U1") + ": " + _u8L("the extruder map was refused") + " (" + error + ")", true);
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
    }
    return MoonrakerPrinterAgent::start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

} // namespace Slic3r
