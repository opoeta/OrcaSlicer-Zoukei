// Native "flashforge" printer agent for Flashforge AD5X / 5M-family printers on the LAN.
//
// Two firmware cases are handled automatically when the printer is selected:
//   * ZMOD (Klipper + Moonraker on :7125): everything the Moonraker agent does (status, uploads, prints) plus the
//     IFS slots from ZMOD's own zmod_color object and the tool -> IFS port assignment on print start.
//   * stock firmware (HTTP JSON API on :8898, authenticated with serial number + check code): status, temperatures,
//     IFS slots, upload + print with the IFS material mapping, pause / resume / stop.
// Filament sync is subscription based in both cases: the printer's IFS follows the printer, the sidebar follows the IFS.
//
// Stock API credentials go in the printer preset's "API key" field as "SERIAL:CHECKCODE" (both are shown on the
// printer's screen); the stock endpoints were learnt from the ISC licensed ff-5mp-api project, none of its code is used.
#ifndef slic3r_Utils_FlashforgePrinterAgent_hpp_
#define slic3r_Utils_FlashforgePrinterAgent_hpp_

#include "MoonrakerPrinterAgent.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

#define FLASHFORGE_PRINTER_AGENT_ID "flashforge"

class FlashforgePrinterAgent final : public MoonrakerPrinterAgent, public std::enable_shared_from_this<FlashforgePrinterAgent>
{
public:
    enum class Mode { unresolved = 0, zmod, stock };
    // The touchscreen UI of a ZMOD printer: Flashforge's original screen, HelixScreen or GuppyScreen.
    enum class Ui { unknown = 0, original, helix, guppy };

    // A change of one IFS slot to write to the printer (ZMOD): the project's filament.
    struct SlotWrite
    {
        int         port = 0;      // 1..4
        std::string type;          // Orca filament_type ("PLA", "PETG", ...)
        std::string name;          // filament preset name ("Generic PLA Silk"), helps to tell SILK from PLA
        std::string color;         // "#RRGGBB"
        std::string vendor;
        int         nozzle_temp = 0;
        int         bed_temp    = 0;
    };

    // One IFS slot as the printer reports it.
    struct IfsSlot
    {
        int         port = 0;  // 1..4, as ZMOD / the stock firmware number them
        bool        has_filament = false;
        std::string material;  // "PLA", "SILK", "PETG", ...
        std::string color;     // "#RRGGBB"
        std::string name;      // display name from the spool database, may be empty
        std::string vendor;
        int         nozzle_temp = 0;
        int         bed_temp = 0;
    };

    explicit FlashforgePrinterAgent(std::string log_dir);
    ~FlashforgePrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    Mode mode() const { return Mode(m_mode.load()); }
    Ui   ui() const { return Ui(m_ui.load()); }
    // "ZMOD + HelixScreen", "ZMOD + tela original", ... ("" when not ZMOD / not known yet).
    std::string ui_name() const;

    // Project -> printer (ZMOD): CHANGE_ZCOLOR for the firmware (what both screens show) and, with HelixScreen,
    // the lane_data metadata. write_block_reason() is "" when writing is possible right now; write_slots() runs on
    // a worker thread and reports through a plater notification.
    std::string write_block_reason() const;
    void        write_slots(std::vector<SlotWrite> writes);

    // The vendor model_id of the selected printer ("Flashforge-AD5X"), UI thread.
    static std::string current_model_id();
    // Last IFS slots seen (any thread); empty before the first reply.
    std::vector<IfsSlot> ifs_slots() const;
    // MJPEG stream of the printer camera ("" when unknown), any thread.
    std::string camera_url() const;

    int  connect_printer(const PrinterConnectionParams& params) override;
    int  disconnect_printer() override;
    int  send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int  send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    bool start_discovery(bool start, bool sending) override;
    int  bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;

    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }
    bool             fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

protected:
    std::string make_device_name(const std::string& hostname) const override;

private:
    struct Session
    {
        uint64_t    generation = 0;
        std::string host;
        std::string port;
        bool        use_ssl = false;
        std::string dev_id;
        std::string serial;
        std::string check_code;
        std::string base_url; // Moonraker base url (zmod) or http://host:8898 (stock)
        std::string api_key;
    };

    void           resolve_mode(const PrinterConnectionParams& params, uint64_t generation);
    void           start_zmod(const PrinterConnectionParams& params, uint64_t generation);
    void           start_stock(uint64_t generation);
    void           zmod_loop(uint64_t generation, std::string base_url, std::string api_key);
    void           stock_loop(uint64_t generation);
    bool           is_current(uint64_t generation) const { return generation == m_generation.load(); }

    // ZMOD
    bool           fetch_zmod_slots(const std::string& base_url, const std::string& api_key, std::vector<IfsSlot>& slots);
    bool           write_lane_data(const std::string& base_url, const std::string& api_key, const SlotWrite& w, const std::string& zmod_type, const IfsSlot* before);
    void           write_slots_worker(std::vector<SlotWrite> writes);
    int            handle_ams_setting(const nlohmann::json& cmd);
    void           apply_zmod_slots(uint64_t generation, std::vector<IfsSlot> slots);
    void           send_zmod_mapping(const PrintParams& params);
    int            zmod_job(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, bool start);

    // stock
    bool           stock_post(const std::string& path, const nlohmann::json& body, nlohmann::json& reply, std::string& error, int timeout = 6) const;
    bool           stock_control(const std::string& cmd, const nlohmann::json& args);
    nlohmann::json build_stock_status(const nlohmann::json& detail);
    void           publish_stock(uint64_t generation, nlohmann::json detail);
    void           announce_stock(const nlohmann::json& detail);
    int            handle_stock_command(const nlohmann::json& cmd);
    int            stock_upload(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, bool start);

    // shared
    void           run_on_main(std::function<void()> fn);
    void           publish(const nlohmann::json& message);
    nlohmann::json tray_json(int slot_zero_based, const IfsSlot* slot) const; // main thread
    std::string    preset_id_for(const IfsSlot& slot) const;                  // main thread

    std::atomic<int>      m_mode{0};
    std::atomic<int>      m_ui{0};
    std::atomic<bool>     m_busy{false};          // printing / paused (ZMOD)
    std::atomic<bool>     m_writing{false};
    bool                  m_display_popup = false; // zmod_color.display: CHANGE_ZCOLOR raises a popup on the screen
    std::vector<std::string> m_valid_types;       // zmod_color.valid_types, without "?"
    std::string           m_zmod_url, m_zmod_key;
    std::atomic<uint64_t> m_generation{0};
    mutable std::mutex    m_mutex;
    Session               m_session;
    std::string           m_model_id;
    std::string           m_model_name;
    std::string           m_nozzle_diameter;
    std::vector<IfsSlot>  m_slots;
    std::string           m_slot_signature;
    std::string           m_camera_url;
    nlohmann::json        m_detail;       // last stock /detail
    std::atomic<int>      m_sequence{1};
    bool                  m_announced = false;
    bool                  m_connected_reported = false;
    std::string           m_selected_machine;

    OnMsgArrivedFn        m_on_ssdp;
    OnPrinterConnectedFn  m_on_printer_connected;
    OnMessageFn           m_on_message;
    OnLocalConnectedFn    m_on_local_connect;
    OnMessageFn           m_on_local_message;
    QueueOnMainFn         m_queue_main;
};

} // namespace Slic3r

#endif // slic3r_Utils_FlashforgePrinterAgent_hpp_
