// Native agent for the Snapmaker U1 (Klipper + Moonraker on :7125, four tool heads). Orca Zoukei.
//
// Reads (POST /printer/objects/query): print_task_config, filament_detect, filament_feed left|right, print_stats,
// extruder .. extruder3, heater_bed. Writes go through POST /printer/gcode/script:
//   SET_PRINT_FILAMENT_CONFIG  (type / colour of a head; official NFC spools are locked unless FORCE=1)
//   SET_PRINT_EXTRUDER_MAP / SET_PRINT_USED_EXTRUDERS  (project filament -> physical head, before a print starts)
//   AUTO_FEEDING EXTRUDER=n LOAD=1|UNLOAD=1  (explicit user request only)
// All writes are refused while the printer prints or is paused, and FORCE=1 is only ever sent after the caller
// confirmed it (the caller owns the confirm dialog; this class refuses an official head without that flag).
//
// The firmware facts come from the open u1-klipper sources (print_task_config.py, filament_feed.py, lava/fluidd.cfg);
// the UI ideas from Snapmaker's AGPL OrcaSlicer fork (SSWCP.cpp, GUI/filamentsync). No code of either is used.
#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <atomic>
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

#define SNAPMAKER_PRINTER_AGENT_ID "snapmaker"

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent, public std::enable_shared_from_this<SnapmakerPrinterAgent>
{
public:
    static constexpr int HEAD_COUNT = 4;

    // One tool head as print_task_config / filament_feed / extruderN report it.
    struct U1Head
    {
        int         index        = 0;
        bool        present      = false; // filament_exist
        bool        official     = false; // NFC spool of Snapmaker: locked, writing needs FORCE=1
        bool        editable     = false; // filament_edit
        bool        soft         = false;
        std::string vendor;               // "NONE" is stored as ""
        std::string type;                 // main type: "PLA", "PETG", ...
        std::string sub_type;             // "SnapSpeed", "Silk", ... ("NONE" -> "")
        std::string color;                // "#RRGGBB"
        int         alpha        = 255;
        std::vector<std::string> colors;  // all colours "#RRGGBB" when >= 2 (dual / gradient), colors[0] == color
        int         color_mode   = 0;     // filament_color_multi.mode: 0 side by side, 1 gradient
        int         bed_temp     = 0;     // from the NFC tag (filament_detect), 0 when unknown
        int         nozzle_temp  = 0;
        double      nozzle_now   = 0.;    // extruderN.temperature / target
        double      nozzle_target = 0.;
        bool        feed_module  = false; // filament_feed: a feeder module serves this head
        bool        feed_detected = false;
        bool        auto_feed    = true;
        std::string feed_state;           // channel_state ("load_finish", "unload_doing", ...)
        std::string feed_action;          // channel_action_state ("none" when idle)
        std::string feed_error;           // channel_error ("ok" when none)
        int         mapped_from  = -1;    // first logical extruder (T) that extruder_map_table sends to this head, -1 none
        // "PLA SnapSpeed", "PETG", "PLA-CF" -> the type as Orca names it (sub types that only qualify a PLA are appended)
        std::string display_type() const;
    };

    struct U1State
    {
        bool        valid = false;        // at least one complete reply
        std::string print_state;          // print_stats.state: standby, printing, paused, complete, cancelled, error
        std::string klippy_state;         // webhooks.state
        double      bed_now = 0., bed_target = 0.;
        std::string active_extruder;      // toolhead.extruder ("extruder", "extruder1", ...)
        std::vector<int> map_table;       // extruder_map_table (first HEAD_COUNT entries are the ones Orca uses)
        std::vector<bool> used;           // extruders_used
        std::vector<U1Head> heads;        // HEAD_COUNT entries
        bool busy_printing() const { return print_state == "printing" || print_state == "paused"; }
        bool feed_busy() const;
    };

    // A change of one head to write: the project's filament.
    struct HeadWrite
    {
        int         head = 0;             // 0..3
        std::string vendor;               // "" -> "Generic"
        std::string type;                 // Orca filament_type: "PLA", "PETG-CF", ...
        std::string name;                 // filament preset name: used to find the sub type (Silk, Matte, ...)
        std::string color;                // "#RRGGBB"; empty keeps the head's colour
        std::vector<std::string> colors;  // >= 2 entries "#RRGGBB" when dual / tri / quad / gradient, colors[0] == color
        bool        gradient = false;
        bool        set_type = true;      // false: colour only (the type stays as it is)
    };

    enum class FeedAction { load, unload };

    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // ---- state (any thread) ----
    U1State     u1_state() const;
    // "" when a write can be sent right now, else the reason (printing, not ready, no data yet).
    std::string write_block_reason() const;
    // Forces a re-read soon (after a write).
    void        refresh_soon();

    // ---- writes (UI thread; the work runs on a worker, the result is a plater notification) ----
    // Returns "" when queued, else why not. `force_confirmed` must only be true after the user confirmed in a dialog; an
    // official head is refused without it and FORCE=1 is sent only for an official head.
    std::string write_head(const HeadWrite& w, bool force_confirmed);
    // Auto-sync: several heads in one request. Official and empty heads are skipped (never forced). Returns "" or the block
    // reason; `written` = how many heads were sent.
    std::string write_heads(const std::vector<HeadWrite>& writes, int* written = nullptr);
    // Load / unload (heats the nozzle and moves filament): explicit user click only.
    std::string feed_head(int head, FeedAction action);

    // The gcode of a head write, exposed for the mock tests. `force` adds FORCE=1.
    static std::string build_filament_config_gcode(const HeadWrite& w, bool force);
    // SET_PRINT_EXTRUDER_MAP lines + SET_PRINT_USED_EXTRUDERS for map[i] = physical head of project filament i (-1 skip).
    static std::string build_extruder_map_gcode(const std::vector<int>& heads_of_filaments);
    // Orca filament type / preset name -> {firmware main type, sub type}.
    static std::pair<std::string, std::string> firmware_type(const std::string& orca_type, const std::string& preset_name);
    // Parses the "status" object of an objects/query reply. False when print_task_config is missing.
    static bool parse_state(const nlohmann::json& status, U1State& out);
    // project filament -> head, from PrintParams.ams_mapping_info (Bambu-style array, item "ams" = tray = head); -1 unmapped.
    static std::vector<int> heads_from_mapping(const std::string& ams_mapping_info);

    // The selected printer's agent when it is a Snapmaker agent (UI thread), else null.
    static std::shared_ptr<SnapmakerPrinterAgent> current();

    // ---- overrides ----
    int  connect_printer(const PrinterConnectionParams& params) override;
    int  disconnect_printer() override;
    int  set_queue_on_main_fn(QueueOnMainFn fn) override;
    int  start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Combine filament_type + filament_sub_type into a unified type string ("PLA" + "Silk" -> "PLA SILK").
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

private:
    // Shared with the worker threads, which only hold a weak_ptr to the agent: a new generation stops the old loop.
    struct Control
    {
        std::atomic<uint64_t>   generation{0};
        std::atomic<bool>       wake{false};
        std::atomic<bool>       write_busy{false};
        std::mutex              mutex;
        std::condition_variable cv;
    };

    bool        query_u1(const std::string& base_url, const std::string& api_key, U1State& out, std::string& error) const;
    static bool post_script(const std::string& base_url, const std::string& api_key, const std::string& script, int timeout_s, std::string& error);
    static void poll_main(std::weak_ptr<SnapmakerPrinterAgent> weak, std::shared_ptr<Control> ctl, uint64_t generation, std::string base_url, std::string api_key);
    void        poll_once(uint64_t generation, const std::string& base_url, const std::string& api_key, int& fails);
    void        store_state(const U1State& state, uint64_t generation);
    void        publish_trays(const U1State& state); // main thread: DevFilaSystem for the print dialog
    void        run_on_main(std::function<void()> fn);
    void        run_write(std::string script, int timeout_s, std::string ok_text, std::string fail_text);
    static std::string state_signature(const U1State& s);

    std::shared_ptr<Control> m_ctl; // made by the constructor
    mutable std::mutex       m_u1_mutex;
    U1State                  m_u1;
    std::string              m_u1_signature;
    QueueOnMainFn            m_queue_main;
};

} // namespace Slic3r
