// Device tab for Flashforge printers (printer_agent "flashforge", ZMOD firmware = Klipper + Moonraker): hosts the
// same page as the Creality panel (resources/web/zoukeiDevice) with the Moonraker backend. The page cannot reach
// Moonraker itself (file:// origin, no CORS headers), so the host runs its few JSON requests and answers them.
// The device list is the printer presets that use the flashforge agent; only those hosts are ever contacted.
#ifndef slic3r_GUI_FlashforgeDevicePanel_hpp_
#define slic3r_GUI_FlashforgeDevicePanel_hpp_

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

class FlashforgeDevicePanel : public wxPanel, public LazyInstance<FlashforgeDevicePanel>
{
public:
    explicit FlashforgeDevicePanel(wxWindow* parent);
    ~FlashforgeDevicePanel() override;

    // The selected printer preset uses the flashforge printer agent (Flashforge vendor or model).
    static bool is_flashforge_printer(const PresetBundle& bundle);

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

    wxWebView*                               m_browser{nullptr};
    std::shared_ptr<Bridge>                  m_bridge;
    std::unordered_map<std::string, Handler> m_handlers;
    bool                                     m_page_requested{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_FlashforgeDevicePanel_hpp_
