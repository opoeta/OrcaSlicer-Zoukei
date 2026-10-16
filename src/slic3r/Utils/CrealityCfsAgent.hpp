// Native "creality_cfs" printer agent: Creality K-series printers over the LAN protocol of
// Creality Print (JSON over ws://<host>:9999). Publishes the CFS slots as AMS trays
// (subscription mode, so the sidebar sync and the filament badges follow the printer) and turns
// Orca's ams_filament_setting into a verified slot write.
//
// Ported to C++ from the "Creality CFS Sync" OrcaSlicer plugin by Heyde Moura (creality_cfs.py);
// it keeps the plugin's agent id so presets saved with printer_agent = creality_cfs keep working.
#ifndef slic3r_Utils_CrealityCfsAgent_hpp_
#define slic3r_Utils_CrealityCfsAgent_hpp_

#include "IPrinterAgent.hpp"
#include "CrealityCfs.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

namespace CrealityCfs { class Client; }

#define CREALITY_CFS_PRINTER_AGENT_ID "creality_cfs"

class CrealityCfsAgent final : public IPrinterAgent, public std::enable_shared_from_this<CrealityCfsAgent>
{
public:
    explicit CrealityCfsAgent(std::string log_dir);
    ~CrealityCfsAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Filament presets as plain data, shared with the auto-sync. refresh_presets() runs on the
    // UI thread; presets() from anywhere.
    static void                                 refresh_presets();
    static std::vector<CrealityCfs::PresetInfo> presets();
    // The vendor model_id of the selected printer (UI thread).
    static std::string current_model_id();
    // Shows a plater notification from any thread.
    static void notify(const std::string& text, bool error = false);

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    bool start_discovery(bool start, bool sending) override;
    int  bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int  set_server_callback(OnServerErrFn fn) override;

    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    // Uploads and starts go through Orca's Creality print host (HTTP upload + ws://<host>:9999 start).
    // PrintParams carries: extra_options = {"files":[{"path","name"}],"self_test":0|1}; ams_mapping_info = the dialog's
    // per-filament mapping (field "ams" = tray id, 255/254 = external spool, -1 = unmapped).
    int start_print(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn) override { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    int start_local_print_with_record(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn) override { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn) override { return upload_job(params, update_fn, cancel_fn, false); }
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override { return upload_job(params, update_fn, cancel_fn, true); }
    int start_sdcard_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) override { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }
    bool             fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

private:
    void on_printer_message(const nlohmann::json& msg);
    void announce(const CrealityCfs::PrinterSnapshot& snap);
    void publish(const nlohmann::json& message);
    void publish_status(bool force);
    int  handle_print_command(const nlohmann::json& cmd);
    int  upload_job(const PrintParams& params, const OnUpdateStatusFn& update_fn, const WasCancelledFn& cancel_fn, bool start);
    void run_on_main(std::function<void()> fn);

    std::recursive_mutex                 m_mutex;
    std::shared_ptr<CrealityCfs::Client> m_client;
    int                                  m_listener{0};
    std::string                          m_dev_id;
    std::string                          m_host;
    std::string                          m_model_id;
    std::string                          m_selected_machine;
    bool                                 m_connected_reported{false};
    bool                                 m_announced{false};
    std::string                          m_announced_name;
    std::string                          m_store_name;       // name of the device in the Creality device list
    std::string                          m_nozzle_diameter;  // from the printer preset (the printer does not report it)
    std::atomic<int>                     m_sequence{1};
    std::atomic<long long>               m_last_push_ms{0};

    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    OnMsgArrivedFn                      m_on_ssdp;
    OnPrinterConnectedFn                m_on_printer_connected;
    GetSubscribeFailureFn               m_on_subscribe_failure;
    OnMessageFn                         m_on_message;
    OnMessageFn                         m_on_user_message;
    OnLocalConnectedFn                  m_on_local_connect;
    OnMessageFn                         m_on_local_message;
    QueueOnMainFn                       m_queue_on_main;
    OnServerErrFn                       m_on_server_err;
};

} // namespace Slic3r

#endif // slic3r_Utils_CrealityCfsAgent_hpp_
