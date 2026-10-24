#include <catch2/catch_all.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/BambuActivity.hpp"
#include "slic3r/Utils/DeviceActivityLog.hpp"

using namespace Slic3r;
namespace DA = Slic3r::DeviceActivity;
namespace BA = Slic3r::DeviceActivity::Bambu;
using json   = nlohmann::json;

namespace {

BA::Lookup lookup()
{
    BA::Lookup lk;
    lk.stage       = [](int s) { return s == 6 ? std::string("Paused (filament ran out)") : s == 0 ? std::string("Printing") : std::string(); };
    lk.hms         = [](const std::string& code) { return code == "0700200000030001" ? std::string("AMS A: the filament has run out") : std::string(); };
    lk.print_error = [](int e) { return e == 0x07008001 ? std::string("Filament runs out in the AMS") : std::string(); };
    return lk;
}

// What an A1 mini (N2S) pushes in LAN mode, trimmed to the fields the console reads.
json report(const std::string& state, int stg, int pct, int layer, const std::string& tray_now, const std::string& a2_type = "PETG", int print_error = 0,
            json hms = json::array())
{
    json trays = json::array();
    trays.push_back({{"id", "0"}, {"tray_type", "PLA"}, {"tray_color", "FF0000FF"}, {"tray_sub_brands", "Bambu PLA Basic"}, {"remain", 80}});
    trays.push_back({{"id", "1"}, {"tray_type", a2_type}, {"tray_color", "00FF00FF"}, {"remain", 50}});
    trays.push_back({{"id", "2"}});
    trays.push_back({{"id", "3"}, {"tray_type", "PLA"}, {"tray_color", "0000FFFF"}});
    json unit = {{"id", "0"}, {"tray", trays}};
    json ams  = {{"tray_now", tray_now}, {"tray_pre", "255"}, {"tray_tar", tray_now}, {"ams", json::array({unit})}};
    json p    = {{"command", "push_status"},
                 {"msg", 0},
                 {"gcode_state", state},
                 {"subtask_name", "benchy"},
                 {"stg_cur", stg},
                 {"mc_percent", pct},
                 {"layer_num", layer},
                 {"total_layer_num", 120},
                 {"mc_remaining_time", 42},
                 {"print_error", print_error},
                 {"hms", hms},
                 {"nozzle_temper", 219.6},
                 {"bed_temper", 60.2},
                 {"ams_status", 0},
                 {"ams", ams}};
    return json{{"print", p}};
}

std::string all_text(const std::string& host)
{
    std::string out;
    for (const auto& e : DA::entries(host))
        out += e.text + "\n" + e.raw + "\n";
    return out;
}

std::size_t count_containing(const std::string& host, const std::string& needle)
{
    std::size_t n = 0;
    for (const auto& e : DA::entries(host))
        n += e.text.find(needle) != std::string::npos;
    return n;
}

} // namespace

TEST_CASE("BambuActivity: only changes of the report are logged", "[BambuActivity]")
{
    const std::string host = "bambu-test-diff";
    DA::clear(host);
    BA::forget(host);
    DA::set_show_all(false);
    const auto lk = lookup();

    BA::on_report(host, report("IDLE", 255, 0, 0, "255"), lk);
    REQUIRE(DA::entries(host).size() == 1); // baseline only
    CHECK(count_containing(host, "AMS A1 PLA #FF0000") == 1);
    CHECK(count_containing(host, "AMS A3 empty") == 1);

    // the same report (telemetry may differ) does not add anything
    json same = report("IDLE", 255, 0, 0, "255");
    same["print"]["nozzle_temper"] = 21.0;
    BA::on_report(host, same, lk);
    CHECK(DA::entries(host).size() == 1);

    BA::on_report(host, report("RUNNING", 0, 1, 1, "1"), lk);
    CHECK(count_containing(host, "Print state: Printing [RUNNING] - benchy") == 1);
    CHECK(count_containing(host, "AMS tray: in use AMS A2") == 1);
    CHECK(count_containing(host, "Progress: 1%, layer 1/120") == 1);

    // next layer, same 10 % bucket: one progress line
    const auto before = DA::entries(host).size();
    BA::on_report(host, report("RUNNING", 0, 2, 2, "1"), lk);
    CHECK(DA::entries(host).size() == before + 1);
    // same layer, same bucket: nothing
    BA::on_report(host, report("RUNNING", 0, 3, 2, "1"), lk);
    CHECK(DA::entries(host).size() == before + 1);
    // 10 % boundary on the same layer: a line
    BA::on_report(host, report("RUNNING", 0, 10, 2, "1"), lk);
    CHECK(DA::entries(host).size() == before + 2);
}

TEST_CASE("BambuActivity: stage, errors, HMS and AMS events", "[BambuActivity]")
{
    const std::string host = "bambu-test-events";
    DA::clear(host);
    BA::forget(host);
    DA::set_show_all(false);
    const auto lk = lookup();

    BA::on_report(host, report("RUNNING", 0, 20, 5, "1"), lk);
    DA::clear(host);

    json hms = json::array({json{{"attr", 0x07002000u}, {"code", 0x00030001u}}});
    BA::on_report(host, report("PAUSE", 6, 20, 5, "255", "PETG", 0x07008001, hms), lk);
    CHECK(count_containing(host, "Print state: Paused [PAUSE]") == 1);
    CHECK(count_containing(host, "Stage: Paused (filament ran out) (6)") == 1);
    CHECK(count_containing(host, "Print error 0700-8001: Filament runs out in the AMS") == 1);
    CHECK(count_containing(host, "HMS 0700-2000-0003-0001 [common]: AMS A: the filament has run out") == 1);
    CHECK(count_containing(host, "AMS tray: in use external spool / none (255)") == 1);
    bool errors = false;
    for (const auto& e : DA::entries(host))
        errors = errors || (e.source == DA::Source::Error && e.text.find("Print error") == 0);
    CHECK(errors);

    // same report again: nothing new
    const auto n = DA::entries(host).size();
    BA::on_report(host, report("PAUSE", 6, 20, 5, "255", "PETG", 0x07008001, hms), lk);
    CHECK(DA::entries(host).size() == n);

    // error and HMS gone, slot 2 changed type, slot 3 loaded
    json r = report("RUNNING", 0, 20, 5, "1", "ABS");
    r["print"]["ams"]["ams"][0]["tray"][2] = json{{"id", "2"}, {"tray_type", "TPU"}, {"tray_color", "FFFFFFFF"}};
    r["print"]["ams_status"]                = 0x0105; // filament change, pushing the new filament
    BA::on_report(host, r, lk);
    CHECK(count_containing(host, "Print error cleared (0700-8001)") == 1);
    CHECK(count_containing(host, "HMS resolved: 0700-2000-0003-0001") == 1);
    CHECK(count_containing(host, "AMS A2: changed PETG #00FF00 -> ABS #00FF00") == 1);
    CHECK(count_containing(host, "AMS A3: filament inserted TPU #FFFFFF") == 1);
    CHECK(count_containing(host, "AMS: changing filament - pushing the new filament") == 1);

    // slot removed
    json r2 = r;
    r2["print"]["ams"]["ams"][0]["tray"][3] = json{{"id", "3"}};
    BA::on_report(host, r2, lk);
    CHECK(count_containing(host, "AMS A4: filament removed (PLA #0000FF)") == 1);
}

TEST_CASE("BambuActivity: telemetry only when show all is on", "[BambuActivity]")
{
    const std::string host = "bambu-test-telemetry";
    DA::clear(host);
    BA::forget(host);
    DA::set_show_all(false);
    BA::on_report(host, report("IDLE", 255, 0, 0, "255"), lookup());
    CHECK(count_containing(host, "Telemetry") == 0);

    DA::set_show_all(true);
    BA::on_report(host, report("IDLE", 255, 0, 0, "255"), lookup());
    CHECK(count_containing(host, "Telemetry: nozzle=220, bed=60") == 1);
    DA::set_show_all(false);
}

TEST_CASE("BambuActivity: commands are summarised and replies logged", "[BambuActivity]")
{
    const std::string host = "bambu-test-commands";
    DA::clear(host);
    BA::forget(host);
    DA::set_show_all(false);
    const auto lk = lookup();

    BA::on_command(host, json{{"print", {{"command", "gcode_line"}, {"param", "M104 S0\n"}, {"sequence_id", "20001"}}}}, 0, lk);
    BA::on_command(host, json{{"print", {{"command", "pause"}, {"sequence_id", "20002"}}}}, 0, lk);
    BA::on_command(host, json{{"print", {{"command", "ams_change_filament"}, {"target", 1}, {"curr_temp", 220}, {"tar_temp", 240}, {"sequence_id", "3"}}}}, 0, lk);
    BA::on_command(host, json{{"print", {{"command", "ams_change_filament"}, {"target", 255}, {"curr_temp", 220}, {"tar_temp", 220}}}}, 0, lk);
    BA::on_command(host, json{{"print", {{"command", "ams_filament_setting"}, {"ams_id", 0}, {"tray_id", 2}, {"tray_type", "PLA"}, {"tray_color", "FF8800FF"}}}}, 0, lk);
    BA::on_command(host, json{{"print", {{"command", "resume"}}}}, -2, lk);
    BA::on_command(host, json{{"pushing", {{"command", "pushall"}}}}, 0, lk); // polling: silent
    CHECK(count_containing(host, "Command: G-code: M104 S0") == 1);
    CHECK(count_containing(host, "Command: Pause print") == 1);
    CHECK(count_containing(host, "Command: AMS change filament: load AMS A2, 220 -> 240 C") == 1);
    CHECK(count_containing(host, "Command: AMS change filament: unload") == 1);
    CHECK(count_containing(host, "Command: AMS filament setting: AMS A3 PLA #FF8800") == 1);
    CHECK(count_containing(host, "Command: Resume print - failed (-2)") == 1);
    CHECK(count_containing(host, "pushall") == 0);

    // replies
    BA::on_report(host, json{{"print", {{"command", "ams_change_filament"}, {"result", "fail"}, {"reason", "tray empty"}}}}, lk);
    CHECK(count_containing(host, "Printer reply: ams_change_filament -> fail (tray empty)") == 1);
    BA::on_report(host, json{{"print", {{"command", "gcode_line"}, {"result", "success"}}}}, lk);
    CHECK(count_containing(host, "Printer reply: gcode_line -> success") == 1);
}

TEST_CASE("BambuActivity: access code, passwords and certificates are never recorded", "[BambuActivity]")
{
    const std::string host = "bambu-test-secrets";
    DA::clear(host);
    BA::forget(host);
    DA::set_show_all(true); // the most verbose mode must not leak either
    const auto lk = lookup();

    json cmd = {{"command", "gcode_line"}, {"param", "M117 hi"}, {"dev_access_code", "12345678"}, {"access_code", "87654321"}, {"cert", "-----BEGIN"},
                {"password", "hunter2"}, {"token", "TKN123"}, {"nested", {{"Access_Code", "AC999999"}}}};
    BA::on_command(host, json{{"print", cmd}}, 0, lk);
    BA::on_command(host, json{{"system", {{"command", "get_access_code"}}}}, 0, lk);
    json rep = report("RUNNING", 0, 5, 1, "1");
    rep["print"]["ipcam"] = json{{"rtsp_url", "rtsps://bblp:SECRETCODE@10.10.1.246:322/streaming/live/1"}, {"ipcam_dev", "1"}};
    rep["print"]["access_code"] = "11112222";
    rep["print"]["cert_id"]     = "CERTSECRET";
    BA::on_report(host, rep, lk);
    BA::on_report(host, json{{"system", {{"command", "get_access_code"}, {"result", "success"}, {"access_code", "55556666"}}}}, lk);
    BA::on_connect_state(host, 1, "5", lk);
    DA::add(host, DA::Source::Agent, "connecting rtsps://bblp:URLSECRET@10.10.1.246/x with access_code=CODE4242 and certificate: CERTVAL");

    const std::string text = all_text(host);
    for (const char* secret : {"12345678", "87654321", "-----BEGIN", "hunter2", "TKN123", "AC999999", "SECRETCODE", "11112222", "CERTSECRET", "55556666",
                               "URLSECRET", "CODE4242", "CERTVAL"})
        CHECK(text.find(secret) == std::string::npos);
    CHECK(text.find("M117 hi") != std::string::npos);
    CHECK(count_containing(host, "wrong access code") == 1);
    DA::set_show_all(false);
}

TEST_CASE("BambuActivity: HMS code formats", "[BambuActivity]")
{
    CHECK(BA::hms_display_code(0x0C000300u, 0x00020001u) == "0C00-0300-0002-0001");
    CHECK(BA::hms_long_code(0x07002000u, 0x00030001u) == "0700200000030001");
    BA::Lookup lk;
    CHECK(BA::tray_name(0, lk) == "AMS A1");
    CHECK(BA::tray_name(7, lk) == "AMS B4");
}
