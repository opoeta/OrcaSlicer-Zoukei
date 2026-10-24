#include "BambuActivity.hpp"

#include "DeviceActivityLog.hpp"
#include "BmcuHms.hpp"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace Slic3r { namespace DeviceActivity { namespace Bambu {

using json = nlohmann::json;

namespace {

struct Pending
{
    Source      source;
    std::string text;
    json        raw;
};

struct State
{
    bool                               init{false};
    std::string                        gcode_state;
    int                                stage{-2};
    int                                bucket{-1};
    long long                          layer{-1};
    long long                          print_error{0};
    bool                               have_hms{false};
    std::map<std::string, json>        hms; // display code -> {attr, code}
    long long                          ams_status{-1};
    std::string                        tray_now, tray_pre, tray_tar;
    std::map<std::string, std::string> slots; // "A1" / "Ext" -> signature ("PLA #FF0000 Name"), "" = empty
    std::string                        telemetry;
};

std::mutex                 s_mutex;
std::map<std::string, State> s_state;

std::string T(const Lookup& lk, const char* s) { return lk.tr ? lk.tr(s) : std::string(s); }

bool get_int(const json& j, const char* key, long long& out)
{
    if (!j.is_object())
        return false;
    auto it = j.find(key);
    if (it == j.end())
        return false;
    if (it->is_number()) {
        out = it->is_number_float() ? static_cast<long long>(it->get<double>()) : it->get<long long>();
        return true;
    }
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char*             end = nullptr;
        const long long   v   = std::strtoll(s.c_str(), &end, 10);
        if (end != s.c_str()) {
            out = v;
            return true;
        }
    }
    return false;
}

bool get_str(const json& j, const char* key, std::string& out)
{
    if (!j.is_object())
        return false;
    auto it = j.find(key);
    if (it == j.end())
        return false;
    if (it->is_string()) {
        out = it->get<std::string>();
        return true;
    }
    if (it->is_number_integer() || it->is_number_unsigned()) {
        out = std::to_string(it->get<long long>());
        return true;
    }
    return false;
}

std::string cut(const std::string& s, std::size_t n)
{
    if (s.size() <= n)
        return s;
    std::size_t end = n;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
        --end;
    return s.substr(0, end) + "...";
}

std::string hex8(long long v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%08llX", static_cast<unsigned long long>(v) & 0xFFFFFFFFull);
    std::string s(buf);
    s.insert(4, "-");
    return s;
}

const char* gcode_state_msgid(const std::string& s)
{
    if (s == "IDLE")
        return "Idle";
    if (s == "PREPARE")
        return "Preparing";
    if (s == "RUNNING")
        return "Printing";
    if (s == "PAUSE")
        return "Paused";
    if (s == "FINISH")
        return "Finished";
    if (s == "FAILED")
        return "Failed";
    if (s == "SLICING")
        return "Slicing";
    if (s == "INIT")
        return "Starting";
    return nullptr;
}

// Stages where the printer stopped for a problem the user has to look at.
bool stage_is_problem(int stg)
{
    switch (stg) {
    case 6: case 17: case 20: case 21: case 23: case 26: case 27: return true;
    default: return false;
    }
}

std::string stage_text(int stg, const Lookup& lk)
{
    if (stg == 255 || stg == -1)
        return T(lk, "Idle");
    std::string t = lk.stage ? lk.stage(stg) : std::string();
    if (t.empty())
        t = T(lk, "Stage") + " " + std::to_string(stg);
    return t;
}

std::string ams_main_text(long long main, const Lookup& lk)
{
    switch (main) {
    case 0x00: return T(lk, "idle");
    case 0x01: return T(lk, "changing filament");
    case 0x02: return T(lk, "identifying RFID");
    case 0x03: return T(lk, "assist");
    case 0x04: return T(lk, "calibrating");
    case 0x07: return T(lk, "cold pull");
    case 0x10: return T(lk, "self check");
    case 0x20: return T(lk, "debug");
    default: return T(lk, "status") + " 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%02llX", main & 0xFF); return std::string(b); }();
    }
}

std::string ams_sub_text(long long sub, const Lookup& lk)
{
    switch (sub) {
    case 0x02: return T(lk, "heating the nozzle");
    case 0x03: return T(lk, "cutting the filament");
    case 0x04: return T(lk, "pulling the current filament back");
    case 0x05: return T(lk, "pushing the new filament");
    case 0x06: return T(lk, "confirming the filament was extruded");
    case 0x07: return T(lk, "purging the old filament");
    case 0x08: return T(lk, "checking the position");
    case 0x09: return T(lk, "waiting");
    case 0x0B: return T(lk, "checking the position");
    default: return {};
    }
}

std::string colour_of(const json& tray)
{
    std::string c;
    if (get_str(tray, "tray_color", c) && c.size() >= 6)
        return "#" + c.substr(0, 6);
    return {};
}

// Slot signature: type, colour, filament name. Empty when the slot holds no filament.
std::string slot_signature(const json& tray)
{
    std::string type, name, info;
    get_str(tray, "tray_type", type);
    get_str(tray, "tray_sub_brands", name);
    get_str(tray, "tray_info_idx", info);
    const std::string colour = colour_of(tray);
    if (type.empty() && colour.empty() && name.empty() && info.empty())
        return {};
    std::string sig = type.empty() ? std::string("?") : type;
    if (!colour.empty())
        sig += " " + colour;
    if (!name.empty() && name != type)
        sig += " (" + cut(name, 40) + ")";
    return sig;
}

std::string level_text(unsigned level, const Lookup& lk)
{
    switch (level) {
    case 1: return T(lk, "fatal");
    case 2: return T(lk, "serious");
    case 3: return T(lk, "common");
    case 4: return T(lk, "info");
    default: return T(lk, "unknown");
    }
}

void emit(const std::string& host, const std::vector<Pending>& list)
{
    for (const Pending& p : list)
        add(host, p.source, p.text, p.raw);
}

// ams.ams[] and the external spool -> slot signatures.
void collect_slots(const json& p, std::map<std::string, std::string>& out, bool& have_ams, bool& have_ext)
{
    have_ams = have_ext = false;
    auto it = p.find("ams");
    if (it != p.end() && it->is_object()) {
        auto list = it->find("ams");
        if (list != it->end() && list->is_array()) {
            have_ams = true;
            for (const json& unit : *list) {
                long long id = 0;
                if (!get_int(unit, "id", id))
                    continue;
                auto trays = unit.find("tray");
                if (trays == unit.end() || !trays->is_array())
                    continue;
                for (const json& tray : *trays) {
                    long long slot = 0;
                    if (!get_int(tray, "id", slot))
                        continue;
                    out[id >= 0 && id < 4 ? std::string("AMS ") + char('A' + id) + std::to_string(slot + 1) :
                                           "AMS#" + std::to_string(id) + "." + std::to_string(slot + 1)] = slot_signature(tray);
                }
            }
        }
    }
    auto vt = p.find("vt_tray");
    if (vt != p.end() && vt->is_object()) {
        have_ext  = true;
        out["Ext"] = slot_signature(*vt);
    }
    auto vir = p.find("vir_slot");
    if (vir != p.end() && vir->is_array()) {
        for (const json& tray : *vir) {
            long long id = 0;
            if (!get_int(tray, "id", id))
                continue;
            have_ext = true;
            out["Ext " + std::to_string(id)] = slot_signature(tray);
        }
    }
}

std::string slots_summary(const std::map<std::string, std::string>& slots, const Lookup& lk)
{
    std::string out;
    for (const auto& kv : slots) {
        if (!out.empty())
            out += "; ";
        out += kv.first + " " + (kv.second.empty() ? T(lk, "empty") : kv.second);
    }
    return out;
}

std::string telemetry_line(const json& p, std::string& sig)
{
    static const struct { const char* key; const char* label; } keys[] = {
        {"nozzle_temper", "nozzle"}, {"nozzle_target_temper", "nozzle target"}, {"bed_temper", "bed"}, {"bed_target_temper", "bed target"},
        {"chamber_temper", "chamber"}, {"cooling_fan_speed", "part fan"}, {"big_fan1_speed", "aux fan"}, {"big_fan2_speed", "chamber fan"},
        {"heatbreak_fan_speed", "heatbreak fan"}, {"spd_lvl", "speed level"}, {"wifi_signal", "wifi"}};
    std::string line;
    for (const auto& k : keys) {
        auto it = p.find(k.key);
        if (it == p.end())
            continue;
        std::string v;
        if (it->is_number())
            v = std::to_string(static_cast<long long>(it->get<double>() + (it->get<double>() >= 0 ? 0.5 : -0.5)));
        else if (it->is_string())
            v = it->get<std::string>();
        else
            continue;
        if (!line.empty())
            line += ", ";
        line += std::string(k.label) + "=" + v;
    }
    sig = line;
    return line;
}

bool silent_command(const std::string& name)
{
    static const std::set<std::string> silent = {"pushall", "get_version", "get_access_code", "start", "keepalive", "request_push_all", "get_history"};
    return silent.count(name) != 0;
}

} // namespace

// ---- helpers exposed ---------------------------------------------------------------------------------------------

std::string hms_display_code(unsigned attr, unsigned code)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04X-%04X-%04X-%04X", (attr >> 16) & 0xFFFF, attr & 0xFFFF, (code >> 16) & 0xFFFF, code & 0xFFFF);
    return buf;
}

std::string hms_long_code(unsigned attr, unsigned code)
{
    const unsigned level = code >> 16;
    char           buf[64];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X00000%1X%04X", (attr >> 24) & 0xFF, (attr >> 16) & 0xFF, (attr >> 8) & 0xFF,
                  level < 16 ? level : 0u, code & 0xFFFF);
    return buf;
}

std::string tray_name(long long tray, const Lookup& lk)
{
    if (tray == 255)
        return T(lk, "external spool / none") + " (255)";
    if (tray == 254)
        return T(lk, "external spool 2") + " (254)";
    if (tray >= 0 && tray < 16)
        return std::string("AMS ") + char('A' + tray / 4) + std::to_string(tray % 4 + 1);
    return T(lk, "tray") + " " + std::to_string(tray);
}

void forget(const std::string& host)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_state.erase(key_of(host));
}

// ---- report ------------------------------------------------------------------------------------------------------

namespace {

void report_ack(const std::string& host, const std::string& group, const json& body, const std::string& cmd, const Lookup& lk)
{
    std::string result, reason;
    get_str(body, "result", result);
    get_str(body, "reason", reason);
    const bool show = show_all();
    if (silent_command(cmd) && !show && result != "fail" && result != "failed")
        return;
    if (result.empty() && !show)
        return; // an echo / status frame without a verdict
    const bool failed = result == "fail" || result == "failed" || result == "error";
    std::string text  = T(lk, "Printer reply") + ": " + cmd;
    if (!result.empty())
        text += " -> " + result;
    if (!reason.empty())
        text += " (" + cut(reason, 160) + ")";
    json raw = nullptr;
    if (cmd != "get_access_code") { // the reply of get_access_code carries the code
        raw = json::object();
        raw[group] = body;
    }
    add(host, failed ? Source::Error : Source::WsIn, text, raw);
}

} // namespace

void on_report(const std::string& host, const json& report, const Lookup& lk)
{
    if (!report.is_object() || key_of(host).empty())
        return;

    // Replies of the printer to commands (and other non-status frames).
    static const char* groups[] = {"print", "system", "info", "xcam", "camera", "pushing", "upgrade"};
    auto               pit      = report.find("print");
    if (pit == report.end() || !pit->is_object()) {
        for (const char* g : groups) {
            auto it = report.find(g);
            std::string cmd;
            if (it != report.end() && it->is_object() && get_str(*it, "command", cmd))
                report_ack(host, g, *it, cmd, lk);
        }
        return;
    }
    const json& p = *pit;
    {
        std::string cmd;
        if (get_str(p, "command", cmd) && cmd != "push_status") {
            report_ack(host, "print", p, cmd, lk);
            return;
        }
    }
    for (const char* g : {"system", "info"}) { // a frame can carry both
        auto it = report.find(g);
        std::string cmd;
        if (it != report.end() && it->is_object() && get_str(*it, "command", cmd))
            report_ack(host, g, *it, cmd, lk);
    }

    std::vector<Pending> out;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        State&                      s    = s_state[key_of(host)];
        const bool                  base = !s.init;

        std::string job;
        if (!get_str(p, "subtask_name", job) || job.empty())
            get_str(p, "gcode_file", job);

        // --- print state
        std::string st;
        if (get_str(p, "gcode_state", st) && st != s.gcode_state) {
            if (!base) {
                const char* id = gcode_state_msgid(st);
                std::string text = T(lk, "Print state") + ": " + (id ? T(lk, id) : st) + " [" + st + "]";
                if (!job.empty() && st != "IDLE")
                    text += " - " + cut(job, 80);
                out.push_back({st == "FAILED" ? Source::Error : Source::WsIn, text, nullptr});
            }
            s.gcode_state = st;
        }

        // --- stage
        long long stg = 0;
        if (get_int(p, "stg_cur", stg) && int(stg) != s.stage) {
            if (!base && s.stage != -2)
                out.push_back({stage_is_problem(int(stg)) ? Source::Error : Source::WsIn,
                               T(lk, "Stage") + ": " + stage_text(int(stg), lk) + " (" + std::to_string(stg) + ")", nullptr});
            s.stage = int(stg);
        }

        // --- progress: every layer change and every 10 %
        long long pct = -1, layer = -1, total = -1, remaining = -1;
        get_int(p, "mc_percent", pct);
        get_int(p, "layer_num", layer);
        get_int(p, "total_layer_num", total);
        get_int(p, "mc_remaining_time", remaining);
        const int bucket = pct >= 0 ? int(pct / 10) : s.bucket;
        const bool layer_changed  = layer >= 0 && layer != s.layer;
        const bool bucket_changed = pct >= 0 && bucket != s.bucket;
        if (!base && (layer_changed || bucket_changed) && (layer > 0 || pct > 0)) {
            std::string text = T(lk, "Progress") + ": " + std::to_string(pct < 0 ? 0 : pct) + "%";
            if (layer >= 0)
                text += ", " + T(lk, "layer") + " " + std::to_string(layer) + (total > 0 ? "/" + std::to_string(total) : std::string());
            if (remaining >= 0)
                text += ", " + std::to_string(remaining) + " min " + T(lk, "left");
            out.push_back({Source::WsIn, text, nullptr});
        }
        if (layer >= 0)
            s.layer = layer;
        if (pct >= 0)
            s.bucket = bucket;

        // --- print_error
        long long perr = 0;
        if (get_int(p, "print_error", perr) && perr != s.print_error) {
            if (perr != 0) {
                std::string desc = lk.print_error ? lk.print_error(int(perr)) : std::string();
                const auto  fw_id = BmcuHms::fw_mismatch_ams_id_from_print_error(unsigned(perr));
                const bool  ignored = fw_id && lk.ignore_ams_fw_mismatch && lk.ignore_ams_fw_mismatch(*fw_id);
                out.push_back({ignored ? Source::WsIn : Source::Error,
                               T(lk, "Print error") + " " + hex8(perr) + (desc.empty() ? std::string() : ": " + cut(desc, 300)) +
                                   (ignored ? " (" + T(lk, "ignored (BMCU)") + ")" : std::string()),
                               json{{"print_error", perr}}});
            } else if (!base) {
                out.push_back({Source::WsIn, T(lk, "Print error cleared") + " (" + hex8(s.print_error) + ")", nullptr});
            }
            s.print_error = perr;
        }

        // --- HMS
        auto hit = p.find("hms");
        if (hit != p.end() && hit->is_array()) {
            std::map<std::string, json> now;
            for (const json& item : *hit) {
                if (!item.is_object() || !item.contains("attr") || !item.contains("code") || !item["attr"].is_number() || !item["code"].is_number())
                    continue;
                const unsigned attr = item["attr"].get<unsigned>(), code = item["code"].get<unsigned>();
                now[hms_display_code(attr, code)] = json{{"attr", attr}, {"code", code}};
            }
            for (const auto& kv : now) {
                if (s.hms.count(kv.first))
                    continue;
                const unsigned attr = kv.second["attr"].get<unsigned>(), code = kv.second["code"].get<unsigned>();
                const unsigned level = code >> 16;
                std::string    desc  = lk.hms ? lk.hms(hms_long_code(attr, code)) : std::string();
                const auto     fw_id = BmcuHms::fw_mismatch_ams_id_from_hms(attr, code);
                const bool     ignored = fw_id && lk.ignore_ams_fw_mismatch && lk.ignore_ams_fw_mismatch(*fw_id);
                out.push_back({!ignored && level >= 1 && level <= 3 ? Source::Error : Source::WsIn,
                               "HMS " + kv.first + " [" + level_text(level, lk) + "]" + (desc.empty() ? std::string() : ": " + cut(desc, 300)) +
                                   (ignored ? " (" + T(lk, "ignored (BMCU)") + ")" : std::string()),
                               kv.second});
            }
            if (!base)
                for (const auto& kv : s.hms)
                    if (!now.count(kv.first))
                        out.push_back({Source::WsIn, T(lk, "HMS resolved") + ": " + kv.first, nullptr});
            s.hms      = std::move(now);
            s.have_hms = true;
        }

        // --- AMS step
        long long ams_status = 0;
        if (get_int(p, "ams_status", ams_status) && ams_status != s.ams_status) {
            if (!base && (s.ams_status >= 0 || ams_status != 0)) {
                const long long main = (ams_status & 0xFF00) >> 8, sub = ams_status & 0xFF;
                std::string text = "AMS: " + ams_main_text(main, lk);
                if (main == 0x01) {
                    const std::string step = ams_sub_text(sub, lk);
                    if (!step.empty())
                        text += " - " + step;
                }
                out.push_back({Source::WsIn, text, nullptr});
            }
            s.ams_status = ams_status;
        }

        // --- AMS trays in use / previous / target
        auto ait = p.find("ams");
        if (ait != p.end() && ait->is_object()) {
            std::string now, pre, tar;
            const bool  have_now = get_str(*ait, "tray_now", now), have_pre = get_str(*ait, "tray_pre", pre), have_tar = get_str(*ait, "tray_tar", tar);
            if (!have_now) now = s.tray_now;
            if (!have_pre) pre = s.tray_pre;
            if (!have_tar) tar = s.tray_tar;
            (void) have_now; (void) have_pre; (void) have_tar;
            if (now != s.tray_now || pre != s.tray_pre || tar != s.tray_tar) {
                auto name = [&](const std::string& v) {
                    char* end = nullptr;
                    const long long n = std::strtoll(v.c_str(), &end, 10);
                    return end != v.c_str() ? tray_name(n, lk) : v;
                };
                if (!base) {
                    std::string text = T(lk, "AMS tray");
                    text += ": " + T(lk, "in use") + " " + (now.empty() ? "-" : name(now));
                    if (tar != s.tray_tar || (!tar.empty() && tar != now))
                        text += ", " + T(lk, "target") + " " + (tar.empty() ? "-" : name(tar));
                    if (pre != s.tray_pre)
                        text += ", " + T(lk, "previous") + " " + (pre.empty() ? "-" : name(pre));
                    out.push_back({Source::WsIn, text, json{{"tray_now", now}, {"tray_pre", pre}, {"tray_tar", tar}}});
                }
                s.tray_now = now;
                s.tray_pre = pre;
                s.tray_tar = tar;
            }
        }

        // --- slots
        std::map<std::string, std::string> slots;
        bool                               have_ams = false, have_ext = false;
        collect_slots(p, slots, have_ams, have_ext);
        if (!slots.empty()) {
            if (!base) {
                for (const auto& kv : slots) {
                    auto old = s.slots.find(kv.first);
                    const std::string before = old == s.slots.end() ? std::string() : old->second;
                    const bool        known  = old != s.slots.end();
                    if (known && before == kv.second)
                        continue;
                    std::string text;
                    if (kv.second.empty() && known)
                        text = kv.first + ": " + T(lk, "filament removed") + " (" + before + ")";
                    else if (before.empty() && known)
                        text = kv.first + ": " + T(lk, "filament inserted") + " " + kv.second;
                    else if (!known)
                        text = kv.first + ": " + (kv.second.empty() ? T(lk, "empty") : kv.second);
                    else
                        text = kv.first + ": " + T(lk, "changed") + " " + before + " -> " + kv.second;
                    out.push_back({Source::WsIn, text, nullptr});
                }
                if (have_ams)
                    for (const auto& kv : s.slots)
                        if (kv.first.compare(0, 3, "AMS") == 0 && !slots.count(kv.first))
                            out.push_back({Source::WsIn, kv.first + ": " + T(lk, "slot no longer reported"), nullptr});
            }
            for (const auto& kv : slots)
                s.slots[kv.first] = kv.second;
            if (have_ams)
                for (auto it = s.slots.begin(); it != s.slots.end();)
                    it = (it->first.compare(0, 3, "AMS") == 0 && !slots.count(it->first)) ? s.slots.erase(it) : std::next(it);
        }

        // --- telemetry (only when the console shows everything)
        if (show_all()) {
            std::string sig;
            const std::string line = telemetry_line(p, sig);
            if (!line.empty() && sig != s.telemetry) {
                out.push_back({Source::WsIn, T(lk, "Telemetry") + ": " + line, nullptr});
                s.telemetry = sig;
            }
        }

        // --- baseline: one line describing the first report of the session
        if (base) {
            std::string text = T(lk, "Printer report received") + ": " + T(lk, "Print state") + " " + s.gcode_state;
            if (s.stage != -2)
                text += ", " + T(lk, "Stage") + " " + stage_text(s.stage, lk);
            if (pct >= 0)
                text += ", " + std::to_string(pct) + "%";
            if (!job.empty() && s.gcode_state != "IDLE")
                text += ", " + cut(job, 60);
            if (!s.slots.empty())
                text += ". " + slots_summary(s.slots, lk);
            if (!s.tray_now.empty())
                text += ". " + T(lk, "AMS tray") + ": " + T(lk, "in use") + " " + [&] {
                    char* end = nullptr;
                    const long long n = std::strtoll(s.tray_now.c_str(), &end, 10);
                    return end != s.tray_now.c_str() ? tray_name(n, lk) : s.tray_now;
                }();
            out.insert(out.begin(), Pending{Source::Agent, text, nullptr});
            s.init = true;
        }
    }
    emit(host, out);
}

// ---- commands ----------------------------------------------------------------------------------------------------

namespace {

std::string compact_params(const json& body)
{
    std::string out;
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (it.key() == "command" || it.key() == "sequence_id")
            continue;
        if (!out.empty())
            out += ", ";
        std::string v = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        out += it.key() + "=" + cut(v, 80);
    }
    return cut(out, 240);
}

std::string command_summary(const std::string& group, const std::string& cmd, const json& body, const Lookup& lk)
{
    long long n = 0;
    std::string s;
    if (cmd == "gcode_line") {
        std::string g;
        get_str(body, "param", g);
        std::size_t lines = 1;
        for (char c : g)
            lines += c == '\n';
        const std::size_t nl = g.find('\n');
        return "G-code: " + cut(nl == std::string::npos ? g : g.substr(0, nl), 160) + (lines > 1 ? " (+" + std::to_string(lines - 1) + " " + T(lk, "more lines") + ")" : std::string());
    }
    if (cmd == "pause")
        return T(lk, "Pause print");
    if (cmd == "resume")
        return T(lk, "Resume print");
    if (cmd == "stop")
        return T(lk, "Stop print");
    if (cmd == "ams_change_filament") {
        std::string text = T(lk, "AMS change filament");
        if (get_int(body, "target", n))
            text += ": " + (n == 255 ? T(lk, "unload") : T(lk, "load") + " " + tray_name(n, lk));
        long long ct = 0, tt = 0;
        if (get_int(body, "curr_temp", ct) && get_int(body, "tar_temp", tt))
            text += ", " + std::to_string(ct) + " -> " + std::to_string(tt) + " C";
        return text;
    }
    if (cmd == "ams_filament_setting") {
        long long ams = 0, tray = 0;
        std::string type, colour;
        get_int(body, "ams_id", ams);
        get_int(body, "tray_id", tray);
        get_str(body, "tray_type", type);
        get_str(body, "tray_color", colour);
        std::string slot = ams == 255 || tray == 255 ? T(lk, "external spool") : tray_name(ams * 4 + tray, lk);
        if (ams == 254 || tray == 254)
            slot = T(lk, "external spool 2");
        return T(lk, "AMS filament setting") + ": " + slot + " " + type + (colour.size() >= 6 ? " #" + colour.substr(0, 6) : std::string());
    }
    if (cmd == "ams_control") {
        get_str(body, "param", s);
        return T(lk, "AMS control") + ": " + s;
    }
    if (cmd == "clean_print_error")
        return T(lk, "Clear print error");
    (void) group;
    const std::string params = compact_params(body);
    return cmd + (params.empty() ? std::string() : " (" + params + ")");
}

} // namespace

void on_command(const std::string& host, const json& command, int result, const Lookup& lk)
{
    if (!command.is_object() || key_of(host).empty())
        return;
    for (auto it = command.begin(); it != command.end(); ++it) {
        if (!it.value().is_object())
            continue;
        std::string cmd;
        if (!get_str(it.value(), "command", cmd))
            cmd = it.key();
        if (silent_command(cmd) && !show_all())
            continue;
        std::string text = T(lk, "Command") + ": " + command_summary(it.key(), cmd, it.value(), lk);
        if (result != 0)
            text += " - " + T(lk, "failed") + " (" + std::to_string(result) + ")";
        add(host, result != 0 ? Source::Error : Source::WsOut, text, command);
        return;
    }
}

// ---- link --------------------------------------------------------------------------------------------------------

void on_connect_state(const std::string& host, int state, const std::string& msg, const Lookup& lk)
{
    if (key_of(host).empty())
        return;
    if (state == 0) {
        add(host, Source::Agent, T(lk, "MQTT connected"));
    } else if (state == 1) {
        add(host, Source::Error,
            T(lk, "MQTT connection failed") + (msg == "5" ? " - " + T(lk, "wrong access code") : msg.empty() ? std::string() : " (" + cut(msg, 80) + ")"));
    } else if (state == 2) {
        add(host, Source::Error, T(lk, "MQTT connection lost"));
    } else {
        add(host, Source::Agent, T(lk, "MQTT state") + " " + std::to_string(state));
    }
}

void on_online(const std::string& host, bool online, const Lookup& lk)
{
    if (key_of(host).empty())
        return;
    add(host, online ? Source::Agent : Source::Error, online ? T(lk, "Printer online") : T(lk, "Printer offline"));
}

void on_connecting(const std::string& host, bool lan_mode, const Lookup& lk)
{
    if (key_of(host).empty())
        return;
    add(host, Source::Agent, lan_mode ? T(lk, "Connecting over LAN") : T(lk, "Connecting through the cloud"));
}

}}} // namespace Slic3r::DeviceActivity::Bambu
