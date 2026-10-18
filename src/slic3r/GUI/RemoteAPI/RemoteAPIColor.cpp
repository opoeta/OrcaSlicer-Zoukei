// src/slic3r/GUI/RemoteAPI/RemoteAPIColor.cpp
//
// Orca Zoukei: Remote API for the colour features (filaments, mixed filaments, the colour mixing
// engine, coloured-model import). Every handler runs on the UI thread (run_on_ui), validates its input
// and takes an undo snapshot where the equivalent UI action does.
//
//   GET    /api/v1/filaments                    project filament slots (colour, multi colour, TD, mix recipe)
//   PUT    /api/v1/filaments/{slot}             preset / colour / multi_colour / td of a slot (slots are 1-based)
//   GET    /api/v1/mixed_filaments              mixed slots with recipe, pattern, predicted colour
//   POST   /api/v1/mixed_filaments              add (components + ratios | pattern) - "Add Mixed Filament"
//   PUT    /api/v1/mixed_filaments/{slot}       change components / ratios / gradient
//   DELETE /api/v1/mixed_filaments/{slot}       remove
//   POST   /api/v1/color_mix/solve              recipes for target colours, project untouched
//   POST   /api/v1/color_mix/apply              apply a solve result (mixes + repaint / remap)
//   GET    /api/v1/model/colors                 colours in use by the model + the pending colour import
//   POST   /api/v1/model/import_colored         load a coloured OBJ/3MF/GLB without the dialog, return its palette
//   PUT    /api/v1/objects/{id}/filament        per-object / per-volume filament
#include "RemoteAPIController.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/ColorMixEngine.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ColorMixDialog.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PostProcessor.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

namespace {

constexpr const char *kSection = "color_mix"; // same AppConfig section as ColorMixDialog

std::string key_safe(std::string s)
{
    for (char &c : s)
        if (!std::isalnum((unsigned char) c))
            c = '_';
    return s;
}

int hex_alpha(const std::string &hex)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 8)
        return 255;
    try {
        return std::stoi(s.substr(6, 2), nullptr, 16);
    } catch (...) {
        return 255;
    }
}

bool valid_hex(const std::string &hex)
{
    ColorMix::RGB c;
    return ColorMix::rgb_from_hex(hex, c);
}

std::string norm_hex(const std::string &hex)
{
    ColorMix::RGB c;
    return ColorMix::rgb_from_hex(hex, c) ? ColorMix::rgb_to_hex(c) : std::string();
}

std::string hex_u8(const std::array<size_t, 3> &c)
{
    char b[8];
    std::snprintf(b, sizeof b, "#%02X%02X%02X", int(c[0]), int(c[1]), int(c[2]));
    return b;
}

Response reply(const json &r)
{
    if (r.is_object() && r.contains("error")) {
        const std::string e = r["error"].get<std::string>();
        if (e.rfind("unknown_", 0) == 0 || e == "not_found")
            return {404, r};
        if (e == "slicing")
            return {409, r};
        return {422, r};
    }
    return {200, r};
}

PresetBundle &bundle() { return *wxGetApp().preset_bundle; }
Plater       *plater() { return wxGetApp().plater(); }

bool slot_is_mixed(size_t i)
{
    auto *o = bundle().project_config.option<ConfigOptionBools>("filament_is_mixed");
    return o && i < o->values.size() && o->values[i];
}

std::string str_at(const char *key, size_t i)
{
    auto *o = bundle().project_config.option<ConfigOptionStrings>(key);
    return o && i < o->values.size() ? o->values[i] : std::string();
}

double saved_td(const std::string &hex, const std::string &name)
{
    const std::string v = wxGetApp().app_config->get(kSection, "td_" + key_safe(hex) + "_" + key_safe(name));
    return v.empty() ? 0.0 : std::max(0.0, std::atof(v.c_str()));
}

// Physical filaments as the engine sees them. `slot[k]` is the 0-based project slot of physical filament k.
struct PhysInfo {
    std::vector<std::string> color, name, type;
    std::vector<size_t>      slot;
};

PhysInfo physical_info()
{
    PhysInfo p;
    plater()->sidebar().collect_physical_filament_info(p.color, p.name, p.type, &p.slot);
    return p;
}

int phys_index_of_slot(const PhysInfo &p, size_t slot)
{
    for (size_t k = 0; k < p.slot.size(); ++k)
        if (p.slot[k] == slot)
            return int(k);
    return -1;
}

json recipe_json(const std::vector<unsigned> &comps, const std::vector<double> &ratios)
{
    json c = json::array(), r = json::array();
    for (unsigned v : comps)
        c.push_back(v);
    for (double v : ratios)
        r.push_back(int(std::lround(v * 100.0)));
    return {{"components", c}, {"percentages", r}};
}

// Predicted colour of a slot recipe with the engine (pigment model), components are 1-based physical numbers.
std::string predict_hex(const PhysInfo &p, const std::vector<unsigned> &comps, const std::vector<int> &pct)
{
    std::vector<std::string> hexes;
    for (unsigned c : comps)
        hexes.push_back(c >= 1 && c <= p.color.size() ? p.color[c - 1] : "#808080");
    return blend_color_multi(hexes, pct);
}

json slot_json(const PhysInfo &p, size_t i)
{
    PresetBundle &pb  = bundle();
    json          s   = {{"slot", i + 1}, {"mixed", slot_is_mixed(i)}};
    s["colour"]       = str_at("filament_colour", i);
    s["multi_colour"] = str_at("filament_multi_colour", i);
    s["preset"]       = i < pb.filament_presets.size() ? pb.filament_presets[i] : std::string();
    if (slot_is_mixed(i)) {
        const auto comps  = parse_mixed_components(str_at("filament_mixed_components", i));
        const auto ratios = parse_mixed_ratios(str_at("filament_mixed_sublayer_ratios", i), comps.size());
        s.update(recipe_json(comps, ratios));
        std::vector<int> pct;
        for (double r : ratios)
            pct.push_back(int(std::lround(r * 100.0)));
        s["predicted_colour"] = predict_hex(p, comps, pct);
        auto *g               = pb.project_config.option<ConfigOptionBools>("filament_mixed_gradient");
        s["gradient"]         = g && i < g->values.size() && g->values[i];
    } else {
        const int k = phys_index_of_slot(p, i);
        if (k >= 0) {
            s["name"] = p.name[size_t(k)];
            s["type"] = p.type[size_t(k)];
            s["td_mm"] = saved_td(p.color[size_t(k)], p.name[size_t(k)]);
        }
    }
    return s;
}

// ------------------------------------------------------------------------------------------------
// colour mixing solve records (UI thread only)

struct SolveRecord {
    int                  id{0};
    std::string          source; // "pending" | "model" | "colors"
    ColorMix::Solution   solution;
    ColorMix::SolverOptions options;
    std::vector<size_t>  palette_slot;  // palette index -> project slot (0-based)
    std::vector<size_t>  target_slot;   // "model": target t -> original slot (0-based)
    std::vector<std::string> target_hex;
    std::string          method; // "layers" | "sublayers"
    std::vector<unsigned> phys_numbers; // 1-based physical number of palette index
};

std::map<int, SolveRecord> g_records;
int                        g_next_record = 1;

json choice_json(const SolveRecord &rec, const ColorMix::Candidate &cand, const PhysInfo &p)
{
    json c;
    if (cand.recipe.is_physical()) {
        c["type"]     = "physical";
        c["filament"] = rec.palette_slot[size_t(cand.recipe.components[0])] + 1;
    } else {
        c["type"] = "mix";
        json comps = json::array(), pct = json::array();
        for (int k : cand.recipe.components)
            comps.push_back(rec.palette_slot[size_t(k)] + 1);
        for (int v : cand.recipe.percentages())
            pct.push_back(v);
        c["components"]  = comps;
        c["percentages"] = pct;
        c["pattern"]     = cand.recipe.pattern();
        c["existing"]    = cand.existing;
    }
    (void) p;
    return c;
}

ColorMix::SolverOptions read_options(const json &in, size_t palette_size, const std::vector<ColorMix::RGB> &, bool *only_physical_out)
{
    AppConfig *cfg = wxGetApp().app_config;
    auto get_int = [cfg](const char *key, int def) {
        const std::string v = cfg->get(kSection, key);
        return v.empty() ? def : std::atoi(v.c_str());
    };
    ColorMix::SolverOptions opt;
    opt.max_colors     = in.value("max_colors", get_int("max_colors", 8));
    opt.max_mixes      = std::clamp(in.value("max_mixes", get_int("max_mixes", 4)), 0, 16);
    opt.pattern_length = std::clamp(in.value("step", get_int("pattern_length", 4)), 2, 12);
    opt.max_components = in.value("allow_triples", get_int("triples", 0) != 0) ? 3 : 2;
    opt.only_physical  = in.value("only_physical", get_int("only_physical", 0) != 0) || palette_size < 2;
    if (only_physical_out)
        *only_physical_out = opt.only_physical;
    const std::string model = in.value("prediction_model", cfg->get(kSection, "model"));
    opt.model.model         = ColorMix::model_from_key(model);
    ColorMix::Method m      = ColorMix::method_from_key(in.value("method", cfg->get(kSection, "method")));
    if (m == ColorMix::Method::Auto)
        m = ColorMix::choose_method(ColorMixDialog::current_printer());
    opt.model.method = m;
    if (const auto *lh = bundle().prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height"))
        opt.model.layer_height = lh->value;
    if (opt.model.model == ColorMix::Model::Pigment)
        opt.model.measured = [](const std::vector<ColorMix::RGB> &colors, const std::vector<int> &weights, ColorMix::RGB &out) {
            std::vector<std::string> hexes;
            for (const ColorMix::RGB &c : colors)
                hexes.push_back(ColorMix::rgb_to_hex(c));
            const std::string hex = lookup_measured_blend_color(hexes, weights);
            return !hex.empty() && ColorMix::rgb_from_hex(hex, out);
        };
    return opt;
}

json do_solve(const json &in)
{
    Plater *pl = plater();
    if (pl == nullptr)
        return {{"error", "no_plater"}};
    const PhysInfo phys = physical_info();
    if (phys.slot.size() < 2)
        return {{"error", "need_two_filaments"}, {"detail", "the project needs at least two physical filaments"}};

    // Loaded filaments (the palette): 1-based project slots, default = the first N.
    std::vector<size_t> loaded;
    if (in.contains("loaded") && in["loaded"].is_array()) {
        for (const json &v : in["loaded"]) {
            if (!v.is_number_integer())
                return {{"error", "invalid_loaded"}};
            const long n = v.get<long>();
            if (n < 1 || phys_index_of_slot(phys, size_t(n - 1)) < 0)
                return {{"error", "invalid_loaded"}, {"detail", "filament " + std::to_string(n) + " is not a physical project filament"}};
            loaded.push_back(size_t(n - 1));
        }
    } else {
        const ColorMix::PrinterKind pk = ColorMixDialog::current_printer();
        const size_t n = pk.nozzles > 1 && !pk.single_extruder_mm ? size_t(pk.nozzles) : 4;
        for (size_t k = 0; k < phys.slot.size() && loaded.size() < n; ++k)
            loaded.push_back(phys.slot[k]);
    }
    if (loaded.empty())
        return {{"error", "invalid_loaded"}};
    std::sort(loaded.begin(), loaded.end());
    loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());

    // Targets.
    SolveRecord rec;
    rec.id = g_next_record;
    std::vector<ColorMix::RGB> colors;
    std::vector<double>        weights;
    json                       tspec = in.value("targets", json("pending"));
    if (tspec.is_string() && tspec.get<std::string>() == "pending") {
        const Plater::ColorImportInfo *pend = pl->remote_pending_color_import();
        if (pend == nullptr)
            return {{"error", "no_pending_import"}, {"detail", "run model/import_colored first (or pass targets as colours)"}};
        rec.source = "pending";
        for (size_t i = 0; i < pend->cluster_colors.size(); ++i) {
            ColorMix::RGB c;
            ColorMix::rgb_from_hex(hex_u8(pend->cluster_colors[i]), c);
            colors.push_back(c);
            weights.push_back(std::max(1e-6, pend->cluster_share[i]));
            rec.target_hex.push_back(hex_u8(pend->cluster_colors[i]));
        }
    } else if (tspec.is_string() && tspec.get<std::string>() == "model") {
        rec.source = "model";
        std::set<size_t> used;
        for (const ModelObject *o : pl->model().objects)
            for (const ModelVolume *v : o->volumes)
                for (int e : v->get_extruders())
                    if (e >= 1)
                        used.insert(size_t(e - 1));
        const bool used_only = in.value("used_only", true);
        for (size_t k = 0; k < phys.slot.size(); ++k) {
            const size_t slot = phys.slot[k];
            if (std::find(loaded.begin(), loaded.end(), slot) != loaded.end())
                continue;
            if (used_only && !used.count(slot))
                continue;
            ColorMix::RGB c;
            if (!ColorMix::rgb_from_hex(phys.color[k], c))
                continue;
            colors.push_back(c);
            weights.push_back(1.0);
            rec.target_slot.push_back(slot);
            rec.target_hex.push_back(norm_hex(phys.color[k]));
        }
        if (colors.empty())
            return {{"error", "nothing_to_reproduce"}, {"detail", "every filament the model uses is already loaded"}};
    } else if (tspec.is_array()) {
        rec.source = "colors";
        for (const json &t : tspec) {
            std::string hex = t.is_string() ? t.get<std::string>() : t.value("hex", std::string());
            ColorMix::RGB c;
            if (!ColorMix::rgb_from_hex(hex, c))
                return {{"error", "invalid_target"}, {"detail", "bad colour '" + hex + "'"}};
            colors.push_back(c);
            weights.push_back(t.is_object() ? std::max(1e-6, t.value("weight", 1.0)) : 1.0);
            rec.target_hex.push_back(ColorMix::rgb_to_hex(c));
        }
        if (colors.empty())
            return {{"error", "invalid_target"}, {"detail", "targets is empty"}};
    } else {
        return {{"error", "invalid_targets"}, {"detail", "targets: \"pending\" | \"model\" | [\"#RRGGBB\" | {hex, weight}]"}};
    }

    // Palette.
    std::vector<ColorMix::Filament> palette;
    json td_in = in.value("td", json::object());
    for (size_t slot : loaded) {
        const int          k = phys_index_of_slot(phys, slot);
        ColorMix::Filament f;
        ColorMix::rgb_from_hex(phys.color[size_t(k)], f.color);
        f.td_mm = saved_td(phys.color[size_t(k)], phys.name[size_t(k)]);
        if (td_in.is_object() && td_in.contains(std::to_string(slot + 1)))
            f.td_mm = std::max(0.0, td_in[std::to_string(slot + 1)].get<double>());
        f.opacity = ColorMix::default_opacity(phys.type[size_t(k)], phys.name[size_t(k)], hex_alpha(phys.color[size_t(k)]));
        f.name    = phys.name[size_t(k)];
        f.type    = phys.type[size_t(k)];
        palette.push_back(f);
        rec.palette_slot.push_back(slot);
        rec.phys_numbers.push_back(unsigned(k + 1));
    }

    json opts_in = in;
    if (rec.source == "model")
        opts_in["max_colors"] = 0;
    rec.options = read_options(opts_in, palette.size(), colors, nullptr);
    rec.method  = rec.options.model.method == ColorMix::Method::SubLayers ? "sublayers" : "layers";

    // Existing fixed mixes of the project whose components are all loaded are free to reuse.
    PresetBundle &pb = bundle();
    auto *grad       = pb.project_config.option<ConfigOptionBools>("filament_mixed_gradient");
    for (size_t i = 0; i < pb.filament_presets.size(); ++i) {
        if (!slot_is_mixed(i) || (grad && i < grad->values.size() && grad->values[i]))
            continue;
        const auto comps  = parse_mixed_components(str_at("filament_mixed_components", i));
        const auto ratios = parse_mixed_ratios(str_at("filament_mixed_sublayer_ratios", i), comps.size());
        ColorMix::Recipe r;
        for (size_t k = 0; k < comps.size(); ++k) {
            const int pk  = comps[k] >= 1 && comps[k] <= phys.slot.size() ? int(phys.slot[comps[k] - 1]) : -1;
            int       pal = -1;
            for (size_t q = 0; q < loaded.size(); ++q)
                if (int(loaded[q]) == pk)
                    pal = int(q);
            if (pal < 0) {
                r.components.clear();
                break;
            }
            r.components.push_back(pal);
            r.weights.push_back(int(std::lround(ratios[k] * 100.0)));
        }
        if (r.components.size() >= 2)
            rec.options.existing_mixes.push_back(r);
    }

    rec.solution = ColorMix::solve(colors, weights, palette, rec.options);

    json out = {{"solve_id", rec.id}, {"source", rec.source}, {"method", rec.method},
                {"prediction_model", ColorMix::model_key(rec.options.model.model)},
                {"loaded", json::array()}};
    for (size_t s : loaded)
        out["loaded"].push_back(s + 1);
    const int free_slots = int(EnforcerBlockerType::ExtruderMax) - int(pb.filament_presets.size());
    out["free_slots"]    = free_slots;
    json clusters        = json::array();
    for (size_t c = 0; c < rec.solution.clusters.size(); ++c) {
        const ColorMix::Assignment &a = rec.solution.clusters[c];
        json j = {{"cluster", c}, {"target", ColorMix::rgb_to_hex(a.target)}, {"weight", a.weight}, {"delta_e", a.delta_e}};
        if (a.candidate >= 0) {
            const ColorMix::Candidate &cand = rec.solution.candidates[size_t(a.candidate)];
            j["choice"]    = choice_json(rec, cand, phys);
            j["predicted"] = ColorMix::rgb_to_hex(cand.predicted);
        }
        clusters.push_back(j);
    }
    out["clusters"] = clusters;
    json assign     = json::array();
    for (size_t t = 0; t < rec.solution.source_cluster.size(); ++t) {
        json a = {{"target", rec.target_hex[t]}, {"cluster", rec.solution.source_cluster[t]}};
        if (rec.source == "model")
            a["from_filament"] = rec.target_slot[t] + 1;
        assign.push_back(a);
    }
    out["assignments"] = assign;
    json mixes         = json::array();
    for (int ci : rec.solution.new_mixes)
        mixes.push_back(choice_json(rec, rec.solution.candidates[size_t(ci)], phys));
    out["new_mixes"]             = mixes;
    out["mean_delta_e"]          = rec.solution.mean_delta_e;
    out["max_delta_e"]           = rec.solution.max_delta_e;
    out["physical_mean_delta_e"] = rec.solution.physical_mean_delta_e;

    ++g_next_record;
    g_records[rec.id] = std::move(rec);
    while (g_records.size() > 8)
        g_records.erase(g_records.begin());
    return out;
}

json do_apply(const json &in)
{
    Plater *pl = plater();
    if (!in.contains("solve_id") || !in["solve_id"].is_number_integer())
        return {{"error", "missing_solve_id"}};
    auto it = g_records.find(in["solve_id"].get<int>());
    if (it == g_records.end())
        return {{"error", "unknown_solve_id"}, {"detail", "the solve result expired; run color_mix/solve again"}};
    SolveRecord &rec = it->second;
    const PhysInfo phys = physical_info();
    Sidebar &sb = pl->sidebar();

    // Chosen recipe per cluster: the solver's candidate unless overridden.
    struct Pick { std::vector<size_t> slots; std::vector<int> pct; };
    std::vector<Pick> picks(rec.solution.clusters.size());
    for (size_t c = 0; c < picks.size(); ++c) {
        const ColorMix::Assignment &a = rec.solution.clusters[c];
        if (a.candidate < 0)
            continue;
        const ColorMix::Candidate &cand = rec.solution.candidates[size_t(a.candidate)];
        for (int k : cand.recipe.components)
            picks[c].slots.push_back(rec.palette_slot[size_t(k)]);
        picks[c].pct = cand.recipe.is_physical() ? std::vector<int>{100} : cand.recipe.percentages();
    }
    if (in.contains("overrides") && in["overrides"].is_object()) {
        for (auto kv = in["overrides"].begin(); kv != in["overrides"].end(); ++kv) {
            size_t c = 0;
            try { c = size_t(std::stoul(kv.key())); } catch (...) { return {{"error", "invalid_override"}}; }
            if (c >= picks.size())
                return {{"error", "invalid_override"}, {"detail", "cluster " + kv.key() + " does not exist"}};
            const json &o = kv.value();
            Pick p;
            if (o.contains("filament")) {
                const long n = o["filament"].get<long>();
                if (n < 1 || size_t(n) > bundle().filament_presets.size())
                    return {{"error", "invalid_override"}, {"detail", "filament out of range"}};
                p.slots = {size_t(n - 1)};
                p.pct   = {100};
            } else if (o.contains("components") && o.contains("percentages")) {
                for (const json &v : o["components"]) {
                    const long n = v.get<long>();
                    if (phys_index_of_slot(phys, size_t(std::max(0L, n - 1))) < 0)
                        return {{"error", "invalid_override"}, {"detail", "components must be physical filaments"}};
                    p.slots.push_back(size_t(n - 1));
                }
                for (const json &v : o["percentages"])
                    p.pct.push_back(v.get<int>());
                if (p.slots.size() < 2 || p.slots.size() != p.pct.size())
                    return {{"error", "invalid_override"}};
            } else {
                return {{"error", "invalid_override"}};
            }
            picks[c] = p;
        }
    }

    pl->take_snapshot(_u8L("Apply colour mixing"));
    json mixes = json::array(), warnings = json::array();
    std::vector<int> cluster_slot(picks.size(), -1);
    bool any_mix = false;
    for (size_t c = 0; c < picks.size(); ++c) {
        const Pick &p = picks[c];
        if (p.slots.empty())
            continue;
        if (p.slots.size() == 1) {
            cluster_slot[c] = int(p.slots[0]);
            continue;
        }
        std::vector<unsigned> comps;
        for (size_t s : p.slots) {
            const int k = phys_index_of_slot(phys, s);
            if (k < 0)
                break;
            comps.push_back(unsigned(k + 1));
        }
        int slot = comps.size() == p.slots.size() ? sb.ensure_mixed_filament(comps, p.pct) : -1;
        if (slot < 0) {
            // No room for another slot: fall back to the dominant component.
            size_t best = 0;
            for (size_t k = 1; k < p.pct.size(); ++k)
                if (p.pct[k] > p.pct[best])
                    best = k;
            slot = int(p.slots[best]);
            warnings.push_back("cluster " + std::to_string(c) + ": could not create the mix (slot limit); used filament " + std::to_string(slot + 1));
        } else {
            any_mix = true;
            json m = recipe_json(comps, std::vector<double>(p.pct.begin(), p.pct.end()));
            m["slot"] = slot + 1;
            mixes.push_back(m);
        }
        cluster_slot[c] = slot;
    }
    if (any_mix)
        ColorMixDialog::apply_method(rec.method == "sublayers" ? ColorMix::Method::SubLayers : ColorMix::Method::AlternatingLayers);

    json out = {{"applied", true}, {"mixes", mixes}, {"warnings", warnings}};
    if (rec.source == "pending") {
        std::vector<int> per_target(rec.solution.source_cluster.size(), -1);
        for (size_t t = 0; t < per_target.size(); ++t) {
            const int c = rec.solution.source_cluster[t];
            if (c >= 0 && size_t(c) < cluster_slot.size())
                per_target[t] = cluster_slot[size_t(c)];
        }
        std::string err;
        if (!pl->remote_apply_color_import(per_target, err))
            return {{"error", "apply_failed"}, {"detail", err}};
        json asg = json::array();
        for (size_t t = 0; t < per_target.size(); ++t)
            asg.push_back({{"target", rec.target_hex[t]}, {"filament", per_target[t] + 1}});
        out["assignments"] = asg;
    } else if (rec.source == "model") {
        std::map<int, int> relocations;
        json asg = json::array();
        for (size_t t = 0; t < rec.target_slot.size() && t < rec.solution.source_cluster.size(); ++t) {
            const int c  = rec.solution.source_cluster[t];
            const int to = c >= 0 && size_t(c) < cluster_slot.size() ? cluster_slot[size_t(c)] : -1;
            if (to >= 0 && to != int(rec.target_slot[t]))
                relocations[int(rec.target_slot[t])] = to;
            asg.push_back({{"from_filament", rec.target_slot[t] + 1}, {"to_filament", to + 1}});
        }
        out["assignments"] = asg;
        if (!relocations.empty()) {
            Model &model = pl->model();
            remap_model_filament_slots(model, relocations);
            for (ModelObject *object : model.objects)
                for (auto &range : object->layer_config_ranges) {
                    ModelConfig &cfg = range.second;
                    if (cfg.has("extruder")) {
                        auto r = relocations.find(cfg.extruder() - 1);
                        if (r != relocations.end())
                            cfg.set("extruder", r->second + 1);
                    }
                }
            std::vector<size_t> all(model.objects.size());
            std::iota(all.begin(), all.end(), 0);
            pl->changed_objects(all);
            if (GLCanvas3D *canvas = pl->get_view3D_canvas3D())
                canvas->reload_scene(true, true);
            pl->update();
        }
    } else {
        json asg = json::array();
        for (size_t t = 0; t < rec.solution.source_cluster.size(); ++t) {
            const int c = rec.solution.source_cluster[t];
            asg.push_back({{"target", rec.target_hex[t]}, {"filament", (c >= 0 && size_t(c) < cluster_slot.size() ? cluster_slot[size_t(c)] : -1) + 1}});
        }
        out["assignments"] = asg;
    }
    g_records.erase(it);
    return out;
}

// ------------------------------------------------------------------------------------------------

bool parse_recipe(const json &in, const PhysInfo &phys, std::vector<unsigned> &comps, std::vector<int> &pct, std::string &err)
{
    if (!in.contains("components") || !in["components"].is_array() || in["components"].size() < 2) {
        err = "components: at least two physical filament numbers (1-based)";
        return false;
    }
    comps.clear();
    for (const json &v : in["components"]) {
        if (!v.is_number_integer()) { err = "components must be integers"; return false; }
        const long n = v.get<long>();
        const int  k = n >= 1 ? phys_index_of_slot(phys, size_t(n - 1)) : -1;
        if (k < 0) { err = "filament " + std::to_string(n) + " is not a physical project filament"; return false; }
        comps.push_back(unsigned(k + 1));
    }
    pct.clear();
    if (in.contains("pattern") && in["pattern"].is_string()) {
        // Letters A, B, C ... name the components in order: "AAB" = 2:1.
        const std::string pat = in["pattern"].get<std::string>();
        std::vector<int>  cnt(comps.size(), 0);
        for (char ch : pat) {
            const int k = std::toupper((unsigned char) ch) - 'A';
            if (k < 0 || size_t(k) >= comps.size()) { err = "pattern letters must be A.. for the listed components"; return false; }
            ++cnt[size_t(k)];
        }
        if (std::accumulate(cnt.begin(), cnt.end(), 0) == 0) { err = "empty pattern"; return false; }
        pct = cnt;
    } else if (in.contains("ratios") && in["ratios"].is_array()) {
        for (const json &v : in["ratios"])
            pct.push_back(int(std::lround(v.get<double>())));
    } else if (in.contains("percentages") && in["percentages"].is_array()) {
        for (const json &v : in["percentages"])
            pct.push_back(int(std::lround(v.get<double>())));
    } else {
        err = "give ratios (percentages) or pattern (e.g. \"AAB\")";
        return false;
    }
    if (pct.size() != comps.size()) { err = "ratios must have one value per component"; return false; }
    for (int v : pct)
        if (v < 0) { err = "ratios must be >= 0"; return false; }
    if (std::accumulate(pct.begin(), pct.end(), 0) <= 0) { err = "ratios sum to zero"; return false; }
    return true;
}

json list_mixed()
{
    const PhysInfo phys = physical_info();
    json           arr  = json::array();
    for (size_t i = 0; i < bundle().filament_presets.size(); ++i)
        if (slot_is_mixed(i))
            arr.push_back(slot_json(phys, i));
    const DynamicPrintConfig &pc = bundle().prints.get_edited_preset().config;
    const bool sub = pc.has("enable_mixed_color_sublayer") && pc.opt_bool("enable_mixed_color_sublayer");
    return {{"mixed_filaments", arr}, {"mode", sub ? "sublayers" : "layers"},
            {"free_slots", int(EnforcerBlockerType::ExtruderMax) - int(bundle().filament_presets.size())}};
}

void apply_mode(const json &in)
{
    if (!in.contains("mode") || !in["mode"].is_string())
        return;
    const std::string m = in["mode"].get<std::string>();
    ColorMixDialog::apply_method(m == "sublayers" ? ColorMix::Method::SubLayers
                                 : m == "layers"  ? ColorMix::Method::AlternatingLayers : ColorMix::Method::Auto);
}

json add_mixed(const json &in)
{
    const PhysInfo phys = physical_info();
    std::vector<unsigned> comps;
    std::vector<int>      pct;
    std::string           err;
    if (!parse_recipe(in, phys, comps, pct, err))
        return {{"error", "invalid_recipe"}, {"detail", err}};
    if (in.contains("mode") && in["mode"].is_string() && in["mode"] != "layers" && in["mode"] != "sublayers" && in["mode"] != "auto")
        return {{"error", "invalid_mode"}, {"detail", "mode: layers | sublayers | auto"}};
    if (int(bundle().filament_presets.size()) >= int(EnforcerBlockerType::ExtruderMax))
        return {{"error", "no_free_slot"}};
    plater()->take_snapshot(_u8L("Add mixed filament"));
    const size_t before = bundle().filament_presets.size();
    const int gradient  = in.value("gradient", false) ? 1 : 0;
    const int slot      = plater()->sidebar().remote_add_mixed_filament(comps, pct, gradient, in.value("gradient_direction", 0));
    if (slot < 0)
        return {{"error", "add_failed"}};
    apply_mode(in);
    const PhysInfo phys2 = physical_info();
    json           out   = slot_json(phys2, size_t(slot));
    out["created"]       = size_t(slot) >= before;
    return out;
}

} // namespace

bool Controller::dispatch_color(const Request &req, Response &out)
{
    const std::string &t    = req.target;
    const std::string  path = t.substr(0, t.find('?'));
    static const std::string base = "/api/v1/";
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::string rest = path.substr(base.size());

    auto parse_body = [&](json &body) -> bool {
        if (req.body.empty()) { body = json::object(); return true; }
        body = json::parse(req.body, nullptr, false);
        return !body.is_discarded() && body.is_object();
    };
    auto slot_of = [&](const std::string &s) -> long {
        try {
            size_t used = 0;
            const long n = std::stol(s, &used);
            return used == s.size() ? n : -1;
        } catch (...) { return -1; }
    };

    // ---- filaments -------------------------------------------------------------------------
    if (req.method == "GET" && rest == "filaments") {
        out = reply(run_on_ui([]() -> json {
            const PhysInfo phys = physical_info();
            json arr = json::array();
            for (size_t i = 0; i < bundle().filament_presets.size(); ++i)
                arr.push_back(slot_json(phys, i));
            return {{"filaments", arr}, {"count", arr.size()}, {"max", int(EnforcerBlockerType::ExtruderMax)}};
        }, 30));
        return true;
    }
    if (req.method == "PUT" && rest.rfind("filaments/", 0) == 0) {
        const long slot = slot_of(rest.substr(10));
        json body;
        if (!parse_body(body)) { out = {400, {{"error", "invalid_json"}}}; return true; }
        out = reply(run_on_ui([slot, body]() -> json {
            PresetBundle &pb = bundle();
            if (slot < 1 || size_t(slot) > pb.filament_presets.size())
                return {{"error", "unknown_filament"}};
            const size_t i = size_t(slot - 1);
            const PhysInfo phys = physical_info();
            plater()->take_snapshot(_u8L("Edit filament"));
            bool preset_changed = false;
            if (body.contains("preset")) {
                // PUT /preset only changes the first filament; this sets the preset of any slot.
                if (!body["preset"].is_string() || pb.filaments.find_preset(body["preset"].get<std::string>()) == nullptr)
                    return {{"error", "unknown_preset"}};
                if (slot_is_mixed(i))
                    return {{"error", "mixed_has_no_preset"}, {"detail", "a mixed filament uses its components' presets"}};
                const std::string name = body["preset"].get<std::string>();
                if (name != pb.filament_presets[i]) {
                    pb.set_filament_preset(i, name);
                    preset_changed = true;
                }
            }
            if (body.contains("colour")) {
                const std::string hex = body["colour"].is_string() ? norm_hex(body["colour"].get<std::string>()) : std::string();
                if (hex.empty())
                    return {{"error", "invalid_colour"}};
                if (slot_is_mixed(i))
                    return {{"error", "mixed_colour_is_derived"}, {"detail", "a mixed filament's colour follows its components"}};
                auto *o = pb.project_config.option<ConfigOptionStrings>("filament_colour");
                if (o && i < o->values.size())
                    o->values[i] = hex;
                if (auto *m = pb.project_config.option<ConfigOptionStrings>("filament_multi_colour"); m && i < m->values.size())
                    m->values[i] = hex;
            }
            if (body.contains("multi_colour")) {
                if (!body["multi_colour"].is_string())
                    return {{"error", "invalid_multi_colour"}};
                // "#RRGGBB #RRGGBB ..." (space separated, as the filament editor writes it)
                std::istringstream iss(body["multi_colour"].get<std::string>());
                std::string tok, joined;
                while (iss >> tok) {
                    const std::string h = norm_hex(tok);
                    if (h.empty())
                        return {{"error", "invalid_multi_colour"}, {"detail", "bad colour '" + tok + "'"}};
                    joined += (joined.empty() ? "" : " ") + h;
                }
                auto *m = pb.project_config.option<ConfigOptionStrings>("filament_multi_colour");
                if (m && i < m->values.size())
                    m->values[i] = joined;
            }
            if (body.contains("td")) {
                const int k = phys_index_of_slot(phys, i);
                if (k < 0)
                    return {{"error", "td_needs_physical"}};
                const double td = body["td"].get<double>();
                if (td < 0.0 || td > 50.0)
                    return {{"error", "invalid_td"}, {"detail", "0 (unknown) .. 50 mm"}};
                const std::string key = "td_" + key_safe(phys.color[size_t(k)]) + "_" + key_safe(phys.name[size_t(k)]);
                wxGetApp().app_config->set(kSection, key, td > 0.0 ? wxString::Format("%.1f", td).ToStdString() : std::string());
            }
            if (preset_changed) {
                pb.normalize_filament_colour_lists();
                plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
                plater()->sidebar().update_dynamic_filament_list();
                pb.export_selections(*wxGetApp().app_config);
            }
            plater()->update_filament_colors_in_full_config();
            plater()->sidebar().update_mixed_filament_list();
            plater()->update_project_dirty_from_presets();
            return slot_json(physical_info(), i);
        }, 30));
        return true;
    }

    // ---- mixed filaments -------------------------------------------------------------------
    if (rest == "mixed_filaments") {
        if (req.method == "GET") {
            out = reply(run_on_ui([]() { return list_mixed(); }, 30));
            return true;
        }
        if (req.method == "POST") {
            json body;
            if (!parse_body(body)) { out = {400, {{"error", "invalid_json"}}}; return true; }
            out = reply(run_on_ui([body]() { return add_mixed(body); }, 60));
            if (out.status == 200) out.status = 201;
            return true;
        }
    }
    if ((req.method == "PUT" || req.method == "DELETE") && rest.rfind("mixed_filaments/", 0) == 0) {
        const long slot = slot_of(rest.substr(16));
        json body;
        if (req.method == "PUT" && !parse_body(body)) { out = {400, {{"error", "invalid_json"}}}; return true; }
        const bool del = req.method == "DELETE";
        out = reply(run_on_ui([slot, body, del]() -> json {
            PresetBundle &pb = bundle();
            if (slot < 1 || size_t(slot) > pb.filament_presets.size() || !slot_is_mixed(size_t(slot - 1)))
                return {{"error", "unknown_mixed_filament"}};
            const size_t i = size_t(slot - 1);
            Plater *pl = plater();
            if (del) {
                const auto idx = pl->mixed_filament_config_indices();
                auto it = std::find(idx.begin(), idx.end(), i);
                if (it == idx.end())
                    return {{"error", "unknown_mixed_filament"}};
                pl->take_snapshot(_u8L("Remove mixed filament"));
                pl->sidebar().delete_mixed_filament_at(size_t(it - idx.begin()));
                return {{"removed", slot}, {"filaments", pb.filament_presets.size()}};
            }
            const PhysInfo phys = physical_info();
            std::vector<unsigned> comps;
            std::vector<int>      pct;
            if (body.contains("components") || body.contains("ratios") || body.contains("pattern") || body.contains("percentages")) {
                json merged = body;
                if (!merged.contains("components")) {
                    json c = json::array();
                    for (unsigned v : parse_mixed_components(str_at("filament_mixed_components", i)))
                        c.push_back(phys.slot[v - 1] + 1);
                    merged["components"] = c;
                }
                std::string err;
                if (!parse_recipe(merged, phys, comps, pct, err))
                    return {{"error", "invalid_recipe"}, {"detail", err}};
            } else {
                comps = parse_mixed_components(str_at("filament_mixed_components", i));
                for (double r : parse_mixed_ratios(str_at("filament_mixed_sublayer_ratios", i), comps.size()))
                    pct.push_back(int(std::lround(r * 100.0)));
            }
            int gradient = -1;
            if (body.contains("gradient"))
                gradient = body["gradient"].get<bool>() ? 1 : 0;
            pl->take_snapshot(_u8L("Edit mixed filament"));
            if (!pl->sidebar().remote_update_mixed_filament(i, comps, pct, gradient, body.value("gradient_direction", 0)))
                return {{"error", "update_failed"}};
            apply_mode(body);
            return slot_json(physical_info(), i);
        }, 60));
        return true;
    }

    // ---- colour mixing engine ----------------------------------------------------------------
    if (req.method == "POST" && (rest == "color_mix/solve" || rest == "color_mix/apply")) {
        json body;
        if (!parse_body(body)) { out = {400, {{"error", "invalid_json"}}}; return true; }
        const bool solve = rest == "color_mix/solve";
        out = reply(run_on_ui([body, solve]() { return solve ? do_solve(body) : do_apply(body); }, 120));
        return true;
    }

    // ---- model colours -----------------------------------------------------------------------
    if (req.method == "GET" && rest == "model/colors") {
        out = reply(run_on_ui([]() -> json {
            Plater *pl = plater();
            const PhysInfo phys = physical_info();
            json out = {{"pending_import", nullptr}};
            if (const auto *pend = pl->remote_pending_color_import()) {
                json cl = json::array();
                for (size_t i = 0; i < pend->cluster_colors.size(); ++i)
                    cl.push_back({{"cluster", i}, {"colour", hex_u8(pend->cluster_colors[i])},
                                  {"share", pend->cluster_share[i]}, {"faces", pend->cluster_faces[i]}});
                out["pending_import"] = {{"clusters", cl}, {"objects", pend->object_indices}};
            }
            std::map<int, size_t> parts;
            for (const ModelObject *o : pl->model().objects)
                for (const ModelVolume *v : o->volumes)
                    if (v->is_model_part())
                        for (int e : v->get_extruders())
                            if (e >= 1)
                                ++parts[e];
            json used = json::array();
            for (const auto &[e, n] : parts)
                used.push_back({{"filament", e}, {"colour", str_at("filament_colour", size_t(e - 1))}, {"parts", n},
                                {"mixed", slot_is_mixed(size_t(e - 1))}});
            out["filaments_in_use"] = used;
            return out;
        }, 30));
        return true;
    }
    if (req.method == "POST" && rest == "model/import_colored") {
        json body;
        if (!parse_body(body) || !body.contains("path") || !body["path"].is_string()) {
            out = {400, {{"error", "missing_path"}}};
            return true;
        }
        const std::string file = body["path"].get<std::string>();
        out = reply(run_on_ui([file, body]() -> json {
            if (!boost::filesystem::exists(file))
                return {{"error", "not_found"}};
            Plater::ColorImportInfo info;
            Plater *pl = plater();
            std::vector<size_t> idxs = pl->remote_load_colored_model(boost::filesystem::path(boost::nowide::widen(file)),
                                                                     body.value("max_colors", 0), std::clamp(body.value("smooth", 5), 0, 10), info);
            json objs = json::array();
            for (size_t i : idxs)
                if (i < pl->model().objects.size())
                    objs.push_back({{"id", (uint64_t) pl->model().objects[i]->id().id}, {"index", i}, {"name", pl->model().objects[i]->name}});
            json r = {{"objects", objs}, {"colored", info.ok}};
            if (!info.ok) {
                r["detail"] = info.error.empty() ? "the model carries no importable colours (MTL / vertex colours / texture)" : info.error;
                return r;
            }
            json cl = json::array();
            for (size_t i = 0; i < info.cluster_colors.size(); ++i)
                cl.push_back({{"cluster", i}, {"colour", hex_u8(info.cluster_colors[i])}, {"share", info.cluster_share[i]}, {"faces", info.cluster_faces[i]}});
            r["palette"] = cl;
            r["next"]    = "color_mix/solve with targets=\"pending\", then color_mix/apply";
            return r;
        }, 180));
        return true;
    }

    // ---- G-code post-processing check ----------------------------------------------------------
    // Runs the post-processing scripts / slicing-pipeline plugins on a private copy of the current plate's
    // G-code exactly as the printer-agent send paths do, reports the outcome and deletes the copy.
    if (req.method == "POST" && rest == "gcode/post_process_check") {
        json body;
        if (!parse_body(body)) { out = {400, {{"error", "invalid_json"}}}; return true; }
        out = reply(run_on_ui([body]() -> json {
            PartPlate *plate = plater()->get_partplate_list().get_curr_plate();
            if (plate == nullptr || plate->fff_print() == nullptr || !plate->is_valid_gcode_file())
                return {{"error", "not_sliced"}, {"detail", "slice the current plate first"}};
            const std::string src  = plate->get_tmp_gcode_path();
            const std::string name = body.value("output_name", std::string("check.gcode"));
            try {
                SendGCodeCopy c = post_process_gcode_for_send(src, body.value("host", std::string("Creality")), name,
                                                              plate->fff_print()->full_print_config());
                json r = {{"processed", c.processed}, {"output_name", c.output_name}, {"original_bytes", boost::filesystem::file_size(src)}};
                if (c.processed)
                    r["processed_bytes"] = boost::filesystem::file_size(c.path);
                return r;
            } catch (const std::exception &e) {
                return {{"error", "post_process_failed"}, {"detail", e.what()}};
            }
        }, 120));
        return true;
    }

    // ---- per-object / per-volume filament ------------------------------------------------------
    if (req.method == "PUT" && rest.rfind("objects/", 0) == 0 && rest.size() > 9 && rest.compare(rest.size() - 9, 9, "/filament") == 0) {
        uint64_t oid = 0;
        try { oid = std::stoull(rest.substr(8, rest.size() - 9 - 8)); }
        catch (...) { out = {400, {{"error", "bad_object_id"}}}; return true; }
        json body;
        if (!parse_body(body)) { out = {400, {{"error", "invalid_json"}}}; return true; }
        out = reply(run_on_ui([oid, body]() -> json {
            Plater *pl = plater();
            Model  &model = pl->model();
            int idx = -1;
            for (size_t i = 0; i < model.objects.size(); ++i)
                if ((uint64_t) model.objects[i]->id().id == oid)
                    idx = int(i);
            if (idx < 0)
                return {{"error", "unknown_object"}};
            if (!body.contains("filament") || !body["filament"].is_number_integer())
                return {{"error", "missing_filament"}, {"detail", "filament: 1-based project slot (0 = object default)"}};
            const long f = body["filament"].get<long>();
            if (f < 0 || size_t(f) > bundle().filament_presets.size())
                return {{"error", "invalid_filament"}};
            ModelObject *obj = model.objects[size_t(idx)];
            pl->take_snapshot(_u8L("Set filament"));
            if (body.contains("volume")) {
                const long vi = body["volume"].get<long>();
                if (vi < 0 || size_t(vi) >= obj->volumes.size())
                    return {{"error", "unknown_volume"}};
                ModelVolume *v = obj->volumes[size_t(vi)];
                if (f == 0) v->config.erase("extruder"); else v->config.set("extruder", int(f));
            } else {
                if (f == 0) obj->config.erase("extruder"); else obj->config.set("extruder", int(f));
                if (body.value("all_volumes", true))
                    for (ModelVolume *v : obj->volumes)
                        if (v->is_model_part() && v->config.has("extruder"))
                            v->config.erase("extruder");
            }
            if (auto *ol = wxGetApp().obj_list()) {
                ol->update_objects_list_filament_column(std::max<size_t>(wxGetApp().filaments_cnt(), size_t(f)));
                ol->update_info_items(size_t(idx));
            }
            pl->changed_object(idx);
            pl->update();
            return {{"object", oid}, {"filament", f}, {"volume", body.contains("volume") ? body["volume"] : json(nullptr)}};
        }, 30));
        return true;
    }
    return false;
}

}}} // namespace Slic3r::GUI::RemoteAPI
