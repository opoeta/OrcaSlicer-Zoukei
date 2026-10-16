// Device tab for Creality printers: hosts Orca Zoukei's own device panel (resources/web/zoukeiDevice,
// plain HTML/CSS/JS) and answers the few host commands it sends: language and theme, the device
// list (add by IP, LAN scan, rename, remove), camera signalling and the Orca filament presets.
// The panel talks to the printers over WebSocket through WsProxy; only LAN addresses from the
// device list (or just probed by the user) are ever contacted. No Creality cloud or account.
// The device store, LAN probe, WebSocket proxy and image relay in CrealityDevice/ are ported from
// Creality Print (CrealityOfficial/CrealityPrint@59ae8cbb0437dcc7236e27433ecd4aeb52be0244, AGPL-3.0).
#ifndef slic3r_GUI_CrealityDevicePanel_hpp_
#define slic3r_GUI_CrealityDevicePanel_hpp_

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

namespace CrealityDevice {
class WsProxy;
class ImageServer;
}

class CrealityDevicePanel : public wxPanel, public LazyInstance<CrealityDevicePanel>
{
public:
    explicit CrealityDevicePanel(wxWindow* parent);
    ~CrealityDevicePanel() override;

    // The selected printer is a Creality one: Creality vendor, Creality Print host type, or a
    // "Creality ..." printer model.
    static bool is_creality_printer(const PresetBundle& bundle);

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
    void on_new_window(wxWebViewEvent& evt);

    // Sends {command, data} to the page's window.handleStudioCmd. UI thread.
    void post_command(const std::string& command, json data);
    // Same, from any thread.
    void post_command_async(const std::string& command, json data);
    void run_script(const std::string& script);

    // Device list (zoukei_creality_devices.json), seeded on first use.
    static void ensure_device_store_loaded();
    static bool is_known_lan_host(const std::string& host);

    json device_list() const;
    void send_device_list();

    void route_device_add(const json& j);
    void route_lan_scan(const json& j);
    void route_get_webrtc_local_param(const json& j);
    void route_get_filament_presets(const json& j);

    wxWebView*                                   m_browser{nullptr};
    std::unique_ptr<CrealityDevice::WsProxy>     m_ws_proxy;
    std::unique_ptr<CrealityDevice::ImageServer> m_image_server;
    std::shared_ptr<Bridge>                      m_bridge;
    std::unordered_map<std::string, Handler>     m_handlers;
    bool                                         m_page_requested{false};
    bool                                         m_scan_running{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_CrealityDevicePanel_hpp_
