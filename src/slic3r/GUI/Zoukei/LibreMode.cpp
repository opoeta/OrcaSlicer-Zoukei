// Orca Zoukei: Libre Mode switch, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0. See LibreMode.hpp.
#include "LibreMode.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <imgui/imgui.h>

#include <algorithm>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace LibreMode {

namespace {

constexpr const char *KEY_ENABLED = "zoukei_libre_mode";
constexpr const char *KEY_SNAP    = "zoukei_snap_drag";
constexpr const char *KEY_BED     = "zoukei_snap_drag_bed";
constexpr const char *KEY_BLOCK   = "zoukei_snap_drag_group";

// app_config is null very early in start-up and again during teardown, and the render paths that ask run in both windows.
// A key absent from the ini reads as `fallback` (no seed in AppConfig::set_defaults, which also runs on empty storage).
bool read_flag(const char *key, bool fallback)
{
    const AppConfig *ac = wxGetApp().app_config;
    if (ac == nullptr)
        return false;
    return ac->has(key) ? ac->get_bool(key) : fallback;
}

void write_flag(const char *key, bool on)
{
    AppConfig *ac = wxGetApp().app_config;
    if (ac == nullptr)
        return;
    ac->set_bool(key, on);
    ac->save();
}

} // namespace

bool enabled() { return read_flag(KEY_ENABLED, false); }

void sync_to_bundle()
{
    if (wxGetApp().preset_bundle != nullptr)
        wxGetApp().preset_bundle->libre_true_objects = enabled();
}

void set_enabled(bool on)
{
    if (enabled() == on)
        return;
    write_flag(KEY_ENABLED, on);
    sync_to_bundle();
    // True Objects changes the slicing itself: the plate result is stale and the next slice must apply the new mirror.
    if (Plater *plater = wxGetApp().plater()) {
        plater->schedule_background_process();
        plater->update_menus(); // the object menu gains / loses the Libre Mode items
    }
}

bool snap_drag() { return read_flag(KEY_SNAP, true); }
void set_snap_drag(bool on) { write_flag(KEY_SNAP, on); }
bool snap_to_bed() { return read_flag(KEY_BED, true); }
void set_snap_to_bed(bool on) { write_flag(KEY_BED, on); }
bool move_as_block() { return read_flag(KEY_BLOCK, false); }
void set_move_as_block(bool on) { write_flag(KEY_BLOCK, on); }

void render_imgui_options()
{
    ImGui::TextUnformatted(_u8L("Libre Mode").c_str());
    ImGui::SameLine();
    bool on = enabled();
    if (ImGui::Checkbox("##libre_master", &on))
        set_enabled(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("True Objects: the slicer measures what is really under every surface, so a face resting on "
                                     "another object prints as a normal contact surface and only the part over air is a bridge. "
                                     "Also enables Snap & Drag. Off = stock behaviour.").c_str());
    if (!on)
        return;

    bool snap = snap_drag();
    if (ImGui::Checkbox((_u8L("Snap & Drag") + "##libre_snap").c_str(), &snap))
        set_snap_drag(snap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("While dragging, rest the object on the top surface of whatever real part is underneath it.").c_str());

    const float dim = ImGui::GetStyle().Alpha * 0.5f;
    if (!snap)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, dim);
    bool bed = snap_to_bed();
    if (ImGui::Checkbox((_u8L("Snap to bed") + "##libre_bed").c_str(), &bed) && snap)
        set_snap_to_bed(bed);
    bool block = move_as_block();
    if (ImGui::Checkbox((_u8L("Move selection as one block") + "##libre_block").c_str(), &block) && snap)
        set_move_as_block(block);
    if (!snap)
        ImGui::PopStyleVar();
}

// --- Object list actions -----------------------------------------------------------------------------------------------

namespace {

// Objects of the current selection: the 3D selection first, the object list as fallback (a right click clears the 3D one).
std::vector<size_t> selected_objects()
{
    std::vector<size_t> out;
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return out;
    const size_t n = plater->model().objects.size();
    auto add = [&out, n](int idx) {
        if (idx >= 0 && size_t(idx) < n && std::find(out.begin(), out.end(), size_t(idx)) == out.end())
            out.push_back(size_t(idx));
    };
    if (GLCanvas3D *canvas = plater->canvas3D())
        for (const std::pair<int, int> &oi : canvas->get_selection().get_selected_object_instances())
            add(oi.first);
    if (out.empty() && wxGetApp().obj_list() != nullptr) {
        std::vector<int> obj_idxs, vol_idxs;
        wxGetApp().obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
        for (int idx : obj_idxs)
            add(idx);
    }
    return out;
}

DynamicPrintConfig s_process_clipboard;
bool               s_process_clipboard_all = false;

} // namespace

bool assemble_boolean_available()
{
    if (!enabled())
        return false;
    const std::vector<size_t> sel = selected_objects();
    if (sel.empty())
        return false;
    const ModelObject *mo = wxGetApp().plater()->model().objects[sel.front()];
    return mo != nullptr && mo->volumes.size() > 1;
}

bool assemble_boolean_checked()
{
    const std::vector<size_t> sel = selected_objects();
    if (sel.empty())
        return true;
    const ModelObject *mo  = wxGetApp().plater()->model().objects[sel.front()];
    const ConfigOption *opt = mo != nullptr ? mo->config.option("neotko_assemble_boolean") : nullptr;
    return opt == nullptr ? true : opt->getBool();
}

void toggle_assemble_boolean()
{
    if (!enabled())
        return;
    const std::vector<size_t> sel = selected_objects();
    if (sel.empty())
        return;
    Plater *plater = wxGetApp().plater();
    // The new value derives from the first selected object (inverts its current state); the default is on, so the first toggle turns it off.
    const bool new_value = !assemble_boolean_checked();
    plater->take_snapshot("Toggle assembled boolean mode");
    for (size_t idx : sel)
        plater->model().objects[idx]->config.set_key_value("neotko_assemble_boolean", new ConfigOptionBool(new_value));
    plater->changed_objects(sel);
    plater->update();
}

void copy_process_settings(const std::string &category)
{
    if (!enabled())
        return;
    const std::vector<size_t> sel = selected_objects();
    if (sel.empty())
        return;
    const DynamicPrintConfig &src = wxGetApp().plater()->model().objects[sel.front()]->config.get();
    s_process_clipboard_all = category.empty();
    if (category.empty()) {
        s_process_clipboard = src; // All: every override as it is
        return;
    }
    DynamicPrintConfig filtered;
    for (const std::string &key : src.keys()) {
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def != nullptr && def->category == category)
            filtered.set_key_value(key, src.option(key)->clone());
    }
    s_process_clipboard = std::move(filtered);
}

void paste_process_settings()
{
    if (!enabled() || s_process_clipboard.empty())
        return;
    const std::vector<size_t> sel = selected_objects();
    if (sel.empty())
        return;
    Plater *plater = wxGetApp().plater();
    plater->take_snapshot("Paste Process Settings");
    // All replaces the whole per-object config; a block merges, so only that block's keys are overwritten.
    for (size_t idx : sel) {
        if (s_process_clipboard_all)
            plater->model().objects[idx]->config.assign_config(s_process_clipboard);
        else
            plater->model().objects[idx]->config.apply(s_process_clipboard);
    }
    plater->changed_objects(sel);
    if (wxGetApp().params_panel() != nullptr)
        wxGetApp().params_panel()->notify_object_config_changed();
}

bool has_process_settings_clipboard() { return !s_process_clipboard.empty(); }

} // namespace LibreMode
} // namespace GUI
} // namespace Slic3r
