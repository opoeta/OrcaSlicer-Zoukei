// Orca Zoukei: ColorStitch Painter gizmo, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// See GLGizmoColorStitchPainter.hpp.
#include "GLGizmoColorStitchPainter.hpp"

#include "libslic3r/ColorStitch.hpp"
#include "libslic3r/ColorStitchPaint.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SurfaceEffectProfile.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoUtils.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include <glad/gl.h>
#include <imgui.h>
#include <wx/busycursor.h>

namespace Slic3r::GUI {

namespace {

constexpr int kSlots = ColorStitchPaint::kSlotCount;

ColorRGBA argb_to_color(uint32_t argb)
{
    return ColorRGBA(float((argb >> 16) & 0xFF) / 255.f, float((argb >> 8) & 0xFF) / 255.f, float(argb & 0xFF) / 255.f, 1.f);
}

ImVec4 argb_to_imvec(uint32_t argb)
{
    return ImVec4(float((argb >> 16) & 0xFF) / 255.f, float((argb >> 8) & 0xFF) / 255.f, float(argb & 0xFF) / 255.f, 1.f);
}

bool combo_items(const char *id, int &selection, const std::vector<std::string> &items, float width)
{
    std::vector<const char *> c_items;
    c_items.reserve(items.size());
    for (const std::string &s : items)
        c_items.push_back(s.c_str());
    if (c_items.empty())
        return false;
    selection = std::clamp(selection, 0, int(c_items.size()) - 1);
    ImGui::PushItemWidth(width);
    const bool changed = ImGui::Combo(id, &selection, c_items.data(), int(c_items.size()));
    ImGui::PopItemWidth();
    return changed;
}

int kv_int(const std::map<std::string, std::string> &kv, const std::string &key, int fallback)
{
    auto it = kv.find(key);
    if (it == kv.end())
        return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

// 0-based filaments A / B of a ColorStitch pass: the pass payload, else the recipe payload, else 1 / 2.
std::pair<int, int> colorstitch_tools(const SurfacePass &pass, const SurfaceEffectProfile *profile)
{
    int a = 1, b = 2;
    if (profile != nullptr) {
        a = kv_int(profile->colorstitch.kv, "interlayer_colormix_filament_a", a);
        b = kv_int(profile->colorstitch.kv, "interlayer_colormix_filament_b", b);
    }
    a = kv_int(pass.colorstitch.kv, "interlayer_colormix_filament_a", a);
    b = kv_int(pass.colorstitch.kv, "interlayer_colormix_filament_b", b);
    return { a - 1, b - 1 };
}

PathBlendPassConfig pathblend_of(const SurfacePass &pass)
{
    auto it = pass.pathblend.kv.find("blob");
    if (it != pass.pathblend.kv.end() && !it->second.empty())
        return PathBlendPassConfig::from_blob_json(it->second);
    PathBlendPassConfig pb;
    pb.enabled = true;
    return pb;
}

void set_pathblend(SurfacePass &pass, PathBlendPassConfig pb)
{
    pb.enabled = true;
    pb.sync_legacy_view();
    pass.pathblend.present   = true;
    pass.pathblend.kv["blob"] = pb.to_blob_json();
}

// Volumes of `mo` that carry `profile_id` in their slot table, with the slot.
std::vector<std::pair<ModelVolume *, int>> slots_of_profile(ModelObject &mo, int profile_id)
{
    std::vector<std::pair<ModelVolume *, int>> out;
    for (ModelVolume *mv : mo.volumes)
        for (size_t s = 1; s < mv->colorstitch_slot_to_profile_id.size(); ++s)
            if (mv->colorstitch_slot_to_profile_id[s] == profile_id)
                out.emplace_back(mv, int(s));
    return out;
}

EnforcerBlockerStateMap erase_state_map(int slot)
{
    EnforcerBlockerStateMap map;
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = EnforcerBlockerType(int(i));
    if (slot > 0 && slot < int(map.size()))
        map[size_t(slot)] = EnforcerBlockerType::NONE;
    return map;
}

} // namespace

GLGizmoColorStitchPainter::GLGizmoColorStitchPainter(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::FillButtonIcon)
{
}

bool GLGizmoColorStitchPainter::on_init()
{
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["remove_all"]       = _L("Erase all");
    // A flat face is the usual target: flood only the coplanar faces by default.
    m_smart_fill_angle = 5.f;
    return true;
}

std::string GLGizmoColorStitchPainter::on_get_name() const
{
    return _u8L("ColorStitch Painter");
}

PainterGizmoType GLGizmoColorStitchPainter::get_painter_type() const
{
    // No painter type specific behaviour in the base: this one paints like the fuzzy skin gizmo.
    return PainterGizmoType::FUZZY_SKIN;
}

wxString GLGizmoColorStitchPainter::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    return shift_down ? _L("Erase ColorStitch paint") : _L("Paint ColorStitch recipe");
}

void GLGizmoColorStitchPainter::on_opening()
{
    m_status.clear();
    m_active_slot = 0;
    if (m_active_profile > 0 && SurfaceEffectProfileManager::get().find(m_active_profile) == nullptr)
        m_active_profile = 0;
    if (m_active_profile == 0 && SurfaceEffectProfileManager::get().size() > 0)
        m_active_profile = SurfaceEffectProfileManager::get().list().front().id;
}

void GLGizmoColorStitchPainter::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
    if (m_reslice_pending)
        schedule_reslice();
}

void GLGizmoColorStitchPainter::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

bool GLGizmoColorStitchPainter::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (action == SLAGizmoEventType::LeftDown && !shift_down) {
        if (m_active_profile <= 0 || SurfaceEffectProfileManager::get().find(m_active_profile) == nullptr) {
            m_status = _u8L("Pick or create a recipe first.");
            return true; // consume the click: nothing to paint with
        }
        if (ensure_active_slot() == 0)
            return true;
    }
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

int GLGizmoColorStitchPainter::object_filament() const
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo != nullptr && mo->config.has("extruder"))
        return std::max(1, mo->config.extruder());
    return 1;
}

uint32_t GLGizmoColorStitchPainter::preview_argb(const SurfacePassStack &stack, const SurfaceEffectProfile *profile) const
{
    double r = 0., g = 0., b = 0., w = 0.;
    auto add = [&](int tool0, double weight) {
        ColorRGBA c(0.5f, 0.5f, 0.5f, 1.f);
        if (tool0 >= 0 && tool0 < int(m_filament_colors.size()))
            decode_color(m_filament_colors[size_t(tool0)], c);
        r += c.r() * weight;
        g += c.g() * weight;
        b += c.b() * weight;
        w += weight;
    };
    const int natural = object_filament() - 1;
    for (const SurfacePass &pass : stack.passes) {
        const double weight = std::max(0.01, pass.ratio);
        switch (pass.kind) {
        case SurfacePassKind::Solid: add(pass.solid_tool < 0 ? natural : pass.solid_tool, weight); break;
        case SurfacePassKind::ColorStitch: {
            const std::pair<int, int> t = colorstitch_tools(pass, profile);
            add(t.first, 0.5 * weight);
            add(t.second, 0.5 * weight);
            break;
        }
        case SurfacePassKind::PathBlend: {
            const PathBlendPassConfig pb = pathblend_of(pass);
            add(pb.tool_bottom, 0.5 * weight);
            add(pb.mode == PathBlendPassConfig::Mode::Full ? pb.tool_top : natural, 0.5 * weight);
            break;
        }
        default: add(natural, weight); break;
        }
    }
    if (w <= 0.)
        return 0xFF808080u;
    auto to8 = [w](double v) { return uint32_t(std::clamp(v / w, 0., 1.) * 255. + 0.5); };
    return 0xFF000000u | (to8(r) << 16) | (to8(g) << 8) | to8(b);
}

std::vector<ColorRGBA> GLGizmoColorStitchPainter::state_colors(const ModelVolume &mv) const
{
    const SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    std::vector<ColorRGBA>             colors(size_t(kSlots), ColorRGBA(0.45f, 0.45f, 0.45f, 1.f));
    colors[0] = GLVolume::NEUTRAL_COLOR;
    for (int s = 1; s < kSlots; ++s) {
        const int pid = ColorStitchPaint::profile_for_slot(mv, s);
        if (pid <= 0)
            continue;
        if (const SurfaceEffectProfile *p = mgr.find(pid))
            colors[size_t(s)] = argb_to_color(p->preview_argb != 0 ? p->preview_argb : preview_argb(SurfacePassStack::from_json(p->stack_top_json), p));
        else
            colors[size_t(s)] = ColorRGBA(0.25f, 0.25f, 0.25f, 1.f); // orphan paint
    }
    return colors;
}

void GLGizmoColorStitchPainter::refresh_state_colors()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    size_t idx = 0;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        if (idx < m_triangle_selectors.size())
            if (auto *patch = dynamic_cast<TriangleSelectorPatch *>(m_triangle_selectors[idx].get())) {
                patch->set_ebt_colors(state_colors(*mv));
                patch->request_update_render_data();
            }
        ++idx;
    }
    m_parent.set_as_dirty();
}

const std::vector<bool> &GLGizmoColorStitchPainter::used_states()
{
    if (m_used_states_dirty || m_used_states.size() != size_t(kSlots)) {
        m_used_states.assign(size_t(kSlots), false);
        for (const auto &selector : m_triangle_selectors) {
            const TriangleSelector::TriangleSplittingData data = selector->serialize();
            for (size_t s = 1; s < data.used_states.size() && s < m_used_states.size(); ++s)
                if (data.used_states[s])
                    m_used_states[s] = true;
        }
        m_used_states_dirty = false;
    }
    return m_used_states;
}

void GLGizmoColorStitchPainter::update_model_object()
{
    m_used_states_dirty = true;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    bool updated = false;
    int  idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        if (idx < int(m_triangle_selectors.size()))
            updated |= mv->colorstitch_paint_facets.set(*m_triangle_selectors[size_t(idx)]);
    }
    if (updated)
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoColorStitchPainter::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    m_filament_colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    m_active_slot       = 0;
    m_used_states_dirty = true;
    if (mo == nullptr)
        return;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh *mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, state_colors(*mv)));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor.
        m_triangle_selectors.back()->deserialize(mv->colorstitch_paint_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }
}

int GLGizmoColorStitchPainter::ensure_active_slot()
{
    m_active_slot = 0;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_active_profile <= 0)
        return 0;
    std::vector<ModelVolume *> parts;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            parts.push_back(mv);
    if (parts.empty() || parts.size() != m_triangle_selectors.size())
        return 0;

    // Paint states in use, per part.
    std::vector<std::vector<bool>> used(parts.size(), std::vector<bool>(size_t(kSlots), false));
    for (size_t i = 0; i < parts.size(); ++i) {
        const TriangleSelector::TriangleSplittingData data = m_triangle_selectors[i]->serialize();
        for (size_t s = 0; s < data.used_states.size() && s < size_t(kSlots); ++s)
            used[i][s] = data.used_states[s];
    }
    // Slot usable for the active recipe on part i: mapped to it, unmapped, or mapped but unpainted.
    auto usable = [&](size_t i, int s) {
        const int pid = ColorStitchPaint::profile_for_slot(*parts[i], s);
        return pid == m_active_profile || pid == 0 || !used[i][size_t(s)];
    };
    auto usable_everywhere = [&](int s) {
        for (size_t i = 0; i < parts.size(); ++i)
            if (!usable(i, s))
                return false;
        return true;
    };

    int slot = 0;
    // The slot the recipe already has on this object.
    for (size_t i = 0; i < parts.size() && slot == 0; ++i)
        for (int s = 1; s < kSlots; ++s)
            if (ColorStitchPaint::profile_for_slot(*parts[i], s) == m_active_profile && usable_everywhere(s)) {
                slot = s;
                break;
            }
    // Else a free one.
    for (int s = 1; s < kSlots && slot == 0; ++s) {
        bool painted = false;
        for (size_t i = 0; i < parts.size(); ++i)
            painted |= used[i][size_t(s)];
        if (!painted)
            slot = s;
    }
    if (slot == 0) {
        m_status = _u8L("All 32 paint slots of this object are in use. Free a recipe in \"In use on this object\" first.");
        return 0;
    }
    bool table_changed = false;
    for (ModelVolume *mv : parts) {
        if (mv->colorstitch_slot_to_profile_id.size() < size_t(kSlots))
            mv->colorstitch_slot_to_profile_id.resize(size_t(kSlots), 0);
        if (mv->colorstitch_slot_to_profile_id[size_t(slot)] != m_active_profile) {
            mv->colorstitch_slot_to_profile_id[size_t(slot)] = m_active_profile;
            table_changed = true;
        }
    }
    if (table_changed)
        refresh_state_colors();
    m_status.clear();
    m_active_slot = slot;
    return slot;
}

void GLGizmoColorStitchPainter::free_slot(int slot)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || slot <= 0 || slot >= kSlots)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Free ColorStitch paint slot"), UndoRedo::SnapshotType::GizmoAction);
    const EnforcerBlockerStateMap map = erase_state_map(slot);
    for (auto &selector : m_triangle_selectors) {
        selector->remap_triangle_state(map);
        selector->request_update_render_data(true);
    }
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part() && size_t(slot) < mv->colorstitch_slot_to_profile_id.size())
            mv->colorstitch_slot_to_profile_id[size_t(slot)] = 0;
    if (m_active_slot == slot)
        m_active_slot = 0;
    update_model_object();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}

void GLGizmoColorStitchPainter::erase_all()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset selection"), UndoRedo::SnapshotType::GizmoAction);
    for (auto &selector : m_triangle_selectors) {
        selector->reset();
        selector->request_update_render_data(true);
    }
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            mv->colorstitch_slot_to_profile_id.clear();
    m_active_slot = 0;
    update_model_object();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}

int GLGizmoColorStitchPainter::new_recipe()
{
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    SurfacePassStack             stack;
    stack.enabled = true;
    SurfacePass pass;
    pass.kind       = SurfacePassKind::Solid;
    pass.ratio      = 1.0;
    pass.solid_tool = m_filament_colors.size() >= 2 ? 1 : 0;
    stack.passes.push_back(pass);
    SurfaceEffectProfile p;
    p.name           = _u8L("Recipe") + " " + std::to_string(mgr.next_id());
    p.stack_top_json = stack.to_json();
    p.preview_argb   = preview_argb(stack, nullptr);
    return mgr.add(std::move(p));
}

int GLGizmoColorStitchPainter::capture_recipe_from_object()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return 0;
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config();
    cfg.apply(mo->config.get(), true);
    PrintObjectConfig object_cfg;
    object_cfg.apply(cfg, true);
    const SurfacePassStack stack = SurfacePassStack::resolve(object_cfg);
    if (!stack.any_effect()) {
        m_status = _u8L("This object has no Sandwich, ColorStitch or PathBlend settings to capture (Surface colour page).");
        return 0;
    }
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    SurfaceEffectProfile         p;
    p.name           = mo->name + " " + std::to_string(mgr.next_id());
    p.stack_top_json = stack.to_json();
    p.colorstitch    = SurfaceEffectProfileManager::snapshot_keys(cfg, SurfaceEffectProfileManager::colorstitch_keys());
    p.pathblend      = SurfaceEffectProfileManager::snapshot_keys(cfg, SurfaceEffectProfileManager::pathblend_keys());
    p.multipass      = SurfaceEffectProfileManager::snapshot_keys(cfg, SurfaceEffectProfileManager::multipass_keys());
    p.preview_argb   = preview_argb(stack, &p);
    m_status         = _u8L("Recipe captured. Paint it on the faces that should carry it; the object settings themselves are unchanged.");
    return mgr.add(std::move(p));
}

int GLGizmoColorStitchPainter::duplicate_recipe(int profile_id)
{
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    const SurfaceEffectProfile  *src = mgr.find(profile_id);
    if (src == nullptr)
        return 0;
    SurfaceEffectProfile p = *src;
    p.id             = 0;
    p.auto_generated = false;
    p.name           = src->name + " " + _u8L("copy");
    return mgr.add(std::move(p));
}

void GLGizmoColorStitchPainter::delete_recipe(int profile_id)
{
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    if (mgr.find(profile_id) == nullptr)
        return;
    // Where is it painted?
    Model &model = wxGetApp().plater()->model();
    size_t painted_objects = 0;
    for (ModelObject *mo : model.objects)
        if (!slots_of_profile(*mo, profile_id).empty())
            ++painted_objects;
    if (painted_objects > 0) {
        const wxString msg = wxString::Format(_L("This recipe is painted on %d object(s). Delete it and erase that paint?"), int(painted_objects));
        MessageDialog  dlg(nullptr, msg, _L("ColorStitch Painter"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete ColorStitch recipe"), UndoRedo::SnapshotType::GizmoAction);
        // Keep the open object in sync first, then erase the paint everywhere.
        update_model_object();
        for (ModelObject *mo : model.objects)
            for (const std::pair<ModelVolume *, int> &vs : slots_of_profile(*mo, profile_id)) {
                vs.first->colorstitch_paint_facets.remap_states(*vs.first, erase_state_map(vs.second));
                vs.first->colorstitch_slot_to_profile_id[size_t(vs.second)] = 0;
            }
        mgr.remove(profile_id);
    }
    if (m_active_profile == profile_id) {
        m_active_profile = mgr.size() > 0 ? mgr.list().front().id : 0;
        m_active_slot    = 0;
    }
    update_from_model_object(false);
    schedule_reslice();
}

void GLGizmoColorStitchPainter::commit_stack(const SurfacePassStack &stack, bool reslice)
{
    SurfaceEffectProfile *p = SurfaceEffectProfileManager::get().find_mut(m_active_profile);
    if (p == nullptr)
        return;
    p->stack_top_json = stack.to_json();
    p->preview_argb   = preview_argb(stack, p);
    refresh_state_colors();
    m_reslice_pending = true;
    if (reslice)
        schedule_reslice();
}

void GLGizmoColorStitchPainter::schedule_reslice()
{
    m_reslice_pending = false;
    // The recipe lives outside the model: Print::apply notices the change through the recipe
    // fingerprint of the painted volumes. The snapshot marks the project as modified.
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Edit ColorStitch recipe"), UndoRedo::SnapshotType::GizmoAction);
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoColorStitchPainter::render_tools(float max_tooltip_width)
{
    float scale = m_parent.get_scale();
#ifdef WIN32
    int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    scale *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32
    const float empty_button_width = m_imgui->calc_button_size("").x;

    std::array<wchar_t, 3> tool_ids = { ImGui::FillButtonIcon, ImGui::CircleButtonIcon, ImGui::TriangleButtonIcon };
    std::array<wchar_t, 3> icons;
    if (m_is_dark_mode)
        icons = { ImGui::FillButtonDarkIcon, ImGui::CircleButtonDarkIcon, ImGui::TriangleButtonDarkIcon };
    else
        icons = { ImGui::FillButtonIcon, ImGui::CircleButtonIcon, ImGui::TriangleButtonIcon };
    std::array<wxString, 3> tool_tips = { _L("Smart fill: the flat face under the cursor"), _L("Circle"), _L("Triangle") };
    for (size_t i = 0; i < tool_ids.size(); ++i) {
        if (i != 0)
            ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * float(i) + m_imgui->scaled(1.5f));
        const bool is_active = m_current_tool == tool_ids[i];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f * scale, 4.f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(.6f, .6f, .6f, .2f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, is_active ? ImVec4(0.f, .59f, .53f, .30f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_BorderActive, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        const bool clicked = m_imgui->glyph_button(icons[i], ImVec2(16.f * scale, 16.f * scale));
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);
        if (clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto &selector : m_triangle_selectors) {
                selector->seed_fill_unselect_all_triangles();
                selector->request_update_render_data();
            }
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }

    const float label_w  = std::max(m_imgui->calc_text_size(m_desc["smart_fill_angle"]).x, m_imgui->calc_text_size(m_desc["cursor_size"]).x) +
                          m_imgui->scaled(1.f);
    const float slider_w = m_imgui->scaled(7.f);
    if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["smart_fill_angle"]);
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(slider_w);
        if (m_imgui->bbl_slider_float_style("##cs_smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f", 1.0f, true))
            for (auto &selector : m_triangle_selectors) {
                selector->seed_fill_unselect_all_triangles();
                selector->request_update_render_data();
            }
        ImGui::PopItemWidth();
    } else {
        m_cursor_type = m_current_tool == ImGui::CircleButtonIcon ? TriangleSelector::CursorType::CIRCLE : TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
        if (m_current_tool == ImGui::CircleButtonIcon) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc["cursor_size"]);
            ImGui::SameLine(label_w);
            ImGui::PushItemWidth(slider_w);
            m_imgui->bbl_slider_float_style("##cs_cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
            ImGui::PopItemWidth();
        }
    }
}

void GLGizmoColorStitchPainter::render_recipe_list()
{
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    m_imgui->bold_text(_u8L("Recipes"));
    const float swatch = ImGui::GetFrameHeight();
    if (mgr.size() == 0)
        m_imgui->text_wrapped(_L("No saved recipe yet. Create one, or capture the Sandwich settings of this object."), m_imgui->scaled(16.f));

    ImGui::BeginChild("##cs_recipe_list", ImVec2(m_imgui->scaled(16.f), std::min(float(mgr.size()), 6.f) * (swatch + ImGui::GetStyle().ItemSpacing.y) + 2.f),
                      false);
    int clicked = 0;
    for (const SurfaceEffectProfile &p : mgr.list()) {
        ImGui::PushID(p.id);
        const uint32_t argb = p.preview_argb != 0 ? p.preview_argb : preview_argb(SurfacePassStack::from_json(p.stack_top_json), &p);
        ImGui::ColorButton("##sw", argb_to_imvec(argb), ImGuiColorEditFlags_NoTooltip, ImVec2(swatch, swatch));
        ImGui::SameLine();
        if (ImGui::Selectable(p.name.c_str(), p.id == m_active_profile, 0, ImVec2(0.f, swatch)))
            clicked = p.id;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (clicked != 0 && clicked != m_active_profile) {
        m_active_profile = clicked;
        m_active_slot    = 0;
        m_status.clear();
    }

    if (m_imgui->button(_L("New")))
        m_active_profile = new_recipe();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("A new recipe with one Solid pass."), m_imgui->scaled(16.f));
    ImGui::SameLine();
    if (m_imgui->button(_L("From object"))) {
        if (const int id = capture_recipe_from_object(); id > 0)
            m_active_profile = id;
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Saves the Sandwich / ColorStitch / PathBlend settings of this object (Surface colour page) as a recipe."),
                         m_imgui->scaled(16.f));
    m_imgui->disabled_begin(mgr.find(m_active_profile) == nullptr);
    ImGui::SameLine();
    if (m_imgui->button(_L("Duplicate"))) {
        if (const int id = duplicate_recipe(m_active_profile); id > 0)
            m_active_profile = id;
    }
    ImGui::SameLine();
    const int to_delete = m_imgui->button(_L("Delete")) ? m_active_profile : 0;
    m_imgui->disabled_end();
    if (to_delete > 0)
        delete_recipe(to_delete);
    m_active_slot = (m_active_profile > 0) ? m_active_slot : 0;
}

void GLGizmoColorStitchPainter::render_recipe_editor()
{
    SurfaceEffectProfile *profile = SurfaceEffectProfileManager::get().find_mut(m_active_profile);
    if (profile == nullptr)
        return;
    const float width = m_imgui->scaled(16.f);

    // Name.
    if (m_name_buf_profile != profile->id) {
        std::snprintf(m_name_buf, sizeof(m_name_buf), "%s", profile->name.c_str());
        m_name_buf_profile = profile->id;
    }
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Name"));
    ImGui::SameLine();
    ImGui::PushItemWidth(width - m_imgui->scaled(3.f));
    ImGui::InputText("##cs_name", m_name_buf, sizeof(m_name_buf));
    ImGui::PopItemWidth();
    if (ImGui::IsItemDeactivatedAfterEdit() && std::strlen(m_name_buf) > 0) {
        profile->name = m_name_buf;
        m_reslice_pending = true; // marks the project modified, nothing to re-slice
    }

    SurfacePassStack stack = SurfacePassStack::from_json(profile->stack_top_json);
    if (stack.passes.empty() || !stack.any_effect()) {
        stack.passes.clear();
        stack.enabled = true;
    }

    // Filament choices.
    const int                n_fil = std::max(1, int(m_filament_colors.size()));
    std::vector<std::string> fil_items;
    for (int i = 1; i <= n_fil; ++i)
        fil_items.push_back(std::to_string(i));
    std::vector<std::string> solid_items { _u8L("Object") };
    solid_items.insert(solid_items.end(), fil_items.begin(), fil_items.end());
    const std::vector<std::string> kind_items { _u8L("Natural"), _u8L("Solid"), _u8L("ColorStitch"), _u8L("PathBlend") };
    const std::vector<std::string> cs_modes { "blend2", "stripes", "weave" };
    const std::vector<std::string> cs_mode_items { _u8L("Blend"), _u8L("Stripes"), _u8L("Weave") };

    m_imgui->bold_text(_u8L("Top surface passes (bottom to top)"));
    bool   changed = false, finished = false;
    int    remove  = -1;
    double sum     = 0.;
    for (const SurfacePass &p : stack.passes)
        sum += std::max(0., p.ratio);
    const float combo_w = m_imgui->scaled(5.5f);
    const float small_w = m_imgui->scaled(3.f);
    for (size_t i = 0; i < stack.passes.size(); ++i) {
        SurfacePass &pass = stack.passes[i];
        ImGui::PushID(int(i));
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        m_imgui->text("#" + std::to_string(i + 1));
        ImGui::SameLine();
        int kind = std::clamp(int(pass.kind), 0, 3);
        if (combo_items("##kind", kind, kind_items, combo_w)) {
            pass.kind = SurfacePassKind(kind);
            if (pass.kind != SurfacePassKind::ColorStitch)
                pass.colorstitch = SurfaceEffectPayload();
            if (pass.kind != SurfacePassKind::PathBlend)
                pass.pathblend = SurfaceEffectPayload();
            if (pass.kind == SurfacePassKind::ColorStitch) {
                pass.colorstitch.present = true;
                pass.colorstitch.kv["interlayer_colormix_filament_a"] = "1";
                pass.colorstitch.kv["interlayer_colormix_filament_b"] = std::to_string(std::min(2, n_fil));
                pass.colorstitch.kv["interlayer_colormix_mode"]       = "blend2";
            } else if (pass.kind == SurfacePassKind::PathBlend) {
                PathBlendPassConfig pb;
                pb.tool_bottom = 0;
                pb.tool_top    = std::min(1, n_fil - 1);
                set_pathblend(pass, pb);
            }
            changed = finished = true;
        }
        ImGui::SameLine();
        if (m_imgui->button(_L("Remove")))
            remove = int(i);

        switch (pass.kind) {
        case SurfacePassKind::Solid: {
            int sel = pass.solid_tool < 0 ? 0 : pass.solid_tool + 1;
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_L("Filament"));
            ImGui::SameLine();
            if (combo_items("##solid_tool", sel, solid_items, small_w)) {
                pass.solid_tool = sel - 1;
                changed = finished = true;
            }
            break;
        }
        case SurfacePassKind::ColorStitch: {
            const std::pair<int, int> t = colorstitch_tools(pass, profile);
            int a = std::clamp(t.first, 0, n_fil - 1), b = std::clamp(t.second, 0, n_fil - 1);
            ImGui::AlignTextToFramePadding();
            m_imgui->text("A");
            ImGui::SameLine();
            if (combo_items("##cs_a", a, fil_items, small_w)) {
                pass.colorstitch.present = true;
                pass.colorstitch.kv["interlayer_colormix_filament_a"] = std::to_string(a + 1);
                changed = finished = true;
            }
            ImGui::SameLine();
            m_imgui->text("B");
            ImGui::SameLine();
            if (combo_items("##cs_b", b, fil_items, small_w)) {
                pass.colorstitch.present = true;
                pass.colorstitch.kv["interlayer_colormix_filament_b"] = std::to_string(b + 1);
                changed = finished = true;
            }
            int  mode = 0;
            auto mit  = pass.colorstitch.kv.find("interlayer_colormix_mode");
            if (mit != pass.colorstitch.kv.end())
                for (size_t m = 0; m < cs_modes.size(); ++m)
                    if (cs_modes[m] == mit->second)
                        mode = int(m);
            ImGui::SameLine();
            if (combo_items("##cs_mode", mode, cs_mode_items, combo_w)) {
                pass.colorstitch.present = true;
                pass.colorstitch.kv["interlayer_colormix_mode"] = cs_modes[size_t(mode)];
                changed = finished = true;
            }
            break;
        }
        case SurfacePassKind::PathBlend: {
            PathBlendPassConfig pb   = pathblend_of(pass);
            int                 ramp = std::clamp(pb.tool_bottom, 0, n_fil - 1), cap = std::clamp(pb.tool_top, 0, n_fil - 1);
            bool                full = pb.mode == PathBlendPassConfig::Mode::Full;
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_L("Ramp"));
            ImGui::SameLine();
            bool pb_changed = combo_items("##pb_ramp", ramp, fil_items, small_w);
            ImGui::SameLine();
            pb_changed |= m_imgui->bbl_checkbox(_L("Cap"), full);
            if (full) {
                ImGui::SameLine();
                pb_changed |= combo_items("##pb_cap", cap, fil_items, small_w);
            }
            if (pb_changed) {
                pb.tool_bottom = ramp;
                pb.tool_top    = cap;
                pb.mode        = full ? PathBlendPassConfig::Mode::Full : PathBlendPassConfig::Mode::Half;
                set_pathblend(pass, pb);
                changed = finished = true;
            }
            break;
        }
        default: break;
        }

        if (pass.kind != SurfacePassKind::PathBlend) {
            int pct = sum > 0. ? int(std::lround(100. * std::max(0., pass.ratio) / sum)) : 100;
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_L("Share"));
            ImGui::SameLine();
            ImGui::PushItemWidth(small_w);
            if (ImGui::DragInt("##share", &pct, 1.f, 5, 100, "%d%%")) {
                pass.ratio = std::clamp(pct, 5, 100) / 100. * std::max(sum, 1e-6);
                changed    = true;
            }
            finished |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopItemWidth();
            ImGui::SameLine();
        }
        int angle = pass.kind == SurfacePassKind::PathBlend ? pathblend_of(pass).fill_angle : pass.angle;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Angle"));
        ImGui::SameLine();
        ImGui::PushItemWidth(small_w);
        if (ImGui::DragInt("##angle", &angle, 1.f, -1, 359, angle < 0 ? "auto" : "%d")) {
            angle = std::clamp(angle, -1, 359);
            if (pass.kind == SurfacePassKind::PathBlend) {
                PathBlendPassConfig pb = pathblend_of(pass);
                pb.fill_angle          = angle;
                set_pathblend(pass, pb);
            } else {
                pass.angle = angle;
                if (pass.kind == SurfacePassKind::ColorStitch) {
                    pass.colorstitch.present = true;
                    pass.colorstitch.kv["interlayer_colormix_angle"] = std::to_string(angle);
                }
            }
            changed = true;
        }
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopItemWidth();
        ImGui::PopID();
    }
    if (remove >= 0) {
        stack.passes.erase(stack.passes.begin() + remove);
        changed = finished = true;
    }

    const bool has_pathblend = std::any_of(stack.passes.begin(), stack.passes.end(), [](const SurfacePass &p) { return p.kind == SurfacePassKind::PathBlend; });
    ImGui::Separator();
    m_imgui->disabled_begin(int(stack.passes.size()) >= SurfacePassStack::kMaxPasses || has_pathblend);
    if (m_imgui->button(_L("Add pass"))) {
        SurfacePass pass;
        pass.kind       = SurfacePassKind::Solid;
        pass.ratio      = stack.passes.empty() ? 1.0 : sum / double(stack.passes.size());
        pass.solid_tool = -1;
        stack.passes.push_back(pass);
        changed = finished = true;
    }
    m_imgui->disabled_end();
    if (has_pathblend && stack.passes.size() > 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_WARNING);
        m_imgui->text_wrapped(_L("A PathBlend pass takes the whole layer: the other passes are not printed."), width);
        ImGui::PopStyleColor();
    }
    if (changed) {
        stack.enabled = true;
        commit_stack(stack, finished);
    }
}

void GLGizmoColorStitchPainter::render_slots_in_use()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    const std::vector<bool> &used = used_states();
    std::vector<std::pair<int, int>> rows; // slot, profile id
    for (int s = 1; s < kSlots; ++s) {
        if (!used[size_t(s)])
            continue;
        int pid = 0;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part() && (pid = ColorStitchPaint::profile_for_slot(*mv, s)) > 0)
                break;
        rows.emplace_back(s, pid);
    }
    if (rows.empty())
        return;
    ImGui::Separator();
    m_imgui->bold_text(_u8L("In use on this object"));
    const SurfaceEffectProfileManager &mgr    = SurfaceEffectProfileManager::get();
    const float                        swatch = ImGui::GetFrameHeight();
    int                                to_use = 0, to_free = 0;
    for (const std::pair<int, int> &row : rows) {
        ImGui::PushID(row.first);
        const SurfaceEffectProfile *p = mgr.find(row.second);
        ImGui::ColorButton("##sw", p != nullptr ? argb_to_imvec(p->preview_argb) : ImVec4(0.25f, 0.25f, 0.25f, 1.f), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(swatch, swatch));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(p != nullptr ? p->name : _u8L("Missing recipe"));
        ImGui::SameLine();
        if (p != nullptr && m_imgui->button(_L("Use")))
            to_use = row.second;
        if (p != nullptr)
            ImGui::SameLine();
        if (m_imgui->button(_L("Free")))
            to_free = row.first;
        ImGui::PopID();
    }
    if (to_use > 0) {
        m_active_profile = to_use;
        m_active_slot    = 0;
    }
    if (to_free > 0)
        free_slot(to_free);
}

void GLGizmoColorStitchPainter::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    if (m_filament_colors.empty())
        m_filament_colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();

    const float approx_height = m_imgui->scaled(22.f);
    y = std::min(y, bottom_limit - approx_height);
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    const float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;
    render_tools(max_tooltip_width);
    ImGui::Separator();
    render_recipe_list();
    ImGui::Separator();
    render_recipe_editor();
    render_slots_in_use();

    ImGui::Separator();
    m_imgui->text_wrapped(_L("Click a face to paint the active recipe. Shift + click erases. Recipes print on top surfaces only."),
                          m_imgui->scaled(16.f));
    if (!m_status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_WARNING);
        m_imgui->text_wrapped(m_status, m_imgui->scaled(16.f));
        ImGui::PopStyleColor();
    }

    bool painted = false;
    for (const ModelVolume *mv : mo->volumes)
        painted |= mv->is_model_part() && !mv->colorstitch_paint_facets.empty();
    m_imgui->disabled_begin(!painted);
    if (m_imgui->button(m_desc.at("remove_all")))
        erase_all();
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (m_imgui->button(_L("Done")))
        m_parent.reset_all_gizmos();

    // A drag on a number finished outside of its widget: re-slice once nothing is being edited.
    if (m_reslice_pending && !ImGui::IsAnyItemActive())
        schedule_reslice();

    ImGui::PopStyleVar(1);
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
