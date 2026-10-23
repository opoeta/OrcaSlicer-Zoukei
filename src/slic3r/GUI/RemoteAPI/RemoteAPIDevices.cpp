// src/slic3r/GUI/RemoteAPI/RemoteAPIDevices.cpp
//
// Orca Zoukei Remote API, group G5: printers, printing and filament slots. Every handler runs on the UI thread
// (run_on_ui) and goes through the code the window itself uses: the DeviceManager / MachineObject commands (which
// reach the printer through its agent), the Snapmaker / Flashforge agents' own write and feed functions, and the
// print host job queue. Nothing talks to a printer directly.
//
// Read-only (always allowed):
//   GET  /api/v1/devices                        printers the app knows: agent, model, online, connected, selected, state
//   GET  /api/v1/devices/{id}                   one printer ("selected" = the selected one): state, progress, layer,
//                                               temperatures, remaining time, slots
//   GET  /api/v1/devices/{id}/slots             filament slots (Bambu AMS, Creality CFS, Flashforge IFS, Snapmaker U1
//                                               heads, external spool) with colour / type / lock / feed state
//   GET  /api/v1/print_hosts                    printer presets / physical printers with a print host (OctoPrint,
//                                               Moonraker, PrusaLink ...); credentials are never returned
//   GET  /api/v1/printer_control                {enabled}: the Preferences switch
//
// Control (needs Preferences > Remote API > "Allow printer control from the Remote API" AND "confirm": true in the
// request, else 403 blocked_by_remote_api_policy; each call is logged and answers with what was sent):
//   POST /api/v1/print/send                     {name?, plate?, start?, storage?, group?, confirm}
//                                               uploads the sliced plate to the selected printer preset's print host
//   POST /api/v1/devices/{id}/pause | resume | stop      {confirm}
//   POST /api/v1/devices/{id}/temperature       {nozzle?, bed?, chamber?, extruder?, confirm}
//   POST /api/v1/devices/{id}/clear_error       {confirm}: Creality only, dismisses the active printer error (websocket
//                                               {"method":"set","params":{"cleanErr":1}}, Creality Print's generic "OK")
//   POST /api/v1/devices/{id}/filament         {action: load|unload, unit?, slot, nozzle_temp?, confirm}
//   PUT  /api/v1/devices/{id}/slots             {unit?, slot, type?, color?, colors?, filament_id?, setting_id?, confirm}
//
// The printers' own rules stay in force: a Snapmaker U1 head with an official (locked) spool is never forced, slot
// writes and feeds are refused while a print runs, Flashforge slot writes need ZMOD, and so on.
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/CrealityCfsUi.hpp"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PostProcessor.hpp"
#include "slic3r/Utils/ASCIIFolding.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/FlashforgePrinterAgent.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

namespace {

// ---- policy ----------------------------------------------------------------------------------

bool control_enabled()
{
    return wxGetApp().app_config->get_bool("remote_api_printer_control");
}

// The two safeguards of every control action: the Preferences switch and an explicit "confirm": true.
void require_control(const json &body, const char *action)
{
    if (!control_enabled())
        zk::fail("blocked_by_remote_api_policy",
                 std::string("printer control is off (") + action + " refused). The owner must enable Preferences > Remote API > "
                 "\"Allow printer control from the Remote API\" in OrcaSlicer; it cannot be changed through the API.");
    const bool confirmed = body.is_object() && body.contains("confirm") && body["confirm"].is_boolean() && body["confirm"].get<bool>();
    if (!confirmed)
        zk::fail("blocked_by_remote_api_policy",
                 std::string(action) + " needs \"confirm\": true in the request. Ask the owner first and only then send it.");
}

void log_control(const char *action, const json &sent)
{
    BOOST_LOG_TRIVIAL(info) << "RemoteAPI printer control: " << action << " " << sent.dump();
}

// ---- helpers ---------------------------------------------------------------------------------

std::string u8(const wxString &s)
{
    return std::string(s.ToUTF8().data());
}

std::string hex6(const std::string &c)
{
    if (c.size() < 6)
        return std::string();
    std::string out = "#";
    for (size_t i = 0; i < 6; ++i)
        out += char(std::toupper(static_cast<unsigned char>(c[i])));
    return out;
}

DeviceManager *dev_manager()
{
    DeviceManager *dm = wxGetApp().getDeviceManager();
    if (dm == nullptr)
        zk::fail("no_device_manager", "the device manager is not available");
    return dm;
}

std::string agent_id_of(const MachineObject *m)
{
    return m->printer_agent_id;
}

bool is_selected(DeviceManager *dm, const MachineObject *m)
{
    return dm->get_selected_machine() == m;
}

MachineObject *need_machine(const std::string &id)
{
    DeviceManager *dm = dev_manager();
    if (id.empty() || id == "selected") {
        MachineObject *m = dm->get_selected_machine();
        if (m == nullptr)
            zk::fail("unknown_device", "no printer is selected in the window");
        return m;
    }
    const auto list = dm->get_my_machine_list("");
    auto it = list.find(id);
    if (it != list.end() && it->second != nullptr)
        return it->second;
    const auto local = dm->get_local_machinelist();
    auto it2 = local.find(id);
    if (it2 != local.end() && it2->second != nullptr)
        return it2->second;
    zk::fail("unknown_device", "no printer with this id; see GET /devices");
}

json tray_json(const DevAmsTray &t)
{
    json cols = json::array();
    for (const std::string &c : t.cols)
        cols.push_back(hex6(c));
    json j = {{"slot", t.id},
              {"has_filament", t.is_exists},
              {"empty_placeholder", t.is_slot_placeholder},
              {"type", t.m_fila_type},
              {"sub_brand", t.sub_brands},
              {"color", hex6(t.color)},
              {"colors", cols},
              {"filament_id", t.setting_id},
              {"setting_id", t.filament_setting_id},
              {"official_tag", t.is_bbl},
              {"remain_percent", t.remain},
              {"nozzle_temp_min", t.nozzle_temp_min},
              {"nozzle_temp_max", t.nozzle_temp_max},
              {"weight", t.weight},
              {"diameter", t.diameter}};
    return j;
}

const char *unit_kind(const std::string &agent)
{
    if (agent == "creality_cfs")
        return "CFS";
    if (agent == "flashforge")
        return "IFS";
    if (agent == "snapmaker")
        return "U1 heads";
    return "AMS";
}

json slots_json(MachineObject *m)
{
    json units = json::array();
    if (std::shared_ptr<DevFilaSystem> fs = m->GetFilaSystem()) {
        for (auto &kv : fs->GetAmsList()) {
            DevAms *ams = kv.second;
            if (ams == nullptr)
                continue;
            json trays = json::array();
            for (const auto &tv : ams->GetTrays())
                if (tv.second != nullptr)
                    trays.push_back(tray_json(*tv.second));
            json u = {{"unit", ams->GetAmsId()}, {"name", u8(ams->GetDisplayName())}, {"kind", unit_kind(agent_id_of(m))}, {"slots", trays}};
            if (ams->SupportHumidity())
                u["humidity_percent"] = ams->GetHumidityPercent();
            units.push_back(u);
        }
    }
    json ext = json::array();
    for (const DevAmsTray &t : m->vt_slot) {
        json j = tray_json(t);
        ext.push_back(j);
    }
    json out = {{"units", units}, {"external_spools", ext}, {"agent", agent_id_of(m)}};
    return out;
}

// Agent-specific detail of the SELECTED printer (Snapmaker U1 heads incl. the official-spool lock, Flashforge IFS).
json agent_details(DeviceManager *dm, MachineObject *m)
{
    json d = json::object();
    if (!is_selected(dm, m))
        return d;
    const std::string a = agent_id_of(m);
    if (a == SNAPMAKER_PRINTER_AGENT_ID) {
        if (auto ag = SnapmakerPrinterAgent::current()) {
            const SnapmakerPrinterAgent::U1State s = ag->u1_state();
            json heads = json::array();
            for (const auto &h : s.heads)
                heads.push_back({{"head", h.index}, {"has_filament", h.present}, {"official_locked", h.official}, {"editable", h.editable},
                                 {"vendor", h.vendor}, {"type", h.display_type()}, {"color", h.color}, {"colors", h.colors},
                                 {"nozzle_temp", h.nozzle_now}, {"nozzle_target", h.nozzle_target}, {"feeder_module", h.feed_module},
                                 {"filament_at_feeder", h.feed_detected}, {"feed_state", h.feed_state}, {"feed_action", h.feed_action},
                                 {"feed_error", h.feed_error}});
            d["snapmaker_u1"] = {{"valid", s.valid}, {"print_state", s.print_state}, {"klippy_state", s.klippy_state},
                                 {"bed", s.bed_now}, {"bed_target", s.bed_target}, {"heads", heads},
                                 {"write_blocked_reason", ag->write_block_reason()}};
        }
    } else if (a == FLASHFORGE_PRINTER_AGENT_ID) {
        if (auto ag = CfsUi::ifs_agent()) {
            json slots = json::array();
            for (const auto &s : ag->ifs_slots())
                slots.push_back({{"port", s.port}, {"has_filament", s.has_filament}, {"material", s.material}, {"color", s.color},
                                 {"name", s.name}, {"vendor", s.vendor}, {"nozzle_temp", s.nozzle_temp}, {"bed_temp", s.bed_temp},
                                 {"spool_id", s.spool_id}});
            const char *mode = ag->mode() == FlashforgePrinterAgent::Mode::zmod ? "zmod" : ag->mode() == FlashforgePrinterAgent::Mode::stock ? "stock" : "unresolved";
            d["flashforge_ifs"] = {{"mode", mode}, {"ui", ag->ui_name()}, {"channel", ag->current_channel()}, {"slots", slots},
                                   {"write_blocked_reason", ag->write_block_reason()}};
        }
    }
    return d;
}

json temps_json(MachineObject *m)
{
    json nozzles = json::array();
    if (DevExtderSystem *es = m->GetExtderSystem())
        for (const DevExtder &e : es->GetExtruders())
            nozzles.push_back({{"extruder", e.GetExtId()}, {"current", e.GetCurrentTemp()}, {"target", e.GetTargetTemp()},
                               {"diameter", e.GetNozzleDiameter()}, {"filament_in_extruder", e.HasFilamentInExt()}});
    json t = {{"nozzles", nozzles}};
    if (DevBed *b = m->GetBed()) {
        t["bed"] = b->GetBedTemp();
        t["bed_target"] = b->GetBedTempTarget();
    }
    t["chamber"] = m->chamber_temp;
    t["chamber_target"] = m->chamber_temp_target;
    return t;
}

json device_brief(DeviceManager *dm, MachineObject *m)
{
    return {{"id", m->get_dev_id()},
            {"name", m->get_dev_name()},
            {"model", m->printer_type},
            {"model_name", m->get_show_printer_type()},
            {"agent", agent_id_of(m)},
            {"ip", m->get_dev_ip()},
            {"connection", m->dev_connection_type},
            {"online", m->m_is_online},
            {"connected", m->is_connected()},
            {"has_access", m->has_access_right()},
            {"selected", is_selected(dm, m)},
            {"state", m->print_status},
            {"printing", m->is_in_printing()}};
}

json device_status(DeviceManager *dm, MachineObject *m)
{
    json j = device_brief(dm, m);
    j["progress_percent"] = m->mc_print_percent;
    j["layer"] = m->curr_layer;
    j["total_layers"] = m->total_layers;
    j["remaining_seconds"] = m->mc_left_time;
    j["speed_percent"] = m->printing_speed_mag;
    j["job"] = {{"name", m->subtask_name}, {"file", m->m_gcode_file}, {"plate", m->m_plate_index}};
    j["error_code"] = m->print_error;
    j["temperatures"] = temps_json(m);
    j["can"] = {{"pause", m->can_pause()}, {"resume", m->can_resume()}, {"stop", m->can_abort()}, {"print", m->can_print()}};
    j["slots"] = slots_json(m);
    json ad = agent_details(dm, m);
    if (!ad.empty())
        j["agent_details"] = ad;
    j["control_enabled"] = control_enabled();
    return j;
}

json op_list_devices()
{
    DeviceManager *dm = dev_manager();
    json arr = json::array();
    std::set<std::string> seen;
    for (auto &kv : dm->get_my_machine_list("")) {
        if (kv.second == nullptr)
            continue;
        arr.push_back(device_brief(dm, kv.second));
        seen.insert(kv.first);
    }
    for (auto &kv : dm->get_local_machinelist()) {
        if (kv.second == nullptr || seen.count(kv.first))
            continue;
        arr.push_back(device_brief(dm, kv.second));
    }
    return {{"devices", arr}, {"current_agent", dm->get_current_printer_agent_id()}, {"control_enabled", control_enabled()}};
}

json op_print_hosts()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    auto describe = [](const DynamicPrintConfig &cfg) {
        // Address and kind only; API keys, passwords and certificates stay inside the app.
        return json{{"host", cfg.has("print_host") ? cfg.opt_string("print_host") : std::string()},
                    {"host_type", cfg.has("host_type") ? cfg.opt_serialize("host_type") : std::string()},
                    {"use_3mf", cfg.has("use_3mf") && cfg.opt_bool("use_3mf")}};
    };
    json presets = json::array();
    for (const Preset &p : bundle->printers.get_presets()) {
        if (p.is_default || !p.is_visible || !p.config.has("print_host") || p.config.opt_string("print_host").empty())
            continue;
        json j = describe(p.config);
        j["preset"] = p.name;
        j["selected"] = p.name == bundle->printers.get_selected_preset_name();
        presets.push_back(j);
    }
    json physical = json::array();
    for (const PhysicalPrinter &pp : bundle->physical_printers) {
        json j = describe(pp.config);
        j["name"] = pp.name;
        j["presets"] = pp.preset_names;
        physical.push_back(j);
    }
    const DynamicPrintConfig &edited = bundle->printers.get_edited_preset().config;
    return {{"selected_preset", bundle->printers.get_selected_preset_name()},
            {"selected_has_print_host", edited.has("print_host") && !edited.opt_string("print_host").empty()},
            {"printer_presets", presets},
            {"physical_printers", physical},
            {"note", "POST /print/send uploads to the host of the SELECTED printer preset"}};
}

// ---- control ---------------------------------------------------------------------------------

json result_of(const char *action, MachineObject *m, const json &sent, int rc)
{
    json r = {{"action", action}, {"device", {{"id", m->get_dev_id()}, {"name", m->get_dev_name()}}}, {"sent", sent}, {"result_code", rc}};
    if (rc != 0)
        zk::fail("command_failed", std::string(action) + " was not accepted by the printer connection (code " + std::to_string(rc) + ")");
    r["ok"] = true;
    return r;
}

json op_print_control(const std::string &id, const std::string &action, const json &body)
{
    require_control(body, action.c_str());
    MachineObject *m = need_machine(id);
    if (!m->is_connected())
        zk::fail("not_connected", "the printer is not connected");
    int rc = -1;
    if (action == "pause") {
        if (!m->can_pause())
            zk::fail("bad_state", "the printer is not running a print (state " + m->print_status + ")");
        rc = m->command_task_pause();
    } else if (action == "resume") {
        if (!m->can_resume())
            zk::fail("bad_state", "the printer is not paused (state " + m->print_status + ")");
        rc = m->command_task_resume();
    } else {
        if (!m->can_abort())
            zk::fail("bad_state", "the printer has no print to stop (state " + m->print_status + ")");
        rc = m->command_task_abort();
    }
    json sent = {{"command", action}, {"state_before", m->print_status}};
    log_control(action.c_str(), {{"device", m->get_dev_id()}, {"sent", sent}});
    return result_of(action.c_str(), m, sent, rc);
}

// Creality (K-series) only: dismisses the active printer error, the generic "OK" of Creality Print ({"cleanErr":1}).
json op_clear_error(const std::string &id, const json &body)
{
    require_control(body, "clear_error");
    MachineObject *m = need_machine(id);
    if (agent_id_of(m) != CREALITY_CFS_PRINTER_AGENT_ID)
        zk::fail("not_supported", "clear_error is only available for Creality printers");
    if (!m->is_connected())
        zk::fail("not_connected", "the printer is not connected");
    json j;
    j["print"]["command"]     = "clean_error";
    j["print"]["param"]       = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    const int rc = m->publish_json(j, 1);
    json sent = {{"command", "clean_error"}, {"ws", {{"method", "set"}, {"params", {{"cleanErr", 1}}}}}};
    log_control("clear_error", {{"device", m->get_dev_id()}, {"sent", sent}});
    return result_of("clear_error", m, sent, rc);
}

double need_temp(const json &body, const char *key, double lo, double hi)
{
    const double v = zk::need_num(body, key);
    if (v < lo || v > hi)
        zk::fail("bad_value", std::string("'") + key + "' must be between " + std::to_string(int(lo)) + " and " + std::to_string(int(hi)) + " C");
    return v;
}

json op_temperature(const std::string &id, const json &body)
{
    require_control(body, "temperature");
    MachineObject *m = need_machine(id);
    if (!m->is_connected())
        zk::fail("not_connected", "the printer is not connected");
    const bool has_n = body.contains("nozzle") && !body["nozzle"].is_null();
    const bool has_b = body.contains("bed") && !body["bed"].is_null();
    const bool has_c = body.contains("chamber") && !body["chamber"].is_null();
    if (!has_n && !has_b && !has_c)
        zk::fail("missing_field", "give at least one of nozzle, bed, chamber (target in C, 0 = off)");
    // Hard caps of the API (the printer's firmware limits still apply on top).
    const double bed_cap = m->get_bed_temperature_limit() > 0 ? double(m->get_bed_temperature_limit()) : 120.0;
    json sent = json::object();
    json codes = json::object();
    int rc_all = 0;
    if (has_n) {
        const double t = need_temp(body, "nozzle", 0, 350);
        const int ext = int(zk::opt_num(body, "extruder", 0));
        const int rc = ext > 0 ? m->command_set_nozzle_new(ext, int(t)) : m->command_set_nozzle(int(t));
        sent["nozzle"] = {{"target", int(t)}, {"extruder", ext}};
        codes["nozzle"] = rc;
        rc_all |= rc;
    }
    if (has_b) {
        const double t = need_temp(body, "bed", 0, bed_cap);
        const int rc = m->command_set_bed(int(t));
        sent["bed"] = {{"target", int(t)}};
        codes["bed"] = rc;
        rc_all |= rc;
    }
    if (has_c) {
        const double t = need_temp(body, "chamber", 0, 80);
        const int rc = m->command_set_chamber(int(t));
        sent["chamber"] = {{"target", int(t)}};
        codes["chamber"] = rc;
        rc_all |= rc;
    }
    log_control("temperature", {{"device", m->get_dev_id()}, {"sent", sent}});
    json r = result_of("temperature", m, sent, rc_all);
    r["result_codes"] = codes;
    return r;
}

// Snapmaker U1 head index from the request: "slot" is the head 0..3.
int head_of(const json &body)
{
    const double s = zk::need_num(body, "slot");
    if (s != std::floor(s) || s < 0 || s >= SnapmakerPrinterAgent::HEAD_COUNT)
        zk::fail("bad_value", "'slot' (head) must be 0..3 on a Snapmaker U1");
    return int(s);
}

std::string unit_of(const json &body)
{
    if (!body.contains("unit") || body["unit"].is_null())
        return "0";
    if (body["unit"].is_number_integer())
        return std::to_string(body["unit"].get<long>());
    if (body["unit"].is_string())
        return body["unit"].get<std::string>();
    zk::fail("bad_value", "'unit' must be the unit id from GET /devices/{id}/slots (a number)");
}

int slot_of(const json &body)
{
    const double s = zk::need_num(body, "slot");
    if (s != std::floor(s) || s < 0 || s > 255)
        zk::fail("bad_value", "'slot' must be a whole number from GET /devices/{id}/slots");
    return int(s);
}

std::string need_hex_color(const json &body, const char *key)
{
    const std::string c = zk::need_str(body, key);
    if (c.size() != 7 || c[0] != '#' || !std::all_of(c.begin() + 1, c.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
        zk::fail("bad_value", std::string("'") + key + "' must be '#RRGGBB'");
    return c;
}

json op_filament(const std::string &id, const json &body)
{
    require_control(body, "filament load/unload");
    MachineObject *m = need_machine(id);
    DeviceManager *dm = dev_manager();
    if (!m->is_connected())
        zk::fail("not_connected", "the printer is not connected");
    const std::string action = zk::lower(zk::need_str(body, "action"));
    if (action != "load" && action != "unload")
        zk::fail("bad_value", "'action' must be load or unload");
    if (m->is_in_printing())
        zk::fail("busy", "filament moves are refused while a print runs or is paused");
    const bool load = action == "load";
    json sent = {{"action", action}};
    int rc = 0;
    if (agent_id_of(m) == SNAPMAKER_PRINTER_AGENT_ID) {
        auto ag = SnapmakerPrinterAgent::current();
        if (!ag || !is_selected(dm, m))
            zk::fail("not_selected", "select this Snapmaker U1 in the window first");
        const int head = head_of(body);
        const std::string err = ag->feed_head(head, load ? SnapmakerPrinterAgent::FeedAction::load : SnapmakerPrinterAgent::FeedAction::unload);
        if (!err.empty())
            zk::fail("agent_refused", err);
        sent["head"] = head;
        sent["via"] = "SnapmakerPrinterAgent::feed_head (AUTO_FEEDING; runs on a worker, watch the slots)";
    } else {
        const std::string unit = unit_of(body);
        const int slot = slot_of(body);
        int cur = 210, target = 210;
        if (DevExtderSystem *es = m->GetExtderSystem())
            cur = es->GetNozzleTempCurrent(0);
        target = int(zk::opt_num(body, "nozzle_temp", 210));
        if (target < 150 || target > 320)
            zk::fail("bad_value", "'nozzle_temp' must be between 150 and 320 C");
        rc = m->command_ams_change_filament(load, unit, std::to_string(slot), cur, target);
        sent["unit"] = unit;
        sent["slot"] = slot;
        sent["nozzle_temp"] = target;
        sent["via"] = "MachineObject::command_ams_change_filament";
    }
    log_control("filament", {{"device", m->get_dev_id()}, {"sent", sent}});
    return result_of("filament", m, sent, rc);
}

json op_write_slot(const std::string &id, const json &body)
{
    require_control(body, "slot write");
    MachineObject *m = need_machine(id);
    DeviceManager *dm = dev_manager();
    if (!m->is_connected())
        zk::fail("not_connected", "the printer is not connected");
    if (m->is_in_printing())
        zk::fail("busy", "slot writes are refused while a print runs or is paused");
    const std::string type  = zk::opt_str(body, "type");
    const std::string color = body.contains("color") && !body["color"].is_null() ? need_hex_color(body, "color") : std::string();
    std::vector<std::string> colors;
    if (body.contains("colors") && !body["colors"].is_null()) {
        if (!body["colors"].is_array() || body["colors"].size() < 2 || body["colors"].size() > 4)
            zk::fail("bad_value", "'colors' must be a list of 2..4 '#RRGGBB'");
        for (size_t i = 0; i < body["colors"].size(); ++i) {
            json item = {{"c", body["colors"][i]}};
            colors.push_back(need_hex_color(item, "c"));
        }
    }
    if (type.empty() && color.empty() && colors.empty())
        zk::fail("missing_field", "give type and/or color");
    json sent = json::object();
    const std::string agent = agent_id_of(m);

    if (agent == SNAPMAKER_PRINTER_AGENT_ID) {
        auto ag = SnapmakerPrinterAgent::current();
        if (!ag || !is_selected(dm, m))
            zk::fail("not_selected", "select this Snapmaker U1 in the window first");
        SnapmakerPrinterAgent::HeadWrite w;
        w.head     = head_of(body);
        w.type     = type;
        w.set_type = !type.empty();
        w.vendor   = zk::opt_str(body, "vendor");
        w.name     = zk::opt_str(body, "name");
        w.color    = !color.empty() ? color : (!colors.empty() ? colors.front() : std::string());
        w.colors   = colors;
        w.gradient = zk::opt_bool(body, "gradient", false);
        // force_confirmed is NEVER true here: an official (locked) spool is refused by the agent, as designed.
        const std::string err = ag->write_head(w, false);
        if (!err.empty())
            zk::fail("agent_refused", err);
        sent = {{"head", w.head}, {"type", type}, {"color", w.color}, {"colors", colors}, {"force", false},
                {"via", "SnapmakerPrinterAgent::write_head (SET_PRINT_FILAMENT_CONFIG)"}};
        log_control("slot write", {{"device", m->get_dev_id()}, {"sent", sent}});
        return result_of("slot write", m, sent, 0);
    }

    if (agent == FLASHFORGE_PRINTER_AGENT_ID && CfsUi::ifs_agent() && is_selected(dm, m)) {
        auto ag = CfsUi::ifs_agent();
        if (const std::string reason = ag->write_block_reason(); !reason.empty())
            zk::fail("agent_refused", reason);
        const int port = slot_of(body) + 1; // IFS ports are 1..4; "slot" is 0-based like in GET .../slots
        if (port < 1 || port > 4)
            zk::fail("bad_value", "'slot' must be 0..3 on a Flashforge IFS");
        FlashforgePrinterAgent::SlotWrite w;
        w.port = port;
        for (const auto &s : ag->ifs_slots())
            if (s.port == port) {
                w.type = s.material; w.color = s.color; w.vendor = s.vendor; w.nozzle_temp = s.nozzle_temp; w.bed_temp = s.bed_temp;
            }
        if (!type.empty())
            w.type = type;
        if (!color.empty())
            w.color = color;
        else if (!colors.empty())
            w.color = colors.front();
        w.colors = colors;
        w.gradient = zk::opt_bool(body, "gradient", false);
        w.nozzle_temp = int(zk::opt_num(body, "nozzle_temp", w.nozzle_temp));
        w.bed_temp    = int(zk::opt_num(body, "bed_temp", w.bed_temp));
        if (w.type.empty() || w.color.empty())
            zk::fail("missing_field", "this IFS slot reports no type / colour yet: give both type and color");
        ag->write_slots({w});
        sent = {{"port", port}, {"type", w.type}, {"color", w.color}, {"colors", colors}, {"via", "FlashforgePrinterAgent::write_slots (ZMOD)"}};
        log_control("slot write", {{"device", m->get_dev_id()}, {"sent", sent}});
        return result_of("slot write", m, sent, 0);
    }

    // Bambu AMS, Creality CFS and other agents: the app's own ams_filament_setting command, translated by the agent.
    const std::string unit = unit_of(body);
    const int slot = slot_of(body);
    DevAmsTray *tray = m->GetFilaSystem() ? m->GetFilaSystem()->GetAmsTray(unit, std::to_string(slot)) : nullptr;
    if (tray == nullptr) {
        for (DevAmsTray &t : m->vt_slot)
            if (t.id == unit || t.id == std::to_string(slot))
                tray = &t;
    }
    if (tray == nullptr)
        zk::fail("unknown_slot", "no such unit / slot; see GET /devices/{id}/slots");
    const std::string use_color = !color.empty() ? color : (!colors.empty() ? colors.front() : hex6(tray->color));
    const std::string use_type  = !type.empty() ? type : tray->m_fila_type;
    if (use_type.empty() || use_color.empty())
        zk::fail("missing_field", "the slot has no type / colour yet: give both type and color");
    const std::string filament_id = zk::opt_str(body, "filament_id", tray->setting_id);
    const std::string setting_id  = zk::opt_str(body, "setting_id", tray->filament_setting_id);
    const int tmin = int(zk::opt_num(body, "nozzle_temp_min", tray->nozzle_temp_min.empty() ? 190 : std::atoi(tray->nozzle_temp_min.c_str())));
    const int tmax = int(zk::opt_num(body, "nozzle_temp_max", tray->nozzle_temp_max.empty() ? 240 : std::atoi(tray->nozzle_temp_max.c_str())));
    if (tmin < 100 || tmax > 350 || tmin > tmax)
        zk::fail("bad_value", "nozzle_temp_min / nozzle_temp_max must satisfy 100 <= min <= max <= 350 C");
    std::string rgba = use_color.substr(1);
    std::transform(rgba.begin(), rgba.end(), rgba.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
    rgba += "FF";
    const int rc = m->command_ams_filament_settings(std::atoi(unit.c_str()), slot, filament_id, setting_id, rgba, use_type, tmin, tmax);
    sent = {{"unit", unit}, {"slot", slot}, {"type", use_type}, {"color", use_color}, {"filament_id", filament_id}, {"setting_id", setting_id},
            {"nozzle_temp_min", tmin}, {"nozzle_temp_max", tmax}, {"via", "MachineObject::command_ams_filament_settings"}};
    log_control("slot write", {{"device", m->get_dev_id()}, {"sent", sent}});
    return result_of("slot write", m, sent, rc);
}

json op_print_send(const json &body)
{
    require_control(body, "print send");
    Plater *p = wxGetApp().plater();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (p->model().objects.empty())
        zk::fail("empty_plate", "the project has no objects");
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for the running slice to finish");
    PartPlateList &plates = p->get_partplate_list();
    const int plate_idx = int(zk::opt_num(body, "plate", plates.get_curr_plate_index()));
    PartPlate *plate = plates.get_plate(plate_idx);
    if (plate == nullptr)
        zk::fail("unknown_plate", "no such plate");
    if (!plate->is_slice_result_ready_for_print() || plate->fff_print() == nullptr || !plate->is_valid_gcode_file())
        zk::fail("not_sliced", "slice this plate first (POST /slice) and fix its errors");

    DynamicPrintConfig *cfg = &bundle->printers.get_edited_preset().config;
    PrintHostJob job(cfg);
    if (job.empty())
        zk::fail("no_print_host", "the selected printer preset has no print host (OctoPrint, Moonraker, PrusaLink ...); see GET /print_hosts");
    if (cfg->has("use_3mf") && cfg->opt_bool("use_3mf"))
        zk::fail("use_3mf_not_supported", "this printer expects a .gcode.3mf bundle; send it from the window");

    const bool start = zk::opt_bool(body, "start", false);
    if (start && !job.printhost->get_post_upload_actions().has(PrintHostPostUploadAction::StartPrint))
        zk::fail("start_not_supported", "this print host cannot start a print after the upload; send with start:false");

    std::string name = zk::opt_str(body, "name");
    if (name.empty()) {
        boost::filesystem::path proj(boost::nowide::widen(u8(p->get_project_filename(".3mf"))));
        name = proj.stem().string();
        if (name.empty())
            name = "remote_api_print";
    }
    if (name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos)
        zk::fail("bad_value", "'name' must be a plain file name");
    name = fold_utf8_to_ascii(name, true);
    if (!zk::has_ext(name, ".gcode") && !zk::has_ext(name, ".bgcode"))
        name += ".gcode";

    // The plate's sliced G-code goes to a private temp file (the queue deletes it after the upload), then the
    // post-processing scripts run on it exactly as for an upload from the window.
    boost::system::error_code ec;
    const std::string src = plate->get_tmp_gcode_path();
    if (!boost::filesystem::exists(src, ec))
        zk::fail("not_sliced", "the plate's G-code file is gone; slice again");
    boost::filesystem::path tmp = boost::filesystem::temp_directory_path(ec) / boost::filesystem::unique_path(".OrcaZoukei.api.upload.%%%%-%%%%-%%%%-%%%%");
    std::string copy_error;
    if (Slic3r::copy_file(src, tmp.string(), copy_error) != SUCCESS)
        zk::fail("copy_failed", "could not copy the G-code to a temp file: " + copy_error);
    std::string tmp_str = tmp.string();
    std::string out_name = name;
    try {
        if (!plate->fff_print()->is_BBL_printer())
            if (run_post_process_scripts(tmp_str, false, job.printhost->get_name(), out_name, plate->fff_print()->full_print_config()))
                name = out_name;
    } catch (const std::exception &e) {
        boost::filesystem::remove(tmp, ec);
        zk::fail("post_process_failed", e.what());
    }

    job.upload_data.use_3mf     = false;
    job.upload_data.source_path = tmp_str;
    job.upload_data.upload_path = name;
    job.upload_data.post_action = start ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;
    job.upload_data.group       = zk::opt_str(body, "group");
    job.upload_data.storage     = zk::opt_str(body, "storage");
    job.switch_to_device_tab    = false;

    const std::string host = job.printhost->get_host();
    const std::string host_name = job.printhost->get_name();
    const auto bytes = boost::filesystem::file_size(tmp_str, ec);
    json sent = {{"host", host}, {"host_type", host_name}, {"file", name}, {"plate", plate_idx}, {"start_print", start},
                 {"bytes", ec ? 0 : (uint64_t) bytes}, {"via", "PrintHostJobQueue (same as Send to printer)"}};
    log_control("print send", {{"preset", bundle->printers.get_selected_preset_name()}, {"sent", sent}});
    wxGetApp().printhost_job_queue().enqueue(std::move(job));
    zk::notify("Upload to " + host + " queued by the Remote API: " + name);
    json r = {{"action", "print send"}, {"queued", true}, {"sent", sent}, {"ok", true},
              {"next", "the upload runs in the background; follow it in Window > Print Host Upload Queue"}};
    return r;
}

} // namespace

bool Controller::dispatch_devices(const Request &req, Response &out)
{
    static const std::string base = "/api/v1/";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::vector<std::string> parts = zk::split_path(path.substr(base.size()));
    if (parts.empty())
        return false;
    const std::string &m = req.method;

    // ---- reads --------------------------------------------------------------------------------
    if (m == "GET" && parts.size() == 1 && parts[0] == "devices") {
        out = zk::reply(run_on_ui([]() -> json { return zk::guard([&]() -> json { return op_list_devices(); }); }, 30));
        return true;
    }
    if (m == "GET" && parts.size() == 1 && parts[0] == "print_hosts") {
        out = zk::reply(run_on_ui([]() -> json { return zk::guard([&]() -> json { return op_print_hosts(); }); }, 30));
        return true;
    }
    if (m == "GET" && parts.size() == 1 && parts[0] == "printer_control") {
        out = zk::reply(run_on_ui([]() -> json {
            return json{{"enabled", control_enabled()},
                        {"switch", "Preferences > Remote API > Allow printer control from the Remote API"},
                        {"also_needed", "\"confirm\": true in every control request"}};
        }, 30));
        return true;
    }
    if (parts[0] == "devices" && parts.size() >= 2 && m == "GET" && (parts.size() == 2 || (parts.size() == 3 && parts[2] == "slots"))) {
        const std::string id = parts[1];
        const bool slots_only = parts.size() == 3;
        out = zk::reply(run_on_ui([id, slots_only]() -> json {
            return zk::guard([&]() -> json {
                MachineObject *mo = need_machine(id);
                DeviceManager *dm = dev_manager();
                if (!slots_only)
                    return device_status(dm, mo);
                json j = slots_json(mo);
                j["device"] = {{"id", mo->get_dev_id()}, {"name", mo->get_dev_name()}};
                json ad = agent_details(dm, mo);
                if (!ad.empty())
                    j["agent_details"] = ad;
                return j;
            });
        }, 30));
        return true;
    }

    // ---- control (gated inside the ops) -------------------------------------------------------
    const bool dev_action = parts[0] == "devices" && parts.size() == 3 &&
                            ((m == "POST" && (parts[2] == "pause" || parts[2] == "resume" || parts[2] == "stop" || parts[2] == "temperature" || parts[2] == "filament" || parts[2] == "clear_error")) ||
                             (m == "PUT" && parts[2] == "slots"));
    const bool send = m == "POST" && parts.size() == 2 && parts[0] == "print" && parts[1] == "send";
    if (!dev_action && !send)
        return false;

    json body = json::object();
    if (!zk::parse_body(req, body)) {
        out = {400, {{"error", "invalid_json"}}};
        return true;
    }
    if (send) {
        out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_print_send(body); }); }, 120));
        return true;
    }
    const std::string id  = parts[1];
    const std::string act = parts[2];
    out = zk::reply(run_on_ui([id, act, body]() -> json {
        return zk::guard([&]() -> json {
            if (act == "pause" || act == "resume" || act == "stop")
                return op_print_control(id, act, body);
            if (act == "temperature")
                return op_temperature(id, body);
            if (act == "clear_error")
                return op_clear_error(id, body);
            if (act == "filament")
                return op_filament(id, body);
            return op_write_slot(id, body);
        });
    }, 60));
    return true;
}

}}} // namespace Slic3r::GUI::RemoteAPI
