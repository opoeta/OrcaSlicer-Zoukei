// Device tab for the Snapmaker U1 (printer_agent "snapmaker", Klipper + Moonraker): hosts the same page as the Creality /
// Flashforge panels (resources/web/zoukeiDevice) with the Moonraker backend and the U1 screen (four heads, official lock,
// feed state, temperatures, print status). The page cannot reach Moonraker itself (file:// origin, no CORS headers), so the
// host runs its few JSON requests and answers them. Writes that change filament data go through SnapmakerPrinterAgent
// (refused while printing; FORCE=1 only after the confirm dialog of this panel). The device list is the printer presets that
// are a U1 and have a host; only those hosts are ever contacted.
#ifndef slic3r_GUI_SnapmakerU1DevicePanel_hpp_
#define slic3r_GUI_SnapmakerU1DevicePanel_hpp_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <wx/panel.h>
#include <wx/webview.h>

#include <nlohmann/json.hpp>

#include "Lazy.hpp"

namespace Slic3r {

class PresetBundle;

namespace GUI {

class SnapmakerU1DevicePanel : public wxPanel, public LazyInstance<SnapmakerU1DevicePanel>
{
public:
    explicit SnapmakerU1DevicePanel(wxWindow* parent);
    ~SnapmakerU1DevicePanel() override;

    // The selected printer preset is a Snapmaker U1.
    static bool is_u1_printer(const PresetBundle& bundle);

    void on_sys_color_changed();
    bool Show(bool show = true) override;

private:
    using json    = nlohmann::json;
    using Handler = std::function<void(const json&)>;
    struct Bridge;

    void create_browser();
    void load_page();
    void register_routes();
    void on_script_message(wxWebViewEvent& evt);
    void post_command(const std::string& command, json data);
    void post_command_async(const std::string& command, json data);
    void run_script(const std::string& script);
    void send_device_list();
    void route_moonraker_http(const json& j);
    void route_get_filament_presets(const json& j);
    void route_write_head(const json& j);
    void route_feed(const json& j);
    void route_sync_all(const json& j);

    wxWebView*                               m_browser{nullptr};
    std::shared_ptr<Bridge>                  m_bridge;
    std::unordered_map<std::string, Handler> m_handlers;
    bool                                     m_page_requested{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_SnapmakerU1DevicePanel_hpp_
