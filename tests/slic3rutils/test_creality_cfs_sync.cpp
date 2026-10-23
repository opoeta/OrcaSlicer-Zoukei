// Key cases of the "Creality CFS Sync" plugin's pure functions (tests/test_creality_cfs.py of
// the plugin), ported with the logic to slic3r/Utils/CrealityCfs.
#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/CrealityCfs.hpp"

#include <algorithm>
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

    std::vector<ProjectFilament> project(4);
    project[0].name = "Hyper PLA @Creality K2"; project[0].color = "#FFFFFF"; // slot 1A is an RFID spool
    project[1].name = "Hyper PLA @Creality K2"; project[1].color = "#FF0000"; // 1B already matches
    project[2].name = "Hyper PLA @Creality K2"; project[2].color = "#0000FF"; // 1C is empty
    project[3].name = "Hyper PLA @Creality K2"; project[3].color = "#00FF00"; // no slot: never written
    const std::vector<int> targets = {slot_key(1, 0), slot_key(1, 1), slot_key(1, 2), NO_SLOT};
    const auto plan = plan_project_sync(project, presets, snap, targets);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].first.label() == "1C");
    CHECK(plan[0].second.color == "#0000FF");
    CHECK(project_signature(project) != project_signature({project[0]}));

    // the targets decide, not the list position; one slot is never written twice; the external spool never
    const auto moved = plan_project_sync({project[2], project[2]}, presets, snap, {slot_key(1, 3), slot_key(1, 3)});
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].first.label() == "1D");
    CHECK(plan_project_sync({project[2]}, presets, snap, {slot_key(0, 0)}).empty());
    CHECK(plan_project_sync({project[2]}, presets, snap, {slot_key(1, 9)}).empty());
}

TEST_CASE("Creality CFS sync targets follow the mapping, not the list order", "[CrealityCfs]")
{
    // The owner's K2 Plus with two CFS: slot keys 1A..1D = 100..103, 2A..2D = 200..203.
    const int A1 = slot_key(1, 0), B1 = slot_key(1, 1), C1 = slot_key(1, 2), D1 = slot_key(1, 3);
    const int A2 = slot_key(2, 0), B2 = slot_key(2, 1), C2 = slot_key(2, 2), D2 = slot_key(2, 3);
    const std::vector<std::string> old_sigs = {"VOOLT OFFWHITE|#FFFFFF", "Vermelho|#FF0000", "Rosa Shock|#FF00AA", "Preto|#000000",
                                               "Rosa Shock|#FF00AA",     "Verde|#00FF00",    "Amarelo|#FFFF00",    "Preto|#000000"};
    const std::vector<int> old_slots = {A1, A2, B1, C1, B2, D1, C2, D2};
    const std::vector<int> none(8, NO_SLOT);

    SECTION("nothing changed: nothing is written")
    {
        const auto t = sync_targets(old_sigs, old_slots, old_sigs, none, old_slots);
        CHECK(t.write == none);
        CHECK(t.slots == old_slots);
    }
    SECTION("reordering the list never relabels the CFS")
    {
        std::vector<std::string> sigs = old_sigs;
        std::swap(sigs[1], sigs[5]);
        std::swap(sigs[0], sigs[6]);
        std::vector<int> matched = old_slots;
        std::swap(matched[1], matched[5]);
        std::swap(matched[0], matched[6]);
        const auto t = sync_targets(old_sigs, old_slots, sigs, none, matched);
        CHECK(t.write == none);
        CHECK(t.slots == matched);
    }
    SECTION("removing a filament shifts indices but writes nothing")
    {
        std::vector<std::string> sigs = old_sigs;
        sigs.erase(sigs.begin() + 2);
        const auto t = sync_targets(old_sigs, old_slots, sigs, std::vector<int>(7, NO_SLOT), std::vector<int>(7, NO_SLOT));
        CHECK(t.write == std::vector<int>(7, NO_SLOT));
        CHECK(t.slots[2] == C1); // "Preto" keeps 1C
        CHECK(t.slots[3] == B2); // the second pink keeps 2B
    }
    SECTION("an edited filament updates the slot it occupies")
    {
        std::vector<std::string> sigs = old_sigs;
        sigs[1]                     = "Vermelho Silk|#FF0000"; // new preset, same colour: still matched to 2A
        sigs[6]                     = "Laranja|#FF8000";       // new colour: no slot matches, keeps 2C
        std::vector<int> matched    = old_slots;
        matched[6]                  = NO_SLOT;
        const auto t                = sync_targets(old_sigs, old_slots, sigs, none, matched);
        std::vector<int> expected   = none;
        expected[1]                 = A2;
        expected[6]                 = C2;
        CHECK(t.write == expected);
        CHECK(t.slots == old_slots);
    }
    SECTION("an edited filament never takes the slot of an unchanged one")
    {
        // pink 1B is recoloured blue; the remaining pink must stay on 2B even if the badge now matches it to 1B
        std::vector<std::string> sigs = old_sigs;
        sigs[2]                     = "Rosa Shock|#0000FF";
        std::vector<int> matched    = old_slots;
        matched[2]                  = NO_SLOT;
        matched[4]                  = B1;
        const auto t                = sync_targets(old_sigs, old_slots, sigs, none, matched);
        CHECK(t.write[2] == B1);
        CHECK(t.write[4] == NO_SLOT);
        CHECK(t.slots[4] == B2);
    }
    SECTION("a new filament without a matching slot writes nothing unless a slot was picked")
    {
        std::vector<std::string> sigs = old_sigs;
        sigs.push_back("Azul|#0000FF");
        std::vector<int> matched = old_slots;
        matched.push_back(NO_SLOT);
        std::vector<int> chosen(9, NO_SLOT);
        auto             t = sync_targets(old_sigs, old_slots, sigs, chosen, matched);
        CHECK(t.write == std::vector<int>(9, NO_SLOT));
        chosen[8] = D2; // picked explicitly in the slot picker
        matched[8] = D2;
        t          = sync_targets(old_sigs, old_slots, sigs, chosen, matched);
        CHECK(t.write[8] == D2);
        CHECK(std::count(t.write.begin(), t.write.end(), NO_SLOT) == 8);
    }
    SECTION("two different filaments on one slot: the slot is left alone")
    {
        std::vector<std::string> sigs = old_sigs;
        sigs[0]                     = "Branco|#FFFFFF";
        sigs[3]                     = "Cinza|#808080";
        std::vector<int> chosen     = none;
        chosen[0] = chosen[3]       = D1;
        const auto t                = sync_targets(old_sigs, old_slots, sigs, chosen, chosen);
        CHECK(t.write == none);
        CHECK(t.conflicts == 1);
    }
    SECTION("identical filaments on one slot: written once")
    {
        std::vector<std::string> sigs = old_sigs;
        sigs[3] = sigs[7]       = "Preto Fosco|#000000";
        std::vector<int> chosen = none;
        chosen[3] = chosen[7]   = C1;
        const auto t            = sync_targets(old_sigs, old_slots, sigs, chosen, chosen);
        CHECK(t.write[3] == C1);
        CHECK(t.write[7] == NO_SLOT);
        CHECK(t.conflicts == 0);
    }
    SECTION("U1 / IFS keys (plain head or port numbers) work the same")
    {
        const auto t = sync_targets({"a", "b"}, {1, 2}, {"b", "a"}, {-1, -1}, {2, 1});
        CHECK(t.write == std::vector<int>{-1, -1});
    }
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
