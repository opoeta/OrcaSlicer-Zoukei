// src/slic3r/GUI/RemoteAPI/RemoteAPILayers.cpp
//
// Orca Zoukei Remote API, group G4: per-layer G-code, layer heights and plate settings. Every handler
// runs on the UI thread (run_on_ui), validates its input and takes an undo snapshot where the
// equivalent UI action does.
//
//   GET    /api/v1/layer_gcodes?plate=N           per-layer G-code items of a plate (default: current)
//   POST   /api/v1/layer_gcodes                   {type: color_change|pause|tool_change|custom, height,
//                                                  plate?, extruder?, color?, gcode? (custom), replace?}
//   DELETE /api/v1/layer_gcodes                   {height, plate?, type?} | {all: true, plate?, type?}
//   GET    /api/v1/objects/{id}/layers            height ranges, custom layer-height profile summary
//   PUT    /api/v1/objects/{id}/layer_height/smooth   {radius?: 1..50, keep_min?}  smooth the profile
//                                                  (adaptive / reset: PUT /objects/{id}/layer_height, existing)
//   DELETE /api/v1/objects/{id}/height_range      {min_z, max_z}  remove one range (clear all: PUT height_range)
//   GET    /api/v1/plates/{i}/settings            name, lock, bed type, print sequence, spiral vase, filament map
//   PUT    /api/v1/plates/{i}/settings            {name?, locked?, bed_type?, print_sequence?, spiral_vase?,
//                                                  filament_map_mode?}
//   POST   /api/v1/plates/{i}/move                {to}  reorder plates
//
// "height" is the layer's print Z (top of the layer, mm): the item applies at the first layer whose
// print Z reaches it. Custom G-code text is a "sensitive" edit: it needs Preferences > Remote API >
// "Allow script, G-code and connection edits", like PUT /config does for *_gcode keys.
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

namespace {

const char *gcode_type_name(CustomGCode::Type t)
{
    switch (t) {
    case CustomGCode::ColorChange: return "color_change";
    case CustomGCode::PausePrint:  return "pause";
    case CustomGCode::ToolChange:  return "tool_change";
    case CustomGCode::Template:    return "template";
    case CustomGCode::Custom:      return "custom";
    default:                       return "unknown";
    }
}

bool gcode_type_from_name(const std::string &s, CustomGCode::Type &out)
{
    const std::string n = zk::lower(s);
    if (n == "color_change" || n == "colour_change" || n == "m600") { out = CustomGCode::ColorChange; return true; }
    if (n == "pause" || n == "pause_print")                         { out = CustomGCode::PausePrint;  return true; }
    if (n == "tool_change")                                         { out = CustomGCode::ToolChange;  return true; }
    if (n == "custom")                                              { out = CustomGCode::Custom;      return true; }
    return false;
}

int resolve_plate(const json &src, bool from_query, const std::string &query)
{
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    int plate = plates.get_curr_plate_index();
    if (from_query) {
        if (!query.empty()) {
            uint64_t v = 0;
            if (!zk::parse_uint(query, v))
                zk::fail("bad_plate_index");
            plate = int(v);
        }
    } else if (src.is_object() && src.contains("plate") && !src["plate"].is_null()) {
        plate = int(zk::need_num(src, "plate"));
    }
    if (plate < 0 || plate >= plates.get_plate_count())
        zk::fail("unknown_plate", "plate 0.." + std::to_string(plates.get_plate_count() - 1));
    return plate;
}

json gcode_item_json(const CustomGCode::Item &it)
{
    json o = {{"height", it.print_z}, {"type", gcode_type_name(it.type)}, {"extruder", it.extruder}};
    if (it.type == CustomGCode::ColorChange || it.type == CustomGCode::ToolChange)
        o["color"] = it.color;
    if (it.type == CustomGCode::Custom)
        o["gcode"] = it.extra;
    if (it.type == CustomGCode::PausePrint && !it.extra.empty())
        o["message"] = it.extra;
    return o;
}

json gcodes_json(int plate)
{
    Model &model = wxGetApp().plater()->model();
    json   arr   = json::array();
    auto   it    = model.plates_custom_gcodes.find(plate);
    if (it != model.plates_custom_gcodes.end())
        for (const CustomGCode::Item &item : it->second.gcodes)
            arr.push_back(gcode_item_json(item));
    return {{"plate", plate}, {"items", arr}, {"count", arr.size()}};
}

void invalidate_plate(int plate)
{
    Plater *p = wxGetApp().plater();
    if (PartPlate *pp = p->get_partplate_list().get_plate(plate))
        pp->update_slice_result_valid_state(false);
    p->set_plater_dirty(true);
    p->schedule_background_process();
}

json op_gcode_add(const json &body)
{
    Plater *p = wxGetApp().plater();
    if (!body.is_object())
        zk::fail("body_must_be_object");
    CustomGCode::Type type;
    if (!gcode_type_from_name(zk::need_str(body, "type"), type))
        zk::fail("bad_type", "color_change | pause | tool_change | custom");
    const double height = zk::need_num(body, "height");
    if (!(height > 0.0) || height > 100000.0)
        zk::fail("bad_height", "height is the layer's print Z in mm, > 0");
    const int plate = resolve_plate(body, false, std::string());

    CustomGCode::Item item;
    item.print_z  = height;
    item.type     = type;
    item.extruder = 1;
    const size_t nfil = std::max<size_t>(1, wxGetApp().preset_bundle->filament_presets.size());
    if (type == CustomGCode::ColorChange || type == CustomGCode::ToolChange) {
        const int ext = int(zk::opt_num(body, "extruder", 1));
        if (ext < 1 || size_t(ext) > nfil)
            zk::fail("unknown_filament", "extruder 1.." + std::to_string(nfil));
        item.extruder = ext;
        std::string color = zk::opt_str(body, "color");
        if (color.empty()) {
            if (auto *o = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
                o && size_t(ext - 1) < o->values.size())
                color = o->values[size_t(ext - 1)];
        }
        if (color.size() != 7 || color[0] != '#' ||
            color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
            zk::fail("invalid_colour", "color must be '#RRGGBB'");
        item.color = color;
    } else if (type == CustomGCode::PausePrint) {
        item.extra = zk::opt_str(body, "message");
    } else { // Custom
        item.extra = zk::need_str(body, "gcode");
        if (item.extra.empty() || item.extra.size() > 4000)
            zk::fail("bad_gcode", "1..4000 characters");
        if (!zk::allow_sensitive())
            zk::fail("blocked_by_remote_api_policy",
                     "custom G-code is a sensitive edit: enable Preferences > Remote API > \"Allow script, G-code and connection edits\"");
    }

    p->take_snapshot("Add layer G-code (remote)");
    CustomGCode::Info &info = p->model().plates_custom_gcodes[plate];
    const bool replace = zk::opt_bool(body, "replace", false);
    auto same_layer = [&](const CustomGCode::Item &o) { return std::fabs(o.print_z - height) < 1e-4; };
    auto exists = std::find_if(info.gcodes.begin(), info.gcodes.end(), same_layer);
    if (exists != info.gcodes.end()) {
        if (!replace)
            zk::fail("exists_at_height", "that layer already has an item; pass replace:true to overwrite it");
        info.gcodes.erase(std::remove_if(info.gcodes.begin(), info.gcodes.end(), same_layer), info.gcodes.end());
    }
    info.gcodes.push_back(item);
    std::sort(info.gcodes.begin(), info.gcodes.end());
    invalidate_plate(plate);
    zk::notify(std::string("Added ") + gcode_type_name(type) + " at " + std::to_string(height) + " mm");
    json r = gcodes_json(plate);
    r["added"] = gcode_item_json(item);
    return r;
}

json op_gcode_remove(const json &body)
{
    Plater *p = wxGetApp().plater();
    if (!body.is_object())
        zk::fail("body_must_be_object");
    const int plate = resolve_plate(body, false, std::string());
    const bool all  = zk::opt_bool(body, "all", false);
    const bool has_type = body.contains("type") && !body["type"].is_null();
    CustomGCode::Type type = CustomGCode::Unknown;
    if (has_type && !gcode_type_from_name(zk::need_str(body, "type"), type))
        zk::fail("bad_type", "color_change | pause | tool_change | custom");
    if (!all && !body.contains("height"))
        zk::fail("missing_selector", "give height, or all:true");
    const double height = all ? 0.0 : zk::need_num(body, "height");
    auto found = p->model().plates_custom_gcodes.find(plate);
    if (found == p->model().plates_custom_gcodes.end())
        return {{"plate", plate}, {"removed", 0}, {"items", json::array()}};
    CustomGCode::Info &info = found->second;
    auto matches = [&](const CustomGCode::Item &o) {
        return (all || std::fabs(o.print_z - height) < 1e-3) && (!has_type || o.type == type);
    };
    const size_t before = info.gcodes.size();
    if (std::none_of(info.gcodes.begin(), info.gcodes.end(), matches))
        return {{"plate", plate}, {"removed", 0}, {"items", gcodes_json(plate)["items"]}};
    p->take_snapshot("Remove layer G-code (remote)");
    info.gcodes.erase(std::remove_if(info.gcodes.begin(), info.gcodes.end(), matches), info.gcodes.end());
    const size_t removed = before - info.gcodes.size();
    invalidate_plate(plate);
    zk::notify("Removed " + std::to_string(removed) + " layer G-code item(s)");
    json r = gcodes_json(plate);
    r["removed"] = removed;
    return r;
}

json layers_summary(uint64_t id)
{
    int          idx = -1;
    ModelObject *mo  = zk::need_object(id, idx);
    json ranges = json::array();
    for (const auto &kv : mo->layer_config_ranges) {
        json cfg = json::object();
        for (const std::string &k : kv.second.keys())
            cfg[k] = kv.second.opt_serialize(k);
        ranges.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second},
                          {"layer_height", kv.second.has("layer_height") ? kv.second.opt_float("layer_height") : 0.0},
                          {"config", cfg}});
    }
    json profile = {{"custom", !mo->layer_height_profile.empty()}};
    if (!mo->layer_height_profile.empty()) {
        const std::vector<double> prof = mo->layer_height_profile.get();
        double hmin = 1e9, hmax = 0.0;
        for (size_t i = 1; i < prof.size(); i += 2) {
            hmin = std::min(hmin, prof[i]);
            hmax = std::max(hmax, prof[i]);
        }
        profile["points"] = prof.size() / 2;
        if (prof.size() >= 2) {
            profile["layer_height_min"] = hmin;
            profile["layer_height_max"] = hmax;
            profile["top_z"]            = prof[prof.size() - 2];
        }
    }
    const DynamicPrintConfig &pc = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double base = pc.has("layer_height") ? pc.opt_float("layer_height") : 0.0;
    if (mo->config.has("layer_height"))
        base = mo->config.opt_float("layer_height");
    return {{"id", id}, {"name", mo->name}, {"layer_height", base}, {"height_ranges", ranges}, {"profile", profile}};
}

json op_smooth(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int          idx = -1;
    ModelObject *mo  = zk::need_object(id, idx);
    const double radius = zk::opt_num(body, "radius", 5.0);
    if (radius < 1.0 || radius > 50.0 || radius != std::floor(radius))
        zk::fail("bad_radius", "integer 1..50");
    const bool keep_min = zk::opt_bool(body, "keep_min", false);
    const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
    SlicingParameters sp = PrintObject::slicing_parameters(full_cfg, *mo, 0.f, p->fff_print().shrinkage_compensation());
    std::vector<double> prof = mo->layer_height_profile.get();
    // Fills the default (or height-range based) profile when the object has none yet.
    PrintObject::update_layer_height_profile(*mo, sp, prof);
    prof = smooth_height_profile(prof, sp, HeightProfileSmoothingParams((unsigned int) radius, keep_min));
    if (prof.size() < 4)
        zk::fail("profile_failed", "could not compute a layer height profile");
    p->take_snapshot("Smooth layer heights (remote)");
    mo->layer_height_profile.set(std::move(prof));
    p->schedule_background_process();
    if (ObjectList *ol = wxGetApp().obj_list())
        ol->update_info_items(size_t(idx));
    zk::notify("Smoothed layer heights on '" + mo->name + "'");
    return layers_summary(id);
}

json op_range_remove(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int          idx = -1;
    ModelObject *mo  = zk::need_object(id, idx);
    const double a = zk::need_num(body, "min_z");
    const double b = zk::need_num(body, "max_z");
    auto it = mo->layer_config_ranges.end();
    for (auto i = mo->layer_config_ranges.begin(); i != mo->layer_config_ranges.end(); ++i)
        if (std::fabs(i->first.first - a) < 1e-4 && std::fabs(i->first.second - b) < 1e-4)
            it = i;
    if (it == mo->layer_config_ranges.end())
        zk::fail("unknown_range", "no height range with exactly that min_z / max_z (see GET /objects/{id}/layers)");
    p->take_snapshot("Remove height range (remote)");
    mo->layer_config_ranges.erase(it);
    p->changed_object(idx);
    if (ObjectList *ol = wxGetApp().obj_list())
        ol->update_info_items(size_t(idx));
    zk::notify("Removed a height range on '" + mo->name + "'");
    return layers_summary(id);
}

// ---- plates ---------------------------------------------------------------------------------

const char *bed_type_name(BedType t)
{
    switch (t) {
    case btPC:        return "cool_plate";
    case btEP:        return "eng_plate";
    case btPEI:       return "hot_plate";
    case btPTE:       return "textured_plate";
    case btPCT:       return "textured_cool_plate";
    case btSuperTack: return "supertack_plate";
    default:          return "default";
    }
}

bool bed_type_from_name(const std::string &s, BedType &out)
{
    const std::string n = zk::lower(s);
    for (int i = int(btDefault); i < int(btCount); ++i)
        if (n == bed_type_name(BedType(i))) {
            out = BedType(i);
            return true;
        }
    return false;
}

const char *print_seq_name(PrintSequence s)
{
    switch (s) {
    case PrintSequence::ByLayer:  return "by_layer";
    case PrintSequence::ByObject: return "by_object";
    default:                      return "default";
    }
}

const char *map_mode_name(FilamentMapMode m)
{
    switch (m) {
    case fmmAutoForFlush:  return "auto_for_flush";
    case fmmAutoForMatch:  return "auto_for_match";
    case fmmManual:        return "manual";
    case fmmNozzleManual:  return "nozzle_manual";
    default:               return "default";
    }
}

json plate_settings_json(int index)
{
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    PartPlate *pp = plates.get_plate(index);
    if (pp == nullptr)
        zk::fail("unknown_plate");
    return {{"index", index},
            {"name", pp->get_plate_name()},
            {"locked", pp->is_locked()},
            {"bed_type", bed_type_name(pp->get_bed_type())},
            {"bed_type_effective", bed_type_name(pp->get_bed_type(true))},
            {"print_sequence", print_seq_name(pp->get_print_seq())},
            {"print_sequence_effective", print_seq_name(pp->get_real_print_seq())},
            {"spiral_vase", pp->get_spiral_vase_mode()},
            {"spiral_vase_overridden", pp->has_spiral_mode_config()},
            {"filament_map_mode", map_mode_name(pp->get_filament_map_mode())}};
}

json op_plate_set(int index, const json &body)
{
    Plater *p = wxGetApp().plater();
    if (!body.is_object())
        zk::fail("body_must_be_object");
    PartPlateList &plates = p->get_partplate_list();
    PartPlate *pp = plates.get_plate(index);
    if (pp == nullptr)
        zk::fail("unknown_plate");
    PresetBundle &pb = *wxGetApp().preset_bundle;

    // Validate everything first, then apply.
    bool        set_name = false, set_lock = false, set_bed = false, set_seq = false, set_spiral = false, set_map = false;
    std::string name;
    bool        lock = false;
    BedType     bed  = btDefault;
    PrintSequence seq = PrintSequence::ByDefault;
    FilamentMapMode map_mode = fmmDefault;
    if (body.contains("name")) {
        name = zk::need_str(body, "name");
        if (name.size() > 100)
            zk::fail("bad_name", "at most 100 characters");
        set_name = true;
    }
    if (body.contains("locked")) {
        lock = zk::opt_bool(body, "locked", false);
        set_lock = true;
    }
    if (body.contains("bed_type")) {
        if (!bed_type_from_name(zk::need_str(body, "bed_type"), bed)) {
            std::string names;
            for (int i = int(btDefault); i < int(btCount); ++i)
                names += std::string(i ? " | " : "") + bed_type_name(BedType(i));
            zk::fail("bad_bed_type", names);
        }
        const bool multi = pb.is_bbl_vendor() || pb.printers.get_edited_preset().config.opt_bool("support_multi_bed_types");
        if (bed != btDefault && !multi)
            zk::fail("bed_types_unsupported", "the selected printer does not support per-plate bed types");
        set_bed = true;
    }
    if (body.contains("print_sequence")) {
        const std::string s = zk::lower(zk::need_str(body, "print_sequence"));
        if (s == "by_layer") seq = PrintSequence::ByLayer;
        else if (s == "by_object") seq = PrintSequence::ByObject;
        else if (s == "default") seq = PrintSequence::ByDefault;
        else zk::fail("bad_print_sequence", "by_layer | by_object | default");
        set_seq = true;
    }
    bool spiral_global = false, spiral_off = false;
    if (body.contains("spiral_vase")) {
        const json &v = body["spiral_vase"];
        if (v.is_string() && zk::lower(v.get<std::string>()) == "global") spiral_global = true;
        else if (v.is_boolean() && !v.get<bool>()) spiral_off = true;
        else zk::fail("bad_spiral_vase", "\"global\" (follow the print preset) or false; enabling it needs the confirmation dialog in the window");
        set_spiral = true;
    }
    if (body.contains("filament_map_mode")) {
        const std::string s = zk::lower(zk::need_str(body, "filament_map_mode"));
        if (pb.get_printer_extruder_count() <= 1)
            zk::fail("not_multi_extruder", "the selected printer has one extruder");
        if (s == "default") map_mode = fmmDefault;
        else if (s == "auto_for_flush") map_mode = fmmAutoForFlush;
        else if (s == "auto_for_match") map_mode = fmmAutoForMatch;
        else zk::fail("bad_filament_map_mode", "default | auto_for_flush | auto_for_match (manual maps are set in the window)");
        set_map = true;
    }
    if (!(set_name || set_lock || set_bed || set_seq || set_spiral || set_map))
        zk::fail("nothing_to_set", "name | locked | bed_type | print_sequence | spiral_vase | filament_map_mode");

    p->take_snapshot("Edit plate settings (remote)");
    if (set_name) pp->set_plate_name(name);
    if (set_lock) plates.lock_plate(index, lock);
    if (set_bed) pp->set_bed_type(bed);
    if (set_seq) pp->set_print_seq(seq);
    if (set_spiral) pp->set_spiral_vase_mode(false, spiral_global && !spiral_off);
    if (set_map) pp->set_filament_map_mode(map_mode);
    p->set_plater_dirty(true);
    p->update();
    zk::notify("Updated settings of plate " + std::to_string(index + 1));
    return plate_settings_json(index);
}

json op_plate_move(int index, const json &body)
{
    Plater *p = wxGetApp().plater();
    PartPlateList &plates = p->get_partplate_list();
    if (!body.is_object() || !body.contains("to"))
        zk::fail("missing_field", "to: target plate index");
    const int to = int(zk::need_num(body, "to"));
    const int n  = plates.get_plate_count();
    if (index < 0 || index >= n || to < 0 || to >= n)
        zk::fail("unknown_plate", "plate 0.." + std::to_string(n - 1));
    if (index == to)
        return {{"moved", false}, {"detail", "already there"}, {"plates", n}};
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for or cancel the running slice first");
    if (to == 0) {
        // The "move to front" button of the plate (action 7 in Plater::select_plate_by_hover_id).
        p->select_plate_by_hover_id(index * int(PartPlate::GRABBER_COUNT) + 7, false);
    } else {
        p->take_snapshot("move plate (remote)");
        if (plates.move_plate_to_index(index, to) != 0)
            zk::fail("move_failed");
        plates.update_plates();
        if (ObjectList *ol = wxGetApp().obj_list())
            ol->reload_all_plates();
        p->update();
        p->select_plate(to);
    }
    p->set_plater_dirty(true);
    json arr = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        arr.push_back({{"index", i}, {"name", plates.get_plate(i)->get_plate_name()}});
    return {{"moved", true}, {"from", index}, {"to", to}, {"plates", arr}, {"current", plates.get_curr_plate_index()}};
}

} // namespace

bool Controller::dispatch_layers(const Request &req, Response &out)
{
    static const std::string base = "/api/v1/";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::vector<std::string> parts = zk::split_path(path.substr(base.size()));
    if (parts.empty())
        return false;
    const std::string &m = req.method;
    auto bad_json = [&]() { out = {400, {{"error", "invalid_json"}}}; return true; };

    // ---- layer_gcodes -----------------------------------------------------------------------
    if (parts[0] == "layer_gcodes" && parts.size() == 1) {
        if (m == "GET") {
            const std::string q = zk::query_value(req, "plate");
            out = zk::reply(run_on_ui([q]() -> json {
                return zk::guard([&]() -> json { return gcodes_json(resolve_plate(json(), true, q)); });
            }, 30));
            return true;
        }
        json body;
        if ((m == "POST" || m == "DELETE") && !zk::parse_body(req, body))
            return bad_json();
        if (m == "POST") {
            out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_gcode_add(body); }); }, 30));
            if (out.status == 200) out.status = 201;
            return true;
        }
        if (m == "DELETE") {
            out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_gcode_remove(body); }); }, 30));
            return true;
        }
        return false;
    }

    // ---- object layers ----------------------------------------------------------------------
    if (parts[0] == "objects" && parts.size() >= 3) {
        uint64_t id = 0;
        if (!zk::parse_uint(parts[1], id))
            return false;
        const std::string &act = parts[2];
        if (m == "GET" && act == "layers" && parts.size() == 3) {
            out = zk::reply(run_on_ui([id]() -> json { return zk::guard([&]() -> json { return layers_summary(id); }); }, 30));
            return true;
        }
        if (m == "PUT" && act == "layer_height" && parts.size() == 4 && parts[3] == "smooth") {
            json body;
            if (!zk::parse_body(req, body))
                return bad_json();
            out = zk::reply(run_on_ui([id, body]() -> json { return zk::guard([&]() -> json { return op_smooth(id, body); }); }, 60));
            return true;
        }
        if (m == "DELETE" && act == "height_range" && parts.size() == 3) {
            json body;
            if (!zk::parse_body(req, body))
                return bad_json();
            out = zk::reply(run_on_ui([id, body]() -> json { return zk::guard([&]() -> json { return op_range_remove(id, body); }); }, 30));
            return true;
        }
        return false;
    }

    // ---- plates -----------------------------------------------------------------------------
    if (parts[0] == "plates" && parts.size() == 3) {
        uint64_t idx64 = 0;
        if (!zk::parse_uint(parts[1], idx64))
            return false;
        const int index = int(idx64);
        if (parts[2] == "settings" && m == "GET") {
            out = zk::reply(run_on_ui([index]() -> json { return zk::guard([&]() -> json { return plate_settings_json(index); }); }, 30));
            return true;
        }
        if (parts[2] == "settings" && m == "PUT") {
            json body;
            if (!zk::parse_body(req, body))
                return bad_json();
            out = zk::reply(run_on_ui([index, body]() -> json { return zk::guard([&]() -> json { return op_plate_set(index, body); }); }, 30));
            return true;
        }
        if (parts[2] == "move" && m == "POST") {
            json body;
            if (!zk::parse_body(req, body))
                return bad_json();
            out = zk::reply(run_on_ui([index, body]() -> json { return zk::guard([&]() -> json { return op_plate_move(index, body); }); }, 60));
            return true;
        }
    }
    return false;
}

}}} // namespace
