// Orca Zoukei: feeds the printer Console (DeviceActivityLog) from the Bambu agent. Pure logic, no wx and no
// MachineObject: the GUI glue (GUI/BambuConsole.cpp) hands over the merged `device/<sn>/report` JSON, the command JSON
// Orca publishes on `device/<sn>/request`, and the translations / lookups (stage text, HMS and print_error
// descriptions come from the tables the Orca monitor already uses).
//
// Only changes are recorded (diff against the previous report of the same printer): print state, stage, progress (every
// layer change / every 10 %), print_error, HMS items (new / resolved), AMS step, AMS tray in use / previous / target,
// slots inserted / removed / type or colour changed, the external spool. Temperatures and fans are telemetry and are
// recorded only while the console's "show all" switch is on.
//
// Never recorded: access code, passwords, tokens, certificates (see DeviceActivityLog::redact).
#ifndef slic3r_Utils_BambuActivity_hpp_
#define slic3r_Utils_BambuActivity_hpp_

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace DeviceActivity { namespace Bambu {

struct Lookup
{
    // UI translation of an English message fragment; identity when empty.
    std::function<std::string(const char*)> tr;
    // stg_cur -> readable text (Orca's get_stage_string); a fallback "stage N" is used when empty / returns "".
    std::function<std::string(int)> stage;
    // 16-hex-digit long HMS code (module, number, part, 00000, level, code) -> description, "" when unknown.
    std::function<std::string(const std::string&)> hms;
    // print_error -> description, "" when unknown.
    std::function<std::string(int)> print_error;
};

// The merged (full-state) JSON of one report. Cheap when nothing changed.
void on_report(const std::string& host, const nlohmann::json& report, const Lookup& lk);
// A command Orca published (the JSON handed to publish_json); `result` is the publish return code (0 = ok).
void on_command(const std::string& host, const nlohmann::json& command, int result, const Lookup& lk);
// MQTT link state, as reported by the network agent: 0 = connected, 1 = failed (msg "5" = wrong access code), 2 = lost.
void on_connect_state(const std::string& host, int state, const std::string& msg, const Lookup& lk);
void on_online(const std::string& host, bool online, const Lookup& lk);
void on_connecting(const std::string& host, bool lan_mode, const Lookup& lk);
// Forgets the diff baseline of a printer (the next report is logged as a new baseline).
void forget(const std::string& host);

// "0500-0300-0002-0001" from the hms item's attr / code.
std::string hms_display_code(unsigned attr, unsigned code);
// The 16-hex long code Orca's HMS lookup uses (module, number, part, 00000, level, code).
std::string hms_long_code(unsigned attr, unsigned code);
// "AMS A1" for tray ids 0..15, external spool for 254 / 255.
std::string tray_name(long long tray, const Lookup& lk);

}}} // namespace Slic3r::DeviceActivity::Bambu

#endif // slic3r_Utils_BambuActivity_hpp_
