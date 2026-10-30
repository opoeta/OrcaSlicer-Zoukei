#include "DeviceActivityLog.hpp"

#include "Http.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

namespace Slic3r { namespace DeviceActivity {

using json = nlohmann::json;

namespace {

constexpr std::size_t MAX_RAW      = 3000; // chars kept in an entry's raw
constexpr std::size_t MAX_STRING   = 400;  // longest string value kept inside raw
constexpr std::size_t MAX_TEXT     = 600;
constexpr std::size_t MAX_KLIPPER_LINE = 400;

struct Dev
{
    std::deque<Entry>                 ring;
    std::uint64_t                     next_seq{1};
    std::uint64_t                     dropped{0};
    std::map<std::string, std::string> last;  // websocket: top-level key -> last value (dump)
    std::map<std::string, std::string> slots; // websocket: "box/slot" -> summary
    bool                              have_slots{false};
    long long                         k_offset{-1};
    std::string                       k_partial;
    bool                              k_down{false};
};

std::mutex                s_mutex;
std::map<std::string, Dev> s_devs;
std::atomic<bool>         s_show_all{false};

long long now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool secret_key(const std::string& key)
{
    const std::string k = lower(key);
    static const char* parts[] = {"token", "passw", "pwd", "cookie", "secret", "authoriz", "apikey", "api_key", "api-key", "credential",
                                  "bearer", "sessionid", "session_id", "privatekey", "private_key", "signature", "access_key", "refresh",
                                  // Bambu LAN: access code of the printer, certificates, signed URLs
                                  "access_code", "accesscode", "access-code", "cert", "rtsp_url"};
    for (const char* p : parts)
        if (k.find(p) != std::string::npos)
            return true;
    return k == "auth" || k == "sn_key"; // not "key": the K2 error object has {"errcode":..,"key":..}
}

std::string cut(const std::string& s, std::size_t n)
{
    if (s.size() <= n)
        return s;
    std::size_t end = n;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) // do not split a UTF-8 sequence
        --end;
    return s.substr(0, end) + "... (" + std::to_string(s.size()) + " chars)";
}

json redact_impl(const json& j, int depth)
{
    if (depth > 12)
        return "...";
    if (j.is_object()) {
        json out = json::object();
        for (auto it = j.begin(); it != j.end(); ++it)
            out[it.key()] = secret_key(it.key()) ? json("***") : redact_impl(it.value(), depth + 1);
        return out;
    }
    if (j.is_array()) {
        json out = json::array();
        std::size_t n = 0;
        for (const json& v : j) {
            if (++n > 200) {
                out.push_back("... (" + std::to_string(j.size()) + " items)");
                break;
            }
            out.push_back(redact_impl(v, depth + 1));
        }
        return out;
    }
    if (j.is_string())
        return redact_text(cut(j.get<std::string>(), MAX_STRING));
    return j;
}

std::string iso_utc(long long ms)
{
    const std::time_t t = std::time_t(ms / 1000);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, int(ms % 1000));
    return buf;
}

std::string local_time(long long ms)
{
    const std::time_t t = std::time_t(ms / 1000);
    std::tm           tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms % 1000));
    return buf;
}

std::function<void(const std::string&, const std::string&)> s_alert_handler;
std::map<std::string, long long>                             s_alert_last; // "host|code" -> unix ms

// Calls the handler unless the same code fired for this host within 2 minutes. Never call with s_mutex held.
void raise_alert(const std::string& key, const std::string& text)
{
    const std::string l = lower(text);
    const char*       codes[] = {"key846", "speed_err", "key353"};
    std::string       code    = "alert";
    for (const char* c : codes)
        if (l.find(c) != std::string::npos) {
            code = c;
            break;
        }
    std::function<void(const std::string&, const std::string&)> handler;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        long long&                  last = s_alert_last[key + "|" + code];
        const long long             now  = now_unix_ms();
        if (now - last < 120000)
            return;
        last    = now;
        handler = s_alert_handler;
    }
    if (handler)
        handler(key, text);
}

void push(const std::string& host, Source source, std::string text, std::string raw)
{
    const std::string key = key_of(host);
    if (key.empty())
        return;
    text = cut(redact_text(text), MAX_TEXT);
    Entry e;
    e.t_ms   = now_unix_ms();
    e.source = source;
    e.text   = std::move(text);
    e.raw    = std::move(raw);
    e.alert  = is_alert_text(e.text);
    const std::string alert_text = e.alert ? e.text : std::string();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        Dev& d = s_devs[key];
        e.seq  = d.next_seq++;
        d.ring.push_back(std::move(e));
        while (d.ring.size() > CAPACITY) {
            d.ring.pop_front();
            ++d.dropped;
        }
    }
    if (!alert_text.empty())
        raise_alert(key, alert_text);
}

std::string raw_of(const json& j)
{
    if (j.is_null())
        return {};
    std::string s = redact_impl(j, 0).dump();
    return s.size() > MAX_RAW ? cut(s, MAX_RAW) : s;
}

std::string val_str(const json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_null())
        return "null";
    return v.dump();
}

std::string brief(const json& v, std::size_t n = 100) { return cut(redact_text(v.is_string() ? v.get<std::string>() : v.dump()), n); }

// Telemetry that changes all the time: only logged with "show all".
bool noisy_key(const std::string& key)
{
    static const std::set<std::string> exact = {"nozzleTemp", "bedTemp0", "bedTemp1", "bedTemp2", "boxTemp", "curPosition", "realTimeFlow",
                                                "usedMaterialLength", "printJobTime", "printLeftTime", "curFeedratePct", "heart_beat",
                                                "chamberTemp", "mcuTemp", "cpuTemp", "memUsage", "cpuUsage", "ts", "timestamp", "time"};
    if (exact.count(key))
        return true;
    const std::string k = lower(key);
    if (k.find("target") != std::string::npos)
        return false;
    return k.find("temp") != std::string::npos || k.find("fan") != std::string::npos || k.find("position") != std::string::npos ||
           k.find("feedrate") != std::string::npos || k.find("speed") != std::string::npos;
}

// "box1/slot2: vendor type name #COLOR st=.. pct=.." for each CFS slot of a boxsInfo.
std::map<std::string, std::string> slot_summaries(const json& boxs)
{
    std::map<std::string, std::string> out;
    if (!boxs.is_object() || !boxs.contains("materialBoxs") || !boxs["materialBoxs"].is_array())
        return out;
    for (const json& box : boxs["materialBoxs"]) {
        if (!box.is_object())
            continue;
        const std::string bid = box.contains("id") ? val_str(box["id"]) : "?";
        if (!box.contains("materials") || !box["materials"].is_array())
            continue;
        for (const json& mat : box["materials"]) {
            if (!mat.is_object())
                continue;
            auto f = [&](const char* k) { return mat.contains(k) ? val_str(mat[k]) : std::string(); };
            std::string s = f("vendor") + " " + f("type") + " " + f("name") + " " + f("color");
            s += " state=" + f("state") + " sel=" + f("selected");
            if (mat.contains("rfid") && !f("rfid").empty() && f("rfid") != "0")
                s += " rfid";
            if (mat.contains("editStatus"))
                s += " edit=" + f("editStatus");
            out["box" + bid + "/slot" + f("id")] = s;
        }
    }
    return out;
}

} // namespace

const char* source_name(Source s)
{
    switch (s) {
    case Source::WsOut: return "ws-out";
    case Source::WsIn: return "ws-in";
    case Source::Sync: return "sync";
    case Source::Agent: return "agent";
    case Source::Error: return "error";
    case Source::Klipper: return "klipper";
    default: return "agent";
    }
}

unsigned source_mask(const std::string& csv)
{
    unsigned mask = 0;
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        std::size_t end = csv.find(',', pos);
        if (end == std::string::npos)
            end = csv.size();
        std::string name = lower(csv.substr(pos, end - pos));
        while (!name.empty() && std::isspace((unsigned char) name.back()))
            name.pop_back();
        while (!name.empty() && std::isspace((unsigned char) name.front()))
            name.erase(name.begin());
        for (unsigned i = 0; i < unsigned(Source::Count); ++i)
            if (name == source_name(Source(i)))
                mask |= 1u << i;
        pos = end + 1;
    }
    return mask == 0 ? ~0u : mask;
}

std::string key_of(const std::string& host_or_url)
{
    std::string host = host_or_url;
    const auto  scheme = host.find("://");
    if (scheme != std::string::npos)
        host = host.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    const auto at = host.rfind('@');
    if (at != std::string::npos)
        host = host.substr(at + 1); // never keep user:password
    if (!host.empty() && host[0] != '[') {
        const auto colon = host.find(':');
        if (colon != std::string::npos)
            host = host.substr(0, colon);
    }
    while (!host.empty() && std::isspace((unsigned char) host.back()))
        host.pop_back();
    while (!host.empty() && std::isspace((unsigned char) host.front()))
        host.erase(host.begin());
    return lower(host);
}

json redact(const json& j) { return redact_impl(j, 0); }

std::string redact_text(const std::string& s)
{
    static const std::regex re(
        R"((token|password|passwd|pwd|cookie|secret|api[_-]?key|authorization|bearer|x-api-key|sessionid|access[_-]?code|dev_access_code|cert[a-z_]*)(["']?\s*[:=]\s*["']?)([^"'\s,&}]+))",
        std::regex::icase | std::regex::optimize);
    static const std::regex bearer(R"((bearer\s+)[A-Za-z0-9._~+/=-]{8,})", std::regex::icase | std::regex::optimize);
    static const std::regex userinfo(R"((://[^/\s:@"']+:)[^/\s@"']+@)", std::regex::optimize); // rtsps://bblp:ACCESSCODE@host
    // Fast path: most entries mention none of these words.
    const std::string l = lower(s);
    static const char* words[] = {"token", "passw", "pwd", "cookie", "secret", "api", "key", "authoriz", "bearer", "session", "access", "cert", "://"};
    bool any = false;
    for (const char* w : words)
        if (l.find(w) != std::string::npos) {
            any = true;
            break;
        }
    if (!any)
        return s;
    try {
        return std::regex_replace(std::regex_replace(std::regex_replace(s, userinfo, "$1***@"), bearer, "$1***"), re, "$1$2***");
    } catch (...) {
        return "(unreadable text hidden)";
    }
}

void add(const std::string& host, Source source, const std::string& text, const json& raw)
{
    push(host, source, text, raw_of(raw));
}

void add_raw_text(const std::string& host, Source source, const std::string& text, const std::string& raw_text)
{
    push(host, source, text, raw_text.empty() ? std::string() : cut(redact_text(raw_text), MAX_RAW));
}

bool show_all() { return s_show_all.load(); }
void set_show_all(bool on) { s_show_all = on; }

void ws_event(const std::string& host, const std::string& text, bool is_error)
{
    push(host, is_error ? Source::Error : Source::Agent, text, {});
}

void ws_message(const std::string& host, bool outgoing, const std::string& text)
{
    const std::string key = key_of(host);
    if (key.empty())
        return;
    const bool all = show_all();
    if (outgoing) {
        json msg = json::parse(text, nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) {
            if (all || (text != "ok" && !text.empty()))
                push(host, Source::WsOut, text == "ok" ? "ok (heartbeat reply)" : cut(text, 200), {});
            return;
        }
        const std::string method = msg.contains("method") ? val_str(msg["method"]) : std::string("?");
        const json        params = msg.contains("params") ? msg["params"] : json();
        if (!all && method == "get" && params.is_object()) {
            bool periodic = !params.empty();
            for (auto it = params.begin(); it != params.end(); ++it)
                if (it.key() != "boxsInfo")
                    periodic = false;
            if (periodic)
                return; // the 15 s poll
        }
        push(host, Source::WsOut, method + " " + (params.is_null() ? std::string() : brief(redact_impl(params, 0), 300)), raw_of(msg));
        return;
    }

    json msg = json::parse(text, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) {
        if (text.find("heart_beat") != std::string::npos) {
            if (all)
                push(host, Source::WsIn, "heart_beat", {});
        } else if (!text.empty())
            push(host, Source::WsIn, cut(text, 200), {});
        return;
    }

    std::vector<std::string> parts;
    json                     changed = json::object();
    std::vector<std::string> notes; // extra lines (boxsInfo diff)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        Dev& d = s_devs[key];
        for (auto it = msg.begin(); it != msg.end(); ++it) {
            const std::string& k = it.key();
            if (k == "heart_beat") {
                if (all)
                    parts.push_back("heart_beat");
                continue;
            }
            const json&       v    = it.value();
            const std::string dump = v.dump();
            if (k == "boxsInfo") {
                auto now = slot_summaries(v);
                std::string diff;
                if (!d.have_slots) {
                    diff = "boxsInfo: " + std::to_string(now.size()) + " slot(s)";
                    for (auto& [slot, s] : now)
                        diff += "\n  " + slot + ": " + s;
                } else {
                    for (auto& [slot, s] : now) {
                        auto old = d.slots.find(slot);
                        if (old == d.slots.end())
                            diff += "\n  " + slot + " appeared: " + s;
                        else if (old->second != s)
                            diff += "\n  " + slot + ": " + old->second + "  ->  " + s;
                    }
                    for (auto& [slot, s] : d.slots)
                        if (!now.count(slot))
                            diff += "\n  " + slot + " gone (was " + s + ")";
                    if (!diff.empty())
                        diff = "boxsInfo changed:" + diff;
                }
                d.slots      = std::move(now);
                d.have_slots = true;
                if (!diff.empty() || all) {
                    notes.push_back(diff.empty() ? "boxsInfo (unchanged, " + std::to_string(d.slots.size()) + " slot(s))" : diff);
                    changed[k] = v;
                }
                d.last[k] = std::to_string(dump.size()) + ":" + dump.substr(0, 64);
                continue;
            }
            auto last = d.last.find(k);
            const bool same = last != d.last.end() && last->second == dump;
            d.last[k]       = dump;
            if (same && !all)
                continue;
            if (noisy_key(k) && !all)
                continue;
            if (k == "retMaterials" && v.is_array()) {
                parts.push_back("retMaterials: " + std::to_string(v.size()) + " entries");
                changed[k] = json("(" + std::to_string(v.size()) + " entries)");
                continue;
            }
            parts.push_back(k + "=" + brief(v));
            changed[k] = v;
        }
    }
    if (!parts.empty()) {
        std::string line;
        for (const std::string& p : parts)
            line += (line.empty() ? "" : "  ") + p;
        // keep boxsInfo out of the raw when it is shown in `notes`
        push(host, Source::WsIn, line, raw_of(changed.contains("boxsInfo") && !notes.empty() ? [&] { json c = changed; c.erase("boxsInfo"); return c; }() : changed));
    }
    for (const std::string& n : notes)
        push(host, Source::WsIn, n, all || parts.empty() ? raw_of(changed) : std::string());
}

std::vector<Entry> entries(const std::string& host, std::uint64_t since)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<Entry>          out;
    auto                        it = s_devs.find(key_of(host));
    if (it == s_devs.end())
        return out;
    for (const Entry& e : it->second.ring)
        if (e.seq > since)
            out.push_back(e);
    return out;
}

json query(const std::string& host, std::uint64_t since, std::size_t limit, unsigned sources, const std::string& text, bool include_raw)
{
    limit = std::clamp<std::size_t>(limit, 1, CAPACITY);
    const std::string needle = lower(text);
    std::vector<Entry> picked;
    std::uint64_t      dropped = 0, total = 0, last_seq = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto                        it = s_devs.find(key_of(host));
        if (it != s_devs.end()) {
            dropped  = it->second.dropped;
            total    = it->second.next_seq - 1;
            last_seq = total;
            auto match = [&](const Entry& e) {
                if (!(sources & (1u << unsigned(e.source))))
                    return false;
                return needle.empty() || lower(e.text).find(needle) != std::string::npos || (include_raw && lower(e.raw).find(needle) != std::string::npos);
            };
            if (since == 0) {
                for (auto r = it->second.ring.rbegin(); r != it->second.ring.rend() && picked.size() < limit; ++r)
                    if (match(*r))
                        picked.push_back(*r);
                std::reverse(picked.begin(), picked.end());
            } else {
                for (const Entry& e : it->second.ring)
                    if (e.seq > since && match(e)) {
                        picked.push_back(e);
                        if (picked.size() >= limit)
                            break;
                    }
            }
        }
    }
    json list = json::array();
    for (const Entry& e : picked) {
        json j = {{"seq", e.seq}, {"t", e.t_ms}, {"time", iso_utc(e.t_ms)}, {"source", source_name(e.source)}, {"text", e.text}};
        if (include_raw && !e.raw.empty())
            j["raw"] = e.raw;
        list.push_back(std::move(j));
    }
    // `next`: where to continue. When the page was filled up to the limit, continue after the last returned entry.
    std::uint64_t next = last_seq;
    if (since != 0 && picked.size() >= limit)
        next = picked.back().seq;
    if (since != 0 && picked.empty() && next < since)
        next = since;
    return {{"host", key_of(host)}, {"entries", std::move(list)}, {"next", next}, {"dropped", dropped}, {"total", total},
            {"show_all", show_all()}};
}

std::string to_text(const std::string& host, unsigned sources, const std::string& text)
{
    const std::string needle = lower(text);
    std::string       out;
    for (const Entry& e : entries(host)) {
        if (!(sources & (1u << unsigned(e.source))))
            continue;
        if (!needle.empty() && lower(e.text).find(needle) == std::string::npos)
            continue;
        out += local_time(e.t_ms) + " [" + source_name(e.source) + "] " + e.text + "\n";
        if (!e.raw.empty())
            out += "    " + e.raw + "\n";
    }
    return out;
}

void clear(const std::string& host)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    auto                        it = s_devs.find(key_of(host));
    if (it == s_devs.end())
        return;
    it->second.ring.clear();
    it->second.dropped = 0;
    it->second.last.clear();
    it->second.slots.clear();
    it->second.have_slots = false;
}

std::vector<std::string> hosts()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<std::string>    out;
    for (const auto& [k, d] : s_devs)
        out.push_back(k);
    return out;
}

// ---- Klipper -----------------------------------------------------------------------------------------------------

bool klipper_line_relevant(const std::string& line)
{
    const std::string l = lower(line);
    static const char* words[] = {"error",    "exception", "traceback", "shutdown",   "timer too close", "key code", "keycode", "key_code",
                                  "codex",    "dxc2",      "tnn",       "lost communication", "mcu '", "unable to", "!!", "command error",
                                  "firmware", "emergency", "overheat",  "thermistor", "heater", "stalled", "not homed", "must home",
                                  "rescheduling", "invalid", "failed", "timeout", "rejected"};
    for (const char* w : words)
        if (l.find(w) != std::string::npos)
            return true;
    return false;
}

int klipper_poll(const std::string& host_in, int port)
{
    const std::string key = key_of(host_in);
    if (key.empty())
        return -1;
    // One poll per host at a time (the console and the background poller both call this).
    static std::set<std::string> s_inflight;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_inflight.insert(key).second)
            return 0;
    }
    struct Release { const std::string& k; ~Release() { std::lock_guard<std::mutex> lock(s_mutex); s_inflight.erase(k); } } release{key};
    long long offset;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        offset = s_devs[key].k_offset;
    }
    const std::string url = "http://" + key + ":" + std::to_string(port) + "/server/files/logs/klippy.log";
    std::string       body, headers, error;
    unsigned          status = 0;
    Http http = Http::get(url);
    http.timeout_connect(3).timeout_max(8).size_limit(1024 * 1024);
    http.set_range(offset < 0 ? "-65536" : std::to_string(offset) + "-");
    http.on_header_callback([&](std::string h) { headers += h; })
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) { body = std::move(b); error = std::move(e); status = s; });
    try {
        http.perform_sync();
    } catch (const std::exception& e) {
        error = e.what();
    }

    // Content-Range: bytes a-b/total  |  bytes */total
    long long first = -1, last = -1, total = -1;
    {
        const std::string h = lower(headers);
        const auto        p = h.find("content-range:");
        if (p != std::string::npos) {
            const auto eol  = h.find('\n', p);
            const std::string v = h.substr(p + 14, eol == std::string::npos ? std::string::npos : eol - p - 14);
            const auto        b = v.find("bytes");
            if (b != std::string::npos) {
                long long a = -1, z = -1, t = -1;
                if (std::sscanf(v.c_str() + b, "bytes %lld-%lld/%lld", &a, &z, &t) == 3) {
                    first = a;
                    last  = z;
                    total = t;
                } else if (std::sscanf(v.c_str() + b, "bytes */%lld", &t) == 1)
                    total = t;
            }
        }
    }

    std::unique_lock<std::mutex> lock(s_mutex);
    Dev& d = s_devs[key];
    if (status == 416) { // nothing new, or the log was rotated (shorter than our offset)
        if (total >= 0 && total < d.k_offset) {
            d.k_offset = -1;
            d.k_partial.clear();
        }
        d.k_down = false;
        return 0;
    }
    if (!error.empty() || status == 0 || status >= 400) {
        if (!d.k_down) {
            d.k_down = true;
            Entry e;
            e.t_ms   = now_unix_ms();
            e.source = Source::Error;
            e.text   = "klippy.log unavailable (" + (error.empty() ? "HTTP " + std::to_string(status) : cut(error, 120)) + ")";
            e.seq    = d.next_seq++;
            d.ring.push_back(std::move(e));
        }
        return -1;
    }
    d.k_down = false;
    std::string data = std::move(body);
    if (status == 200 && first < 0) { // server ignored Range: it sent the whole file
        if (offset >= 0 && (long long) data.size() >= offset) {
            const long long size = (long long) data.size();
            data = data.substr(std::size_t(offset));
            first = offset;
            last  = size - 1;
        } else {
            const std::size_t keep = std::min<std::size_t>(data.size(), 65536);
            first = (long long) (data.size() - keep);
            last  = (long long) data.size() - 1;
            data  = data.substr(data.size() - keep);
        }
    }
    if (last >= 0)
        d.k_offset = last + 1;
    else if (first >= 0)
        d.k_offset = first + (long long) data.size();
    else
        d.k_offset = std::max<long long>(0, offset) + (long long) data.size();

    std::string text = d.k_partial + data;
    d.k_partial.clear();
    const bool initial = offset < 0;
    if (initial) { // the suffix starts mid-line
        const auto nl = text.find('\n');
        text          = nl == std::string::npos ? std::string() : text.substr(nl + 1);
    }
    const auto last_nl = text.rfind('\n');
    if (last_nl == std::string::npos) {
        d.k_partial = text.size() > 8192 ? std::string() : text;
        return 0;
    }
    d.k_partial = text.substr(last_nl + 1);
    text.resize(last_nl);

    int         added = 0;
    std::size_t pos   = 0;
    std::vector<std::string> alerts;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos              = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || !klipper_line_relevant(line))
            continue;
        Entry e;
        e.t_ms   = now_unix_ms();
        e.source = Source::Klipper;
        e.text   = cut(redact_text(line), MAX_KLIPPER_LINE);
        e.alert  = is_alert_text(e.text);
        if (e.alert && !initial) // the first read is history, not news
            alerts.push_back(e.text);
        e.seq    = d.next_seq++;
        d.ring.push_back(std::move(e));
        ++added;
        if (d.ring.size() > CAPACITY) {
            d.ring.pop_front();
            ++d.dropped;
        }
    }
    lock.unlock();
    for (const std::string& a : alerts)
        raise_alert(key, a);
    return added;
}

bool is_alert_text(const std::string& text)
{
    const std::string l = lower(text);
    static const char* words[] = {"key846", "speed_err", "key353", "empty printing", "stepper too far in past"};
    for (const char* w : words)
        if (l.find(w) != std::string::npos)
            return true;
    return false;
}

void set_alert_handler(std::function<void(const std::string&, const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_alert_handler = std::move(handler);
}

void start_background_poll()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true))
        return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            for (const std::string& h : hosts())
                klipper_poll(h);
        }
    }).detach();
}

}} // namespace Slic3r::DeviceActivity
