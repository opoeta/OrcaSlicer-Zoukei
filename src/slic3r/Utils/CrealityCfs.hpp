// Creality CFS (Creality Filament System) data model and the pure logic of the native
// "creality_cfs" printer agent: parsing the printer's boxsInfo, matching slots to Orca filament
// presets and to the printer's material library, planning a project -> CFS sync and building the
// Bambu-dialect status OrcaSlicer's device model understands.
//
// Ported to C++ from the "Creality CFS Sync" OrcaSlicer plugin by Heyde Moura (creality_cfs.py,
// v0.1.0, with the Orca Zoukei model_id fix). No GUI dependencies, so it is unit testable.
#ifndef slic3r_Utils_CrealityCfs_hpp_
#define slic3r_Utils_CrealityCfs_hpp_

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace CrealityCfs {

constexpr int DEVICE_STATE_IDLE     = 0;
constexpr int DEVICE_STATE_PRINTING = 1;
constexpr int SLOT_STATE_EMPTY      = 0;
constexpr int SLOT_STATE_RFID       = 2;
constexpr int BOX_TYPE_CFS          = 0;
constexpr int BOX_TYPE_EXTERNAL     = 1;
constexpr int BOX_TYPE_CFS_MINI     = 2;

// "#RRGGBB" (upper case) for the colour spellings seen in the wild ("#0RRGGBB", "RRGGBBAA",
// "0xRRGGBB", "AARRGGBB"); `fallback` when unreadable.
std::string normalize_hex(const std::string& color, const std::string& fallback = "#FFFFFF");
// "#RRGGBB" -> "#0rrggbb", what the printer stores.
std::string hex_to_creality(const std::string& color);
// "#RRGGBB" -> "RRGGBBFF", what Orca's AMS parser expects.
std::string hex_to_bbl(const std::string& color);
// "PLA Silk" / "PLA+" / "PLA-CF" -> "PLA".
std::string normalize_filament_type(const std::string& filament_type);

struct MaterialLibraryEntry
{
    std::string              id;
    std::string              brand;
    std::string              type;
    std::string              name;
    int                      min_temp{0};
    int                      max_temp{0};
    double                   pressure_advance{0.};
    std::vector<std::string> colors;
    bool                     is_generic() const;
};

struct CfsSlot
{
    int         box_id{0};
    int         slot_id{0};
    int         state{SLOT_STATE_EMPTY};
    std::string vendor;
    std::string type;
    std::string name;
    std::string rfid;
    std::string color; // "#RRGGBB" or ""
    int         min_temp{0};
    int         max_temp{0};
    double      pressure{0.};
    int         percent{0};
    bool        selected{false};
    int         edit_status{0};

    bool        is_loaded() const;
    bool        is_rfid_locked() const { return state == SLOT_STATE_RFID; }
    std::string label() const; // "1A"
};

struct CfsBox
{
    int                   id{0};
    int                   type{0};
    int                   state{0};
    std::optional<double> temp;
    std::optional<double> humidity;
    std::vector<CfsSlot>  slots;

    bool is_cfs() const { return type == BOX_TYPE_CFS || type == BOX_TYPE_CFS_MINI; }
    bool is_active() const { return state == 1; }
};

// What to write into a slot; all empty clears it.
struct MaterialSpec
{
    std::string vendor;
    std::string type;
    std::string name;
    std::string color; // "#RRGGBB" or ""
    std::string rfid;  // printer material library id when known
    int         min_temp{0};
    int         max_temp{0};
    double      pressure{0.};

    bool           is_clear() const { return vendor.empty() && type.empty() && name.empty() && color.empty(); }
    nlohmann::json to_params(int box_id, int slot_id) const; // {"modifyMaterial": {...}}
};

struct PrinterSnapshot
{
    std::string                       host;
    bool                              connected{false};
    std::string                       model;
    std::string                       hostname;
    std::string                       mac;
    std::string                       firmware;
    std::optional<int>                device_state;
    std::optional<int>                feed_state;
    std::optional<bool>               cfs_connected;
    std::optional<int>                ai_first_floor;
    std::string                       print_file;
    std::optional<int>                print_progress;
    std::optional<int>                print_left_seconds;
    std::optional<int>                layer;
    std::optional<int>                total_layers;
    std::optional<double>             nozzle_temp;
    std::optional<double>             nozzle_target;
    std::optional<double>             bed_temp;
    std::optional<double>             bed_target;
    std::optional<double>             box_temp;
    std::vector<CfsBox>               boxes;
    nlohmann::json                    box_config = nlohmann::json::object();
    std::vector<MaterialLibraryEntry> library;
    std::string                       last_error;

    bool                is_idle() const;
    std::vector<CfsBox> cfs_boxes() const; // active CFS units, in id order
    const CfsSlot*      find_slot(int box_id, int slot_id) const;
};

std::vector<CfsBox>               parse_boxes(const nlohmann::json& boxs_info);
std::vector<MaterialLibraryEntry> parse_material_library(const nlohmann::json& ret_materials);
// Folds a status/push message from the printer into the snapshot.
void apply_status(PrinterSnapshot& snapshot, const nlohmann::json& msg);

// Why slot writes must wait (printer printing, feeding, no CFS...), empty when writable.
std::string check_writable(const PrinterSnapshot& snapshot);

// The plain data of an Orca filament preset the matching needs.
struct PresetInfo
{
    std::string name;
    std::string vendor;
    std::string type;
    std::string filament_id;
    int         min_temp{0};
    int         max_temp{0};
    double      pressure_advance{0.};
    std::string color;
    bool        is_system{false};
    bool        is_visible{true};
    bool        is_compatible{true};

    std::string base_name() const; // "Hyper PLA @Creality K1 Max" -> "Hyper PLA"
};

const MaterialLibraryEntry* match_library_entry(const std::vector<MaterialLibraryEntry>& library, const std::string& vendor,
                                                const std::string& filament_type, const std::string& name);
const PresetInfo*           match_preset_for_slot(const std::vector<PresetInfo>& presets, const CfsSlot& slot);

// The slot record for a slicer preset: identity from the printer library when it has the
// material, numbers from the preset unless temps_from == "library".
MaterialSpec spec_from_preset(const PresetInfo& preset, const std::string& color, const std::vector<MaterialLibraryEntry>& library,
                              const std::string& temps_from = "preset");
// The slot already holds what the spec describes.
bool slot_matches(const CfsSlot& slot, const MaterialSpec& spec);

// One project filament (sidebar entry).
struct ProjectFilament
{
    std::string name;
    std::string color;
    std::string type;
    std::string vendor;
    std::string filament_id;
};

std::string project_signature(const std::vector<ProjectFilament>& project);
// Slots that must change so that CFS slot i holds project filament i (RFID spools untouched).
std::vector<std::pair<CfsSlot, MaterialSpec>> plan_project_sync(const std::vector<ProjectFilament>& project,
                                                                const std::vector<PresetInfo>& presets, const PrinterSnapshot& snapshot,
                                                                const std::string& temps_from = "preset");

using PresetForSlot = std::function<const PresetInfo*(const CfsSlot&)>;
nlohmann::json build_ams_json(const PrinterSnapshot& snapshot, const PresetForSlot& preset_for_slot = {});
nlohmann::json build_push_status(const PrinterSnapshot& snapshot, int sequence_id, const PresetForSlot& preset_for_slot = {});
nlohmann::json build_get_version(const PrinterSnapshot& snapshot, int sequence_id);

// Orca's ams_filament_setting command -> slot write. Returns false when malformed; (ams_index, slot_id)
// address the CFS unit in cfs_boxes() order.
bool ams_setting_to_spec(const nlohmann::json& print_cmd, const std::vector<PresetInfo>& presets,
                         const std::vector<MaterialLibraryEntry>& library, MaterialSpec& spec, int& ams_index, int& slot_id);

// Vendor model_id for a "Creality ..." printer_model name when the vendor profile is not at hand
// (OrcaSlicer compares the device's model against model_id, not the name).
std::string fallback_model_id(const std::string& printer_model);

}} // namespace Slic3r::CrealityCfs

#endif // slic3r_Utils_CrealityCfs_hpp_
