// Orca Zoukei: glue between the Bambu / Orca-agent MachineObject (MQTT reports and commands) and the printer Console
// (Utils/DeviceActivityLog). The log of a Bambu printer is keyed by its IP address, or by its device id when the IP is
// unknown (cloud mode). The diff / summary logic lives in Utils/BambuActivity.
#ifndef slic3r_GUI_BambuConsole_hpp_
#define slic3r_GUI_BambuConsole_hpp_

#include <string>

#include <nlohmann/json.hpp>

class wxWindow;

namespace Slic3r {
class MachineObject;
namespace GUI { namespace BambuConsole {

// The Console key of a printer, "" when its console is fed by another agent (Creality, Flashforge, Snapmaker, Moonraker).
std::string key_of(const MachineObject* obj);
// The Console key of the printer selected in the monitor, "" when none or not a Bambu-style printer.
std::string selected_key();

// Hooks called by MachineObject / GUI_App. All are cheap and never throw.
void on_report(MachineObject* obj, const nlohmann::json& merged_report);
void on_command(MachineObject* obj, const nlohmann::json& command, int result);
void on_connect_state(MachineObject* obj, int state, const std::string& msg);
void on_online(MachineObject* obj, bool online);
void on_connecting(MachineObject* obj);
// A print job sent from the Send / Print dialog (the file transfer + start does not go through publish_json).
void on_print_job(MachineObject* obj, const std::string& task_name, const std::string& print_type, int plate_index, const std::string& ams_mapping,
                  int result);

// Opens the native Console window of a printer (the monitor's "Console" button).
void show(wxWindow* parent, MachineObject* obj);

}}} // namespace Slic3r::GUI::BambuConsole

#endif
