// Key cases of the "Creality CFS Sync" plugin's pure functions (tests/test_creality_cfs.py of
// the plugin), ported with the logic to slic3r/Utils/CrealityCfs.
#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/CrealityCfs.hpp"

#include <clocale>
#include <string>

using namespace Slic3r::CrealityCfs;
using json = nlohmann::json;

namespace {

json boxes_json()
{
    return json::parse(R"({
        "materialBoxs": [
            {"id": 0, "type": 1, "state": 1, "materials": [{"id": 0, "state": 1, "vendor": "Generic", "type": "PLA", "name": "Generic PLA", "color": "#0ffffff"}]},
            {"id": 1, "type": 0, "state": 1, "temp": 25.5, "humidity": 45, "materials": [
                {"id": 1, "state": 1, "vendor": "Creality", "type": "PLA", "name": "Hyper PLA", "color": "#0ff0000", "minTemp": "190", "maxTemp": "230", "percent": 80, "rfid": "01001"},
                {"id": 0, "state": 2, "vendor": "Creality", "type": "PETG", "name": "Hyper PETG", "color": "#000ff00", "rfid": "06002"},
                {"id": 2, "state": 0},
                {"id": 3, "state": 1, "vendor": "", "type": "", "name": "", "color": ""}
            ]}
        ]
    })");
}

std::vector<MaterialLibraryEntry> library()
{
    return {
        {"00001", "Generic", "PLA", "Generic PLA", 190, 230, 0.04, {}},
        {"01001", "Creality", "PLA", "Hyper PLA", 190, 240, 0.036, {}},
        {"06001", "Generic", "PETG", "Generic PETG", 220, 260, 0.05, {}},
    };
}

} // namespace

TEST_CASE("Creality CFS colours", "[CrealityCfs]")
{
    CHECK(normalize_hex("#0ff8800") == "#FF8800");
    CHECK(normalize_hex("FF8800FF") == "#FF8800");
    CHECK(normalize_hex("0xff8800") == "#FF8800");
    CHECK(normalize_hex("FFFF8800") == "#FF8800");
    CHECK(normalize_hex("nonsense", "") == "");
    CHECK(hex_to_creality("#FF8800") == "#0ff8800");
    CHECK(hex_to_bbl("#ff8800") == "FF8800FF");
    CHECK(normalize_filament_type("PLA Silk") == "PLA");
    CHECK(normalize_filament_type("PETG-CF") == "PETG");
    CHECK(normalize_filament_type("Nylon") == "Nylon");
}

TEST_CASE("Creality CFS boxsInfo parsing", "[CrealityCfs]")
{
    const auto boxes = parse_boxes(boxes_json());
    REQUIRE(boxes.size() == 2);
    const CfsBox& cfs = boxes[1];
    CHECK(cfs.is_cfs());
    REQUIRE(cfs.slots.size() == 4);
    CHECK(cfs.slots[0].slot_id == 0); // sorted by id
    CHECK(cfs.slots[0].is_rfid_locked());
    CHECK(cfs.slots[1].color == "#FF0000");
    CHECK(cfs.slots[1].min_temp == 190);
    CHECK(cfs.slots[1].label() == "1B");
    CHECK_FALSE(cfs.slots[2].is_loaded());
    CHECK_FALSE(cfs.slots[3].is_loaded());
}

TEST_CASE("Creality CFS status folds into the snapshot", "[CrealityCfs]")
{
    PrinterSnapshot snap;
    snap.connected = true;
    apply_status(snap, json{{"deviceState", 0}, {"feedState", 0}, {"nozzleTemp", "210.5"}, {"boxsInfo", boxes_json()}});
    CHECK(snap.is_idle());
    CHECK(check_writable(snap).empty());
    REQUIRE(snap.nozzle_temp);
    CHECK(*snap.nozzle_temp == 210.5);
    CHECK(snap.cfs_boxes().size() == 1);
    apply_status(snap, json{{"deviceState", 1}});
    CHECK_FALSE(check_writable(snap).empty());
}

TEST_CASE("Creality K2 string temperatures survive a comma-decimal locale and partial messages", "[CrealityCfs]")
{
    // Recorded K2 Plus status during a print: temperatures are strings with a dot.
    const json full = json::parse(R"({"bedTemp0": "50.100000", "bedTemp1": "0.000000", "bedTemp2": "0.000000",
        "targetBedTemp0": "50", "nozzleTemp": "139.800000", "targetNozzleTemp": 140, "boxTemp": 28})");

    // Under pt_BR strtod stops at the dot (50.1 -> "50" + garbage -> used to become 0). Locale may be missing on the host.
    const std::string old_locale = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : {"pt_BR.UTF-8", "pt_BR", "Portuguese_Brazil.1252", "de_DE.UTF-8", "German_Germany.1252"})
        if (std::setlocale(LC_NUMERIC, name))
            break;

    PrinterSnapshot snap;
    apply_status(snap, full);
    REQUIRE(snap.bed_temp);
    CHECK(*snap.bed_temp == 50.1);
    REQUIRE(snap.bed_target);
    CHECK(*snap.bed_target == 50.);
    REQUIRE(snap.nozzle_temp);
    CHECK(*snap.nozzle_temp == 139.8);
    REQUIRE(snap.nozzle_target);
    CHECK(*snap.nozzle_target == 140.);
    REQUIRE(snap.box_temp);
    CHECK(*snap.box_temp == 28.);

    // Incremental message without the temperatures, or with unreadable ones: the last known values stay.
    apply_status(snap, json{{"printProgress", 12}});
    apply_status(snap, json{{"nozzleTemp", "xxx.xxxxxx"}, {"bedTemp0", ""}, {"boxTemp", nullptr}});
    CHECK(*snap.bed_temp == 50.1);
    CHECK(*snap.nozzle_temp == 139.8);
    CHECK(*snap.box_temp == 28.);

    // A real new value (also a genuine 0) replaces them.
    apply_status(snap, json{{"bedTemp0", "0.000000"}});
    CHECK(*snap.bed_temp == 0.);

    std::setlocale(LC_NUMERIC, old_locale.c_str());
}

TEST_CASE("Creality CFS AMS payload", "[CrealityCfs]")
{
    PrinterSnapshot snap;
    apply_status(snap, json{{"boxsInfo", boxes_json()}});
    const json ams = build_ams_json(snap);
    REQUIRE(ams["ams"].size() == 1); // the external holder (type 1) is not an AMS
    CHECK(ams["ams_exist_bits"] == "1");
    CHECK(ams["tray_exist_bits"] == "3"); // slots 0 and 1 hold filament
    const json& trays = ams["ams"][0]["tray"];
    REQUIRE(trays.size() == 4);
    CHECK(trays[1]["tray_color"] == "FF0000FF");
    CHECK(trays[1]["tray_info_idx"] == "CFS-01001");
    CHECK(trays[2]["tray_slot_placeholder"] == "1");
    CHECK(ams["ams"][0]["humidity"] == "3");
}

TEST_CASE("Creality CFS preset and library matching", "[CrealityCfs]")
{
    std::vector<PresetInfo> presets(3);
    presets[0].name = "Generic PLA @Creality K2";   presets[0].type = "PLA"; presets[0].vendor = "Generic";  presets[0].is_system = true; presets[0].filament_id = "GFL99";
    presets[1].name = "Hyper PLA @Creality K2";     presets[1].type = "PLA"; presets[1].vendor = "Creality"; presets[1].is_system = true; presets[1].filament_id = "CRE01";
    presets[2].name = "Generic PLA Matte @Creality K2"; presets[2].type = "PLA"; presets[2].vendor = "Generic"; presets[2].is_system = true;

    CfsSlot hyper;
    hyper.state = 1; hyper.vendor = "Creality"; hyper.type = "PLA"; hyper.name = "Hyper PLA";
    REQUIRE(match_preset_for_slot(presets, hyper) != nullptr);
    CHECK(match_preset_for_slot(presets, hyper)->name == "Hyper PLA @Creality K2");

    CfsSlot plain;
    plain.state = 1; plain.vendor = "Generic"; plain.type = "PLA"; plain.name = "PLA";
    REQUIRE(match_preset_for_slot(presets, plain) != nullptr);
    CHECK(match_preset_for_slot(presets, plain)->name == "Generic PLA @Creality K2"); // least specific wins

    const auto lib = library();
    CHECK(match_library_entry(lib, "Creality", "PLA", "Hyper PLA")->id == "01001");
    CHECK(match_library_entry(lib, "eSUN", "PETG", "PETG Basic")->id == "06001"); // generic of the type
    CHECK(match_library_entry(lib, "eSUN", "TPU", "TPU 95A") == nullptr);

    const MaterialSpec spec = spec_from_preset(presets[1], "#00FF00", lib);
    CHECK(spec.rfid == "01001");
    CHECK(spec.color == "#00FF00");
    CHECK(spec.max_temp == 240);
}

TEST_CASE("Creality CFS project sync plan", "[CrealityCfs]")
{
    PrinterSnapshot snap;
    apply_status(snap, json{{"boxsInfo", boxes_json()}});
    std::vector<PresetInfo> presets(1);
    presets[0].name = "Hyper PLA @Creality K2"; presets[0].type = "PLA"; presets[0].vendor = "Creality";

    std::vector<ProjectFilament> project(3);
    project[0].name = "Hyper PLA @Creality K2"; project[0].color = "#FFFFFF"; // slot 1A is an RFID spool
    project[1].name = "Hyper PLA @Creality K2"; project[1].color = "#FF0000"; // 1B already matches
    project[2].name = "Hyper PLA @Creality K2"; project[2].color = "#0000FF"; // 1C is empty
    const auto plan = plan_project_sync(project, presets, snap);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].first.label() == "1C");
    CHECK(plan[0].second.color == "#0000FF");
    CHECK(project_signature(project) != project_signature({project[0]}));
}

TEST_CASE("Creality CFS ams_filament_setting", "[CrealityCfs]")
{
    std::vector<PresetInfo> presets(1);
    presets[0].name = "Hyper PLA @Creality K2"; presets[0].type = "PLA"; presets[0].vendor = "Creality"; presets[0].filament_id = "CRE01";
    MaterialSpec spec;
    int          ams = -1, slot = -1;
    REQUIRE(ams_setting_to_spec(json{{"ams_id", 0}, {"slot_id", "2"}, {"tray_info_idx", "CRE01"}, {"tray_color", "FF0000FF"}}, presets, library(), spec, ams, slot));
    CHECK(ams == 0);
    CHECK(slot == 2);
    CHECK(spec.name == "Hyper PLA");
    CHECK(spec.color == "#FF0000");
    CHECK(spec.to_params(1, 2)["modifyMaterial"]["color"] == "#0ff0000");
    REQUIRE(ams_setting_to_spec(json{{"ams_id", 0}, {"slot_id", 1}, {"tray_info_idx", ""}, {"tray_type", ""}}, presets, library(), spec, ams, slot));
    CHECK(spec.is_clear());
    CHECK(fallback_model_id("Creality K2 Plus") == "Creality-K2-Plus");
}
