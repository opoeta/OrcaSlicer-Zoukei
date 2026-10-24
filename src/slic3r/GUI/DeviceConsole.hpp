// Orca Zoukei: the "Console" of the printer panels. The web panels (resources/web/zoukeiDevice) ask the host for the
// activity log through the commands registered here; the native window (wx) shows the same log for panels that are not
// web pages. The data itself lives in Utils/DeviceActivityLog (also served by GET /api/v1/devices/<id>/activity).
#ifndef slic3r_GUI_DeviceConsole_hpp_
#define slic3r_GUI_DeviceConsole_hpp_

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

class wxWindow;

namespace Slic3r { namespace GUI { namespace DeviceConsole {

using Handler = std::function<void(const nlohmann::json&)>;
using Post    = std::function<void(const std::string& command, nlohmann::json data)>;

// Adds the console commands of the web page to a panel's route table:
//   device_activity {requestId, host, since, limit, sources, q, raw}  -> answers "device_activity" {requestId, entries, next, ...}
//   device_activity_clear {host} | device_activity_show_all {on} | device_activity_copy {host, sources, q}
//   device_activity_export {requestId, host, sources, q}              -> save dialog, answers "device_activity_export" {ok, path}
//   device_klipper_poll {requestId, host}                              -> fetches new klippy.log lines, answers {added, error}
// `post` runs on the UI thread, `post_async` may be called from any thread.
void register_routes(std::unordered_map<std::string, Handler>& routes, Post post, Post post_async);

// The native console window for a printer host (modeless; one per host, raised when already open).
void show_native(wxWindow* parent, const std::string& host, const std::string& title = {});

}}} // namespace Slic3r::GUI::DeviceConsole

#endif
