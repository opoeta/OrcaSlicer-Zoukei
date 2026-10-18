// src/slic3r/GUI/RemoteAPI/RemoteAPICalib.cpp
//
// Orca Zoukei Remote API, group G6: Orca's built-in calibrations (the Calibration menu). Every handler runs on
// the UI thread (run_on_ui). Starting one does exactly what the menu dialog does on OK: it validates the values
// with the dialog's own rules and calls the same Plater::calib_* function, which replaces the current project with
// the calibration project (model + modified print/filament/printer settings). Nothing is sliced or sent to a
// printer here; slice afterwards (POST /slice) and print from the window.
//
//   GET  /api/v1/calibrations                  the calibrations, their parameters (type, default, limits) and the
//                                              current printer context (G-code flavor, belt printer, shaper types)
//   POST /api/v1/calibrations/{kind}/start     {discard_changes?, <parameters of the kind>}
//
// kinds: temperature, max_volumetric_speed, pressure_advance, flow_rate, retraction, vfa, cornering,
//        input_shaping_freq, input_shaping_damp
//
// The calibration uses the presets that are selected now (printer, print, filament): choose the filament /
// printer / plate first (PUT /preset, plates endpoints). A running project with unsaved changes is refused
// (409 unsaved_changes) unless "discard_changes": true, like POST /project/new.
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "libslic3r/Flow.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <algorithm>
#include <cmath>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

namespace {

const DynamicPrintConfig &printer_cfg()
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().config;
}

bool belt_printer()
{
    const auto *opt = printer_cfg().option<ConfigOptionBool>("belt_printer");
    return opt != nullptr && opt->value;
}

GCodeFlavor gcode_flavor()
{
    const auto *opt = printer_cfg().option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    return opt != nullptr ? opt->value : gcfRepRapSprinter;
}

bool uses_junction_deviation()
{
    const auto *opt = printer_cfg().option<ConfigOptionFloats>("machine_max_junction_deviation");
    return gcode_flavor() == gcfMarlinFirmware && opt != nullptr && !opt->values.empty() && opt->values[0] > 0;
}

// Same list as the input shaping dialogs (Disable is not offered; the first entry is the default).
std::vector<std::string> shaper_values()
{
    const ConfigOptionDef *def = printer_cfg().def()->get("input_shaping_type");
    std::vector<std::string> values;
    for (InputShaperType type : input_shaper_types_for_flavor(gcode_flavor())) {
        if (type == InputShaperType::Disable)
            continue;
        const size_t idx = static_cast<size_t>(type);
        if (def != nullptr && idx < def->enum_values.size())
            values.push_back(def->enum_values[idx]);
        else
            values.push_back(std::to_string(static_cast<int>(type)));
    }
    if (values.empty() && def != nullptr && !def->enum_values.empty())
        values.push_back(def->enum_values.front());
    return values;
}

bool project_dirty(Plater *p)
{
    return !p->up_to_date(false, false) || p->is_project_dirty() || wxGetApp().has_unsaved_preset_changes();
}

const char *mode_name(CalibMode m)
{
    switch (m) {
    case CalibMode::Calib_PA_Line:             return "pressure_advance_line";
    case CalibMode::Calib_PA_Pattern:          return "pressure_advance_pattern";
    case CalibMode::Calib_PA_Tower:            return "pressure_advance_tower";
    case CalibMode::Calib_Auto_PA_Line:        return "auto_pa_line";
    case CalibMode::Calib_Flow_Rate:           return "flow_rate";
    case CalibMode::Calib_Temp_Tower:          return "temperature_tower";
    case CalibMode::Calib_Vol_speed_Tower:     return "max_volumetric_speed_tower";
    case CalibMode::Calib_VFA_Tower:           return "vfa_tower";
    case CalibMode::Calib_Retraction_tower:    return "retraction_tower";
    case CalibMode::Calib_Input_shaping_freq:  return "input_shaping_freq";
    case CalibMode::Calib_Input_shaping_damp:  return "input_shaping_damp";
    case CalibMode::Calib_Cornering:           return "cornering";
    default:                                   return "none";
    }
}

// ---- catalog ---------------------------------------------------------------------------------

json num_param(const char *name, double def, const char *unit, const char *rule)
{
    return {{"name", name}, {"type", "number"}, {"default", def}, {"unit", unit}, {"rule", rule}};
}

json bool_param(const char *name, bool def, const char *what)
{
    return {{"name", name}, {"type", "boolean"}, {"default", def}, {"what", what}};
}

json enum_param(const char *name, const json &values, const json &def, const char *what)
{
    return {{"name", name}, {"type", "enum"}, {"values", values}, {"default", def}, {"what", what}};
}

json catalog()
{
    const bool jd = uses_junction_deviation();
    const json shapers = shaper_values();
    json cal = json::array();
    cal.push_back({{"kind", "temperature"}, {"menu", "Calibration > Temperature"},
                   {"what", "Temperature tower: one block per 5 C step from start (bottom) down to end (top)."},
                   {"parameters", json::array({
                       enum_param("filament_type", json::array({"pla", "abs_asa", "petg", "pctg", "tpu", "pa_cf", "pet_cf", "custom"}), "pla",
                                  "Preset of the range (the dialog's filament type); start/end override it. pla 230-190, abs_asa 270-230, petg 250-230, pctg 280-240, tpu 240-210, pa_cf/pet_cf 320-280."),
                       num_param("start", 230, "C", "start <= 500"),
                       num_param("end", 190, "C", "end >= 155 and start >= end + 5"),
                       bool_param("auto_scale", true, "Scale the tower to the nozzle diameter and set a matching layer height."),
                       enum_param("test_model", json::array({0, 1}), 0, "0 standard tower; 1 overhang model (belt printers only).")})}});
    cal.push_back({{"kind", "max_volumetric_speed"}, {"menu", "Calibration > Max flowrate"},
                   {"what", "Max volumetric speed tower: the flow rate rises step by step; the last good step is the filament's limit."},
                   {"parameters", json::array({
                       num_param("start", 5, "mm3/s", "start > 0"),
                       num_param("end", 20, "mm3/s", "end >= start + step"),
                       num_param("step", 0.5, "mm3/s", "step > 0")})}});
    cal.push_back({{"kind", "pressure_advance"}, {"menu", "Calibration > Pressure advance"},
                   {"what", "Pressure advance: tower, line or pattern test."},
                   {"parameters", json::array({
                       enum_param("method", json::array({"tower", "line", "pattern"}), "tower", "Test type. Belt printers: tower only."),
                       enum_param("extruder_type", json::array({"dde", "bowden"}), "dde", "Only changes the defaults below (bowden: end 1.0, step 0.02 / pattern 0.05)."),
                       num_param("start", 0.0, "", "start >= 0"),
                       num_param("end", 0.1, "", "end >= start + step (dde: 0.1 / pattern 0.08; bowden: 1.0)"),
                       num_param("step", 0.002, "", "step >= 0.001 (dde: 0.002 / pattern 0.005; bowden: 0.02 / pattern 0.05)"),
                       bool_param("print_numbers", true, "line only: print the PA value next to each line (tower: always off, pattern: always on)."),
                       {{"name", "accelerations"}, {"type", "number list"}, {"default", json::array()}, {"what", "pattern only: accelerations in mm/s2 to test (must exceed every speed)."}},
                       {{"name", "speeds"}, {"type", "number list"}, {"default", json::array()}, {"what", "pattern only: speeds in mm/s to test."}}})}});
    cal.push_back({{"kind", "flow_rate"}, {"menu", "Calibration > Flow ratio"},
                   {"what", "Flow ratio test plates (needs no numeric values: the model and the settings come with the test)."},
                   {"parameters", json::array({
                       enum_param("type", json::array({"pass1", "pass2", "yolo", "yolo_perfectionist"}), "yolo",
                                  "pass1 coarse, pass2 fine, yolo (recommended), yolo_perfectionist."),
                       enum_param("pattern", json::array({"archimedean", "monotonic"}), "archimedean", "Top surface pattern of the test pieces.")})}});
    cal.push_back({{"kind", "retraction"}, {"menu", "Calibration > Retraction"},
                   {"what", "Retraction length tower."},
                   {"parameters", json::array({
                       num_param("start", 0, "mm", "start >= 0"),
                       num_param("end", 2, "mm", "end >= start + step"),
                       num_param("step", 0.1, "mm", "step > 0")})}});
    cal.push_back({{"kind", "vfa"}, {"menu", "Calibration > VFA"},
                   {"what", "Vertical fine artifacts: outer wall speed tower."},
                   {"parameters", json::array({
                       num_param("start", 40, "mm/s", "start > 10"),
                       num_param("end", 200, "mm/s", "end >= start + step"),
                       num_param("step", 10, "mm/s", "step > 0"),
                       bool_param("auto_scale", true, "Scale the tower to the nozzle diameter."),
                       num_param("layer_height", 0, "mm", "0 = automatic (nozzle / 2). A larger end speed than the filament's max volumetric speed allows is clamped by the slicer; the reply warns about it.")})}});
    cal.push_back({{"kind", "cornering"}, {"menu", "Calibration > Cornering"},
                   {"what", "Cornering (junction deviation or classic jerk) test tower."},
                   {"parameters", json::array({
                       num_param("start", jd ? 0.0 : 1.0, jd ? "mm" : "mm/s", "start >= 0"),
                       num_param("end", jd ? 0.25 : 15.0, jd ? "mm" : "mm/s", jd ? "end <= 0.3 and end > start" : "end <= 100 and end > start"),
                       enum_param("test_model", json::array({0, 1, 2}), 0, "0 ringing tower, 1 fast tower, 2 SCV-V2.")})}});
    cal.push_back({{"kind", "input_shaping_freq"}, {"menu", "Calibration > Input Shaping > Frequency"},
                   {"what", "Input shaping frequency sweep."},
                   {"parameters", json::array({
                       num_param("freq_start_x", 15, "Hz", "0 <= start < end <= 500"),
                       num_param("freq_end_x", 110, "Hz", ""),
                       num_param("freq_start_y", 15, "Hz", "ignored on RepRapFirmware (uses the X range)"),
                       num_param("freq_end_y", 110, "Hz", "ignored on RepRapFirmware"),
                       num_param("damping", 0.15, "", "0 <= damping < 1"),
                       enum_param("shaper_type", shapers, shapers.empty() ? json("") : shapers[0], "Input shaper of the printer's firmware."),
                       enum_param("test_model", json::array({0, 1}), 0, "0 ringing tower, 1 fast tower.")})}});
    cal.push_back({{"kind", "input_shaping_damp"}, {"menu", "Calibration > Input Shaping > Damping"},
                   {"what", "Input shaping damping sweep at a fixed frequency."},
                   {"parameters", json::array({
                       num_param("freq_x", 30, "Hz", "0 <= freq <= 500"),
                       num_param("freq_y", 30, "Hz", "ignored on RepRapFirmware (uses freq_x)"),
                       num_param("damping_start", 0.0, "", "0 <= damping_start < damping_end"),
                       num_param("damping_end", 0.4, "", "damping_end <= 1"),
                       enum_param("shaper_type", shapers, shapers.empty() ? json("") : shapers[0], "Input shaper of the printer's firmware."),
                       enum_param("test_model", json::array({0, 1}), 0, "0 ringing tower, 1 fast tower.")})}});

    const PresetBundle *bundle = wxGetApp().preset_bundle;
    return {{"calibrations", cal},
            {"context", {{"printer", bundle->printers.get_selected_preset_name()},
                         {"print", bundle->prints.get_selected_preset_name()},
                         {"filaments", bundle->filament_presets},
                         {"gcode_flavor", printer_cfg().opt_serialize("gcode_flavor")},
                         {"belt_printer", belt_printer()},
                         {"junction_deviation", uses_junction_deviation()},
                         {"shaper_types", shapers}}},
            {"note", "starting one replaces the current project with the calibration project (same as the menu); "
                     "the selected printer / print / filament presets are used. Then slice with POST /slice."}};
}

// ---- parameter helpers -----------------------------------------------------------------------

double need_range_num(const json &body, const char *key, double def)
{
    return zk::opt_num(body, key, def);
}

std::string pick(const json &body, const char *key, const std::vector<std::string> &allowed, const std::string &def)
{
    const std::string v = zk::lower(zk::opt_str(body, key, def));
    if (std::find(allowed.begin(), allowed.end(), v) == allowed.end()) {
        std::string list;
        for (const std::string &a : allowed)
            list += (list.empty() ? "" : ", ") + a;
        zk::fail("bad_value", std::string("'") + key + "' must be one of: " + list);
    }
    return v;
}

int pick_model(const json &body, int max_model)
{
    const double v = zk::opt_num(body, "test_model", 0);
    if (v != std::floor(v) || v < 0 || v > max_model)
        zk::fail("bad_value", "'test_model' must be an integer 0.." + std::to_string(max_model));
    return int(v);
}

std::vector<double> positive_int_list(const json &body, const char *key)
{
    std::vector<double> out;
    if (!body.contains(key) || body[key].is_null())
        return out;
    if (!body[key].is_array())
        zk::fail("bad_value", std::string("'") + key + "' must be a list of numbers");
    for (const json &v : body[key]) {
        if (!v.is_number())
            zk::fail("bad_value", std::string("'") + key + "' must be a list of numbers");
        const int x = int(v.get<double>());   // the dialog parses whole numbers and drops values <= 0
        if (x > 0)
            out.push_back(x);
    }
    return out;
}

std::string resolve_shaper(const json &body)
{
    const std::vector<std::string> values = shaper_values();
    if (values.empty())
        return std::string();
    if (!body.contains("shaper_type") || body["shaper_type"].is_null())
        return values.front();
    const std::string want = zk::opt_str(body, "shaper_type");
    for (const std::string &v : values)
        if (zk::lower(v) == zk::lower(want))
            return v;
    std::string list;
    for (const std::string &v : values)
        list += (list.empty() ? "" : ", ") + v;
    zk::fail("bad_value", "'shaper_type' must be one of: " + list);
}

// ---- start -----------------------------------------------------------------------------------

json start_calibration(const std::string &kind, const json &body)
{
    static const std::vector<std::string> kinds = {"temperature", "max_volumetric_speed", "pressure_advance", "flow_rate", "retraction",
                                                   "vfa", "cornering", "input_shaping_freq", "input_shaping_damp"};
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
        zk::fail("unknown_calibration", "see GET /calibrations for the kinds");

    Plater *p = wxGetApp().plater();
    if (p->printer_technology() != ptFFF)
        zk::fail("not_fff", "calibrations are for FFF printers");
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for or cancel the running slice first");

    // ---- validate exactly like the dialog's on_start (before the project is touched) -----------
    Calib_Params params;
    json         requested = json::object();
    json         warnings  = json::array();
    int          flow_pass = 0;
    bool         flow_linear = false;
    InfillPattern flow_pattern = ipArchimedeanChords;

    if (kind == "temperature") {
        double start = 230, end = 190;
        const std::string ft = pick(body, "filament_type", {"pla", "abs_asa", "petg", "pctg", "tpu", "pa_cf", "pet_cf", "custom"}, "pla");
        if (ft == "abs_asa")                         { start = 270; end = 230; }
        else if (ft == "petg")                       { start = 250; end = 230; }
        else if (ft == "pctg")                       { start = 280; end = 240; }
        else if (ft == "tpu")                        { start = 240; end = 210; }
        else if (ft == "pa_cf" || ft == "pet_cf")    { start = 320; end = 280; }
        start = need_range_num(body, "start", start);
        end   = need_range_num(body, "end", end);
        if (start > 500 || end < 155 || end > start - 5)
            zk::fail("bad_value", "need: start <= 500, end >= 155, start >= end + 5");
        params.start = std::floor(start);          // the dialog reads whole numbers
        params.end   = std::floor(end);
        params.nozzle_based_resize = zk::opt_bool(body, "auto_scale", true);
        params.test_model          = belt_printer() ? pick_model(body, 1) : 0;
        if (!belt_printer() && body.contains("test_model") && zk::opt_num(body, "test_model", 0) != 0)
            zk::fail("bad_value", "test_model 1 (overhang) exists on belt printers only");
        params.mode = CalibMode::Calib_Temp_Tower;
        requested = {{"filament_type", ft}, {"start", params.start}, {"end", params.end}, {"step", 5}, {"auto_scale", params.nozzle_based_resize},
                     {"test_model", params.test_model}};
    } else if (kind == "max_volumetric_speed") {
        params.start = need_range_num(body, "start", 5);
        params.end   = need_range_num(body, "end", 20);
        params.step  = need_range_num(body, "step", 0.5);
        if (params.start <= 0 || params.step <= 0 || params.end < params.start + params.step)
            zk::fail("bad_value", "need: start > 0, step > 0, end >= start + step");
        params.mode = CalibMode::Calib_Vol_speed_Tower;
        requested = {{"start", params.start}, {"end", params.end}, {"step", params.step}};
    } else if (kind == "pressure_advance") {
        const std::string method = pick(body, "method", {"tower", "line", "pattern"}, "tower");
        const std::string ext    = pick(body, "extruder_type", {"dde", "bowden"}, "dde");
        const bool dde = ext == "dde";
        double def_end  = dde ? (method == "pattern" ? 0.08 : 0.1) : 1.0;
        double def_step = dde ? (method == "pattern" ? 0.005 : 0.002) : (method == "pattern" ? 0.05 : 0.02);
        params.start = need_range_num(body, "start", 0.0);
        params.end   = need_range_num(body, "end", def_end);
        params.step  = need_range_num(body, "step", def_step);
        if (params.start < 0 || params.step < 10 * EPSILON || params.end < params.start + params.step)
            zk::fail("bad_value", "need: start >= 0, step >= 0.001, end >= start + step");
        params.mode = method == "line" ? CalibMode::Calib_PA_Line : method == "pattern" ? CalibMode::Calib_PA_Pattern : CalibMode::Calib_PA_Tower;
        if (belt_printer() && params.mode != CalibMode::Calib_PA_Tower)
            zk::fail("belt_not_supported", "PA line and PA pattern are not enabled on belt printers; use method tower");
        // Tower never numbers, pattern always does, line follows the checkbox (default on).
        params.print_numbers = method == "tower" ? false : method == "pattern" ? true : zk::opt_bool(body, "print_numbers", true);
        if (method == "pattern") {
            params.accelerations = positive_int_list(body, "accelerations");
            params.speeds        = positive_int_list(body, "speeds");
            if (!params.accelerations.empty() && !params.speeds.empty()) {
                const double min_accel = *std::min_element(params.accelerations.begin(), params.accelerations.end());
                const double max_speed = *std::max_element(params.speeds.begin(), params.speeds.end());
                if (min_accel <= max_speed)
                    zk::fail("bad_value", "acceleration values must be greater than speed values");
            }
        }
        requested = {{"method", method}, {"extruder_type", ext}, {"start", params.start}, {"end", params.end}, {"step", params.step},
                     {"print_numbers", params.print_numbers}, {"accelerations", params.accelerations}, {"speeds", params.speeds}};
    } else if (kind == "flow_rate") {
        const std::string type = pick(body, "type", {"pass1", "pass2", "yolo", "yolo_perfectionist"}, "yolo");
        const std::string pat  = pick(body, "pattern", {"archimedean", "monotonic"}, "archimedean");
        flow_linear  = type == "yolo" || type == "yolo_perfectionist";
        flow_pass    = (type == "pass2" || type == "yolo_perfectionist") ? 2 : 1;
        flow_pattern = pat == "monotonic" ? ipMonotonic : ipArchimedeanChords;
        requested = {{"type", type}, {"pattern", pat}};
    } else if (kind == "retraction") {
        params.start = need_range_num(body, "start", 0);
        params.end   = need_range_num(body, "end", 2);
        params.step  = need_range_num(body, "step", 0.1);
        if (params.start < 0 || params.step <= 0 || params.end < params.start + params.step)
            zk::fail("bad_value", "need: start >= 0, step > 0, end >= start + step");
        params.mode = CalibMode::Calib_Retraction_tower;
        requested = {{"start", params.start}, {"end", params.end}, {"step", params.step}};
    } else if (kind == "vfa") {
        params.start = need_range_num(body, "start", 40);
        params.end   = need_range_num(body, "end", 200);
        params.step  = need_range_num(body, "step", 10);
        if (params.start <= 10 || params.step <= 0 || params.end < params.start + params.step)
            zk::fail("bad_value", "need: start > 10, step > 0, end >= start + step");
        params.nozzle_based_resize = zk::opt_bool(body, "auto_scale", true);
        params.vfa_layer_height    = zk::opt_num(body, "layer_height", 0.0);
        if (params.vfa_layer_height < 0 || params.vfa_layer_height > 1.0)
            zk::fail("bad_value", "'layer_height' must be 0 (automatic) or a layer height in mm");
        params.mode = CalibMode::Calib_VFA_Tower;
        // The dialog asks before an end speed beyond the volumetric limit is clamped; over the API, report it.
        {
            const PresetBundle *bundle = wxGetApp().preset_bundle;
            const auto *noz = printer_cfg().option<ConfigOptionFloats>("nozzle_diameter");
            const auto *mvs = bundle->filaments.get_edited_preset().config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
            const auto *lh  = bundle->prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height");
            if (noz && !noz->values.empty() && mvs && !mvs->values.empty() && mvs->values[0] > 0 && noz->values[0] > 0) {
                const double nd = noz->values[0];
                double use_lh = params.vfa_layer_height > 0 ? params.vfa_layer_height : (params.nozzle_based_resize ? nd / 2.0 : (lh && lh->value > 0 ? lh->value : nd / 2.0));
                double width = bundle->prints.get_edited_preset().config.get_abs_value("outer_wall_line_width", nd);
                if (width <= 0)
                    width = nd;
                const double mm3_per_mm = Flow(float(width), float(use_lh), float(nd)).mm3_per_mm();
                if (mm3_per_mm > 0 && params.end > mvs->values[0] / mm3_per_mm)
                    warnings.push_back("end speed " + std::to_string(int(params.end)) + " mm/s exceeds what the filament's max volumetric speed allows (about " +
                                       std::to_string(int(mvs->values[0] / mm3_per_mm)) + " mm/s at this line width and layer height); the upper blocks will be clamped");
            }
        }
        requested = {{"start", params.start}, {"end", params.end}, {"step", params.step}, {"auto_scale", params.nozzle_based_resize}, {"layer_height", params.vfa_layer_height}};
    } else if (kind == "cornering") {
        const bool jd = uses_junction_deviation();
        const double max_end = jd ? 0.3 : 100.0, warn_at = jd ? 0.25 : 20.0;
        params.start = need_range_num(body, "start", jd ? 0.0 : 1.0);
        params.end   = need_range_num(body, "end", jd ? 0.25 : 15.0);
        if (params.start < 0 || params.end > max_end || params.start >= params.end)
            zk::fail("bad_value", std::string("need: 0 <= start < end <= ") + (jd ? "0.3" : "100"));
        if (params.end > warn_at)
            warnings.push_back("high cornering values may cause layer shifts");
        params.mode       = CalibMode::Calib_Cornering;
        params.test_model = pick_model(body, 2);
        requested = {{"start", params.start}, {"end", params.end}, {"test_model", params.test_model}, {"unit", jd ? "mm" : "mm/s"}};
    } else if (kind == "input_shaping_freq") {
        const bool reprap = gcode_flavor() == gcfRepRapFirmware;
        params.freqStartX = need_range_num(body, "freq_start_x", 15);
        params.freqEndX   = need_range_num(body, "freq_end_x", 110);
        params.freqStartY = reprap ? params.freqStartX : need_range_num(body, "freq_start_y", 15);
        params.freqEndY   = reprap ? params.freqEndX : need_range_num(body, "freq_end_y", 110);
        params.start      = need_range_num(body, "damping", 0.15);
        if (params.freqStartX < 0 || params.freqEndX > 500 || params.freqStartX >= params.freqEndX ||
            (!reprap && (params.freqStartY < 0 || params.freqEndY > 500 || params.freqStartY >= params.freqEndY)))
            zk::fail("bad_value", "need: 0 <= freq_start < freq_end <= 500 (X and Y)");
        if (params.start < 0 || params.start >= 1)
            zk::fail("bad_value", "need: 0 <= damping < 1");
        params.shaper_type = resolve_shaper(body);
        params.test_model  = pick_model(body, 1);
        params.mode        = CalibMode::Calib_Input_shaping_freq;
        requested = {{"freq_start_x", params.freqStartX}, {"freq_end_x", params.freqEndX}, {"freq_start_y", params.freqStartY},
                     {"freq_end_y", params.freqEndY}, {"damping", params.start}, {"shaper_type", params.shaper_type}, {"test_model", params.test_model}};
    } else { // input_shaping_damp
        const bool reprap = gcode_flavor() == gcfRepRapFirmware;
        params.freqStartX = need_range_num(body, "freq_x", 30);
        params.freqStartY = reprap ? params.freqStartX : need_range_num(body, "freq_y", 30);
        params.start      = need_range_num(body, "damping_start", 0.0);
        params.end        = need_range_num(body, "damping_end", 0.4);
        if (params.freqStartX < 0 || params.freqStartX > 500 || (!reprap && (params.freqStartY < 0 || params.freqStartY > 500)))
            zk::fail("bad_value", "need: 0 <= freq <= 500");
        if (params.start < 0 || params.end > 1 || params.start >= params.end)
            zk::fail("bad_value", "need: 0 <= damping_start < damping_end <= 1");
        params.shaper_type = resolve_shaper(body);
        params.test_model  = pick_model(body, 1);
        params.mode        = CalibMode::Calib_Input_shaping_damp;
        requested = {{"freq_x", params.freqStartX}, {"freq_y", params.freqStartY}, {"damping_start", params.start}, {"damping_end", params.end},
                     {"shaper_type", params.shaper_type}, {"test_model", params.test_model}};
    }

    // ---- the project is replaced by the calibration project: same guard as POST /project/new ------
    if (project_dirty(p)) {
        if (!zk::opt_bool(body, "discard_changes", false))
            zk::fail("unsaved_changes", "the project has unsaved changes; save it first or pass discard_changes:true");
        p->new_project(true, false);   // no prompt; the calibration then starts from a clean project
        if (project_dirty(p))
            zk::fail("unsaved_changes", "modified presets are still pending; save or discard them in the window first");
    }

    BOOST_LOG_TRIVIAL(info) << "RemoteAPI calibration start: " << kind;
    if (kind == "temperature")                 p->calib_temp(params);
    else if (kind == "max_volumetric_speed")   p->calib_max_vol_speed(params);
    else if (kind == "pressure_advance")       p->calib_pa(params);
    else if (kind == "flow_rate")              p->calib_flowrate(flow_linear, flow_pass, flow_pattern);
    else if (kind == "retraction")             p->calib_retraction(params);
    else if (kind == "vfa")                    p->calib_VFA(params);
    else if (kind == "cornering")              p->Calib_Cornering(params);
    else if (kind == "input_shaping_freq")     p->calib_input_shaping_freq(params);
    else                                       p->calib_input_shaping_damp(params);

    // ---- report what was generated -------------------------------------------------------------
    const Model &model = p->model();
    if (model.objects.empty())
        zk::fail("calibration_failed", "the calibration model could not be loaded (see the log); the project is empty now");

    json objs = json::array();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const ModelObject *mo = model.objects[i];
        const BoundingBoxf3 bb = mo->bounding_box_exact();
        json cfg = json::array();
        for (const std::string &k : mo->config.keys())
            cfg.push_back(k);
        objs.push_back({{"id", (uint64_t) mo->id().id}, {"index", i}, {"name", mo->name},
                        {"size_mm", {bb.size().x(), bb.size().y(), bb.size().z()}}, {"object_settings", cfg}});
    }
    PresetBundle *bundle = wxGetApp().preset_bundle;
    json result = {{"started", kind},
                   {"requested", requested},
                   {"objects", objs},
                   {"plates", p->get_partplate_list().get_plate_count()},
                   {"modified_presets", {{"print", bundle->prints.current_dirty_options(true)},
                                         {"filament", bundle->filaments.current_dirty_options(true)},
                                         {"printer", bundle->printers.current_dirty_options(true)}}},
                   {"next", "POST /slice (or slice_and_wait), then print from the window; the calibration settings are unsaved edits of the selected presets"}};
    if (PartPlate *plate = p->get_partplate_list().get_curr_plate(); plate != nullptr && plate->fff_print() != nullptr)
        result["calibration_mode"] = mode_name(plate->fff_print()->calib_mode());
    if (!warnings.empty())
        result["warnings"] = warnings;
    zk::notify("Calibration project created: " + kind);
    return result;
}

} // namespace

bool Controller::dispatch_calib(const Request &req, Response &out)
{
    static const std::string base = "/api/v1/calibrations";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::string rest = path.substr(base.size());

    if (rest.empty() && req.method == "GET") {
        out = zk::reply(run_on_ui([]() -> json { return zk::guard([&]() -> json { return catalog(); }); }, 30));
        return true;
    }
    if (rest.size() > 1 && rest[0] == '/' && req.method == "POST") {
        const std::vector<std::string> parts = zk::split_path(rest.substr(1));
        if (parts.size() == 2 && parts[1] == "start") {
            json body = json::object();
            if (!zk::parse_body(req, body)) {
                out = {400, {{"error", "invalid_json"}}};
                return true;
            }
            const std::string kind = parts[0];
            out = zk::reply(run_on_ui([kind, body]() -> json { return zk::guard([&]() -> json { return start_calibration(kind, body); }); }, 180));
            return true;
        }
    }
    return false;
}

}}} // namespace Slic3r::GUI::RemoteAPI
