// Orca Zoukei: in-memory activity log of the printers ("Console" of the device panels and
// GET /api/v1/devices/<id>/activity). One ring buffer (2000 entries) per device, keyed by the
// printer's host (IP or name, scheme and port dropped). Fed by the printer agents (websocket traffic of the
// Creality link, agent events, errors), the project -> printer auto-syncs (which filament, which slot, why, wrote
// or skipped) and an optional tail of Klipper's klippy.log.
//
// Thread-safe; every function may be called from any thread. Nothing secret is stored: JSON keys that look like
// tokens, passwords, cookies, keys or authorisation are replaced by "***" before an entry is kept, and long strings
// are cut. Periodic traffic (heartbeats, polls, temperature telemetry, unchanged values) is skipped unless "show all"
// is on, so the log shows changes and one-off messages.
#ifndef slic3r_Utils_DeviceActivityLog_hpp_
#define slic3r_Utils_DeviceActivityLog_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace DeviceActivity {

enum class Source : unsigned { WsOut = 0, WsIn, Sync, Agent, Error, Klipper, Count };

// "ws-out", "ws-in", "sync", "agent", "error", "klipper"
const char* source_name(Source s);
// Bit mask from "ws-in,sync"; empty or unknown -> all sources.
unsigned    source_mask(const std::string& csv);

constexpr std::size_t CAPACITY = 2000;

struct Entry
{
    std::uint64_t seq{0};
    long long     t_ms{0}; // Unix time, milliseconds
    Source        source{Source::Agent};
    std::string   text;
    std::string   raw; // redacted JSON (or text), may be empty
    bool          alert{false}; // key846 / SPEED_ERR / key353 / pause: highlighted and notified
};

// "http://10.1.2.3:7125/x" -> "10.1.2.3" (lower case). The key every function below takes as `host`.
std::string key_of(const std::string& host_or_url);

// Appends an entry. `raw` is redacted (JSON keys) and cut; null = no raw.
void add(const std::string& host, Source source, const std::string& text, const nlohmann::json& raw = nullptr);
void add_raw_text(const std::string& host, Source source, const std::string& text, const std::string& raw_text);
inline void error(const std::string& host, const std::string& text, const nlohmann::json& raw = nullptr) { add(host, Source::Error, text, raw); }
inline void sync(const std::string& host, const std::string& text, const nlohmann::json& raw = nullptr) { add(host, Source::Sync, text, raw); }
inline void agent(const std::string& host, const std::string& text, const nlohmann::json& raw = nullptr) { add(host, Source::Agent, text, raw); }

// One websocket frame of a printer link, outgoing ({"method":..,"params":..} or "ok") or incoming. Summarised: only
// changes and one-off messages are kept (see the file comment); "show all" keeps every frame.
void ws_message(const std::string& host, bool outgoing, const std::string& text);
// A link event: connected / disconnected / failed.
void ws_event(const std::string& host, const std::string& text, bool is_error = false);

// {"entries":[{seq,t,time,source,text,raw?}],"next":N,"dropped":N,"total":N,"host":..}
//  since == 0: the newest `limit` entries; since > 0: the oldest `limit` entries with seq > since (to follow the log).
nlohmann::json query(const std::string& host, std::uint64_t since, std::size_t limit, unsigned sources = ~0u, const std::string& text = {},
                     bool include_raw = false);
std::vector<Entry> entries(const std::string& host, std::uint64_t since = 0);
// The log as plain text (one line per entry), for copy / export.
std::string        to_text(const std::string& host, unsigned sources = ~0u, const std::string& text = {});
void               clear(const std::string& host);
std::vector<std::string> hosts();

bool show_all();
void set_show_all(bool on);

// Redacts a JSON value in place of a copy: secret-looking keys -> "***", long strings cut.
nlohmann::json redact(const nlohmann::json& j);
// Masks "token=...", "password: ..." etc. inside free text.
std::string    redact_text(const std::string& s);

// ---- Klipper klippy.log ------------------------------------------------------------------------------------------
// Fetches what klippy.log (http://<host>:7125/server/files/logs/klippy.log) gained since the last call, using Range
// requests (the first call reads the last 64 KiB), and adds the relevant lines (errors, shutdown, key codes, Codex /
// DXC2 / tnn, "Timer too close"...) as Source::Klipper. Blocking: call from a worker thread. Returns the number of
// lines added, or -1 on a network error (also logged as an error entry, once per outage).
int  klipper_poll(const std::string& host, int port = 7125);
bool klipper_line_relevant(const std::string& line);

// ---- alerts --------------------------------------------------------------------------------------------------------
// True for lines that mean "the print stopped / needs you": key846 (CFS empty printing), SPEED_ERR, key353 (MCU shutdown),
// a pause. Such entries get Entry::alert and call the handler (any thread; at most once per host and code per 2 minutes).
bool is_alert_text(const std::string& text);
void set_alert_handler(std::function<void(const std::string& host, const std::string& text)> handler);
// Polls klippy.log of every known host every 20 s so alerts arrive with the console closed. Idempotent.
void start_background_poll();

}} // namespace Slic3r::DeviceActivity

#endif // slic3r_Utils_DeviceActivityLog_hpp_
