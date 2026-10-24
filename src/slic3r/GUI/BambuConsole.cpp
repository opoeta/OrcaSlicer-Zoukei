#include "BambuConsole.hpp"

#include "DeviceConsole.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "I18N.hpp"

#include "slic3r/Utils/BambuActivity.hpp"
#include "slic3r/Utils/DeviceActivityLog.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace BambuConsole {

namespace BA = Slic3r::DeviceActivity::Bambu;
namespace DA = Slic3r::DeviceActivity;

namespace {

bool other_agent(const MachineObject* obj)
{
    const std::string& id = obj->printer_agent_id;
    return id == "creality_cfs" || id == "flashforge" || id == "snapmaker" || id == "moonraker";
}

BA::Lookup lookup_for(MachineObject* obj)
{
    BA::Lookup lk;
    lk.tr    = [](const char* s) { return I18N::translate_utf8(s); };
    lk.stage = [](int stg) { return std::string(Slic3r::get_stage_string(stg).ToUTF8().data()); };
    lk.hms   = [obj](const std::string& long_code) -> std::string {
        HMSQuery* q = wxGetApp().get_hms_query();
        return q ? std::string(q->query_hms_msg(obj, long_code).ToUTF8().data()) : std::string();
    };
    lk.print_error = [obj](int err) -> std::string {
        HMSQuery* q = wxGetApp().get_hms_query();
        return q ? std::string(q->query_print_error_msg(obj, err).ToUTF8().data()) : std::string();
    };
    return lk;
}

} // namespace

std::string key_of(const MachineObject* obj)
{
    if (obj == nullptr || other_agent(obj))
        return {};
    const std::string ip = DA::key_of(obj->get_dev_ip());
    return ip.empty() ? DA::key_of(obj->get_dev_id()) : ip;
}

std::string selected_key()
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (dm == nullptr)
        return {};
    return key_of(dm->get_selected_machine());
}

void on_report(MachineObject* obj, const nlohmann::json& merged_report)
{
    try {
        const std::string key = key_of(obj);
        if (!key.empty())
            BA::on_report(key, merged_report, lookup_for(obj));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "BambuConsole::on_report: " << e.what();
    } catch (...) {
    }
}

void on_command(MachineObject* obj, const nlohmann::json& command, int result)
{
    try {
        const std::string key = key_of(obj);
        if (!key.empty())
            BA::on_command(key, command, result, lookup_for(obj));
    } catch (...) {
    }
}

void on_connect_state(MachineObject* obj, int state, const std::string& msg)
{
    try {
        const std::string key = key_of(obj);
        if (!key.empty())
            BA::on_connect_state(key, state, msg, lookup_for(obj));
    } catch (...) {
    }
}

void on_online(MachineObject* obj, bool online)
{
    try {
        const std::string key = key_of(obj);
        if (!key.empty())
            BA::on_online(key, online, lookup_for(obj));
    } catch (...) {
    }
}

void on_connecting(MachineObject* obj)
{
    try {
        const std::string key = key_of(obj);
        if (!key.empty())
            BA::on_connecting(key, obj->is_lan_mode_printer(), lookup_for(obj));
    } catch (...) {
    }
}

void on_print_job(MachineObject* obj, const std::string& task_name, const std::string& print_type, int plate_index, const std::string& ams_mapping,
                  int result)
{
    try {
        const std::string key = key_of(obj);
        if (key.empty())
            return;
        std::string text = _u8L("Print job sent") + ": " + task_name + ", " + _u8L("plate") + " " + std::to_string(plate_index + 1);
        if (!print_type.empty())
            text += " (" + print_type + ")";
        if (result != 0)
            text += " - " + _u8L("failed") + " (" + std::to_string(result) + ")";
        nlohmann::json raw = {{"task_name", task_name}, {"print_type", print_type}, {"plate_index", plate_index}, {"result", result}};
        if (!ams_mapping.empty())
            raw["ams_mapping"] = nlohmann::json::parse(ams_mapping, nullptr, false);
        DA::add(key, result != 0 ? DA::Source::Error : DA::Source::WsOut, text, raw);
    } catch (...) {
    }
}

void show(wxWindow* parent, MachineObject* obj)
{
    const std::string key = key_of(obj);
    if (key.empty())
        return;
    const std::string name = obj->get_dev_name();
    DeviceConsole::show_native(parent, key, name.empty() ? key : name + " (" + key + ")");
}

}}} // namespace Slic3r::GUI::BambuConsole
