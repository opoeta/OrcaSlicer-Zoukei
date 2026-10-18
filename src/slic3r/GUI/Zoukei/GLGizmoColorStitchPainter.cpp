// Orca Zoukei: ColorStitch Painter gizmo, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// See GLGizmoColorStitchPainter.hpp.
#include "GLGizmoColorStitchPainter.hpp"

#include "libslic3r/ColorStitch.hpp"
#include "libslic3r/ColorStitchPaint.hpp"
#include "libslic3r/ColorStitchSticker.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Tesselate.hpp"
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

#include <boost/filesystem.hpp>
#include <glad/gl.h>
#include <imgui.h>
#include <wx/busycursor.h>
#include <wx/filedlg.h>

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

// The stack of a zone (0 Top, 1 Penultimate, 2 Bottom) is stored in one of three JSON strings of the recipe.
std::string SurfaceEffectProfile::*zone_field(int zone)
{
    return zone == 1 ? &SurfaceEffectProfile::stack_penu_json : (zone == 2 ? &SurfaceEffectProfile::stack_bottom_json : &SurfaceEffectProfile::stack_top_json);
}

// Pose of a sticker in the world frame (the frame of the bed): position of its centre on the face, spin
// about the vertical, size factor, mirror and the side (down = on the bottom face, seen from below).
struct StickerPose
{
    Vec3d  pos    = Vec3d::Zero();
    double spin   = 0.;
    double scale  = 1.;
    bool   mirror = false;
    bool   down   = false;
};

Transform3d compose_pose(const StickerPose &p)
{
    Eigen::Matrix3d L = Eigen::AngleAxisd(p.spin, Vec3d::UnitZ()).toRotationMatrix();
    if (p.mirror)
        L = L * Eigen::Vector3d(-1., 1., 1.).asDiagonal();
    L *= std::max(1e-6, p.scale);
    if (p.down)
        L = Eigen::Vector3d(1., -1., -1.).asDiagonal() * L; // a half turn about X: the drawing reads from below
    Transform3d t   = Transform3d::Identity();
    t.linear()      = L;
    t.translation() = p.pos;
    return t;
}

StickerPose decompose_pose(const Transform3d &t)
{
    StickerPose p;
    p.pos             = t.translation();
    Eigen::Matrix3d L = t.linear();
    p.down            = L.col(2).z() < 0.;
    if (p.down)
        L = Eigen::Vector3d(1., -1., -1.).asDiagonal() * L;
    Eigen::Matrix2d A = L.topLeftCorner<2, 2>();
    p.mirror          = A.determinant() < 0.;
    if (p.mirror)
        A.col(0) = -A.col(0);
    p.scale = std::sqrt(std::max(1e-12, std::abs(A.determinant())));
    p.spin  = std::atan2(A(1, 0), A(0, 0));
    return p;
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
    m_active_slot           = 0;
    m_sticker_sel           = -1;
    m_sticker_placing       = false;
    m_sticker_preview_dirty = true;
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
    render_sticker_preview();
    if (!m_sticker_placing)
        render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

bool GLGizmoColorStitchPainter::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    // Sticker placing mode: a click on a face puts the selected sticker there, nothing is painted.
    if (m_sticker_placing) {
        const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (action == SLAGizmoEventType::LeftDown) {
            if (mo != nullptr && m_sticker_sel >= 0 && size_t(m_sticker_sel) < mo->colorstitch_stickers.size()) {
                if (place_sticker_at_mouse(size_t(m_sticker_sel), mouse_position))
                    m_sticker_placing = false;
            }
            return true;
        }
        if (action == SLAGizmoEventType::RightDown) {
            m_sticker_placing = false;
            return true;
        }
        if (action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Dragging)
            return true;
    }
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
            colors[size_t(s)] = argb_to_color(swatch_argb(*p));
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
    m_active_slot           = 0;
    m_used_states_dirty     = true;
    m_sticker_preview_dirty = true;
    if (mo == nullptr || m_sticker_sel >= int(mo->colorstitch_stickers.size()))
        m_sticker_sel = -1;
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
    const SurfacePassStack stack  = SurfacePassStack::resolve(object_cfg);
    const SurfacePassStack penu   = SurfacePassStack::resolve_zone(object_cfg, SurfaceZone::Penultimate);
    const SurfacePassStack bottom = SurfacePassStack::resolve_zone(object_cfg, SurfaceZone::Bottom);
    if (!stack.any_effect() && !penu.any_effect() && !bottom.any_effect()) {
        m_status = _u8L("This object has no Sandwich, ColorStitch or PathBlend settings to capture (Surface colour page).");
        return 0;
    }
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    SurfaceEffectProfile         p;
    p.name              = mo->name + " " + std::to_string(mgr.next_id());
    p.stack_top_json    = stack.to_json();
    p.stack_penu_json   = penu.any_effect() ? penu.to_json() : std::string();
    p.stack_bottom_json = bottom.any_effect() ? bottom.to_json() : std::string();
    p.colorstitch       = SurfaceEffectProfileManager::snapshot_keys(cfg, SurfaceEffectProfileManager::colorstitch_keys());
    p.pathblend         = SurfaceEffectProfileManager::snapshot_keys(cfg, SurfaceEffectProfileManager::pathblend_keys());
    p.multipass         = SurfaceEffectProfileManager::snapshot_keys(cfg, SurfaceEffectProfileManager::multipass_keys());
    p.preview_argb      = swatch_argb(p);
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

uint32_t GLGizmoColorStitchPainter::swatch_argb(const SurfaceEffectProfile &p) const
{
    if (p.preview_argb != 0)
        return p.preview_argb;
    for (const std::string *json : { &p.stack_top_json, &p.stack_penu_json, &p.stack_bottom_json }) {
        const SurfacePassStack s = SurfacePassStack::from_json(*json);
        if (s.any_effect())
            return preview_argb(s, &p);
    }
    return 0xFF808080u;
}

void GLGizmoColorStitchPainter::commit_stack(const SurfacePassStack &stack, int zone, bool reslice)
{
    SurfaceEffectProfile *p = SurfaceEffectProfileManager::get().find_mut(m_active_profile);
    if (p == nullptr)
        return;
    p->*zone_field(zone) = stack.to_json();
    // The swatch shows the Top zone while it prints something, else the zone just edited.
    const SurfacePassStack top = zone == 0 ? stack : SurfacePassStack::from_json(p->stack_top_json);
    p->preview_argb            = preview_argb(top.any_effect() ? top : stack, p);
    m_sticker_preview_dirty    = true;
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
        const uint32_t argb = swatch_argb(p);
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

    // Zone selector. A zone that prints something is marked.
    {
        std::vector<std::string> zone_items { _u8L("Top"), _u8L("Penultimate"), _u8L("Bottom") };
        for (int z = 0; z < 3; ++z)
            if (SurfacePassStack::from_json(profile->*zone_field(z)).any_effect())
                zone_items[size_t(z)] += " *";
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Zone"));
        ImGui::SameLine();
        combo_items("##cs_zone", m_zone, zone_items, m_imgui->scaled(7.f));
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Top: the visible top surface. Penultimate: the solid layer under a top surface, only where a top covers it. "
                                "Bottom: the bottom surface on the bed or on supports (paint it from below). * = the zone prints something."),
                             m_imgui->scaled(16.f));
        if (m_zone == 1)
            m_imgui->text_wrapped(_L("Needs a top surface with a solid layer under it (top shell layers 2 or more) and a top larger than 3 mm."),
                                  m_imgui->scaled(16.f));
        else if (m_zone == 2)
            m_imgui->text_wrapped(_L("Pass #1 touches the bed. Bridges are not changed and PathBlend is not available here."), m_imgui->scaled(16.f));
    }
    const int zone = m_zone;

    SurfacePassStack stack = SurfacePassStack::from_json(profile->*zone_field(zone));
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
    std::vector<std::string> kind_items { _u8L("Natural"), _u8L("Solid"), _u8L("ColorStitch"), _u8L("PathBlend") };
    if (zone == 2)
        kind_items.pop_back(); // no PathBlend in the Bottom zone
    const std::vector<std::string> cs_modes { "blend2", "stripes", "weave" };
    const std::vector<std::string> cs_mode_items { _u8L("Blend"), _u8L("Stripes"), _u8L("Weave") };

    m_imgui->bold_text(zone == 0 ? _u8L("Top surface passes (bottom to top)") :
                       zone == 1 ? _u8L("Penultimate layer passes (bottom to top)") : _u8L("Bottom surface passes (from the bed up)"));
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
    // (a Bottom stack never prints a PathBlend pass: the combo does not offer it)
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
        commit_stack(stack, zone, finished);
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

const ModelInstance *GLGizmoColorStitchPainter::selected_instance() const
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return nullptr;
    const int idx = m_parent.get_selection().get_instance_idx();
    return (idx >= 0 && size_t(idx) < mo->instances.size()) ? mo->instances[size_t(idx)] : nullptr;
}

int GLGizmoColorStitchPainter::nearest_filament(uint32_t rgb) const
{
    int    best   = 0;
    double best_d = 1e30;
    for (size_t i = 0; i < m_filament_colors.size(); ++i) {
        ColorRGBA c(0.5f, 0.5f, 0.5f, 1.f);
        decode_color(m_filament_colors[i], c);
        const double dr = double(c.r()) * 255. - double((rgb >> 16) & 0xFFu);
        const double dg = double(c.g()) * 255. - double((rgb >> 8) & 0xFFu);
        const double db = double(c.b()) * 255. - double(rgb & 0xFFu);
        const double d  = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best   = int(i);
        }
    }
    return best;
}

int GLGizmoColorStitchPainter::flat_recipe_for_filament(int filament0)
{
    SurfaceEffectProfileManager &mgr  = SurfaceEffectProfileManager::get();
    const std::string            name = _u8L("Flat") + " F" + std::to_string(filament0 + 1);
    for (const SurfaceEffectProfile &p : mgr.list())
        if (p.name == name)
            return p.id;
    SurfacePassStack stack;
    stack.enabled = true;
    SurfacePass pass;
    pass.kind       = SurfacePassKind::Solid;
    pass.ratio      = 1.0;
    pass.solid_tool = filament0;
    stack.passes.push_back(pass);
    SurfaceEffectProfile p;
    p.name              = name;
    p.stack_top_json    = stack.to_json();
    p.stack_bottom_json = stack.to_json(); // the same recipe serves stickers on bottom faces
    p.auto_generated    = true;
    p.preview_argb      = preview_argb(stack, nullptr);
    return mgr.add(std::move(p));
}

void GLGizmoColorStitchPainter::commit_stickers(const std::string &snapshot_name)
{
    m_sticker_preview_dirty = true;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), snapshot_name, UndoRedo::SnapshotType::GizmoAction);
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}

void GLGizmoColorStitchPainter::load_sticker()
{
    ModelObject         *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const ModelInstance *mi = selected_instance();
    if (mo == nullptr || mi == nullptr)
        return;
    wxFileDialog dlg(wxGetApp().plater(), _L("Choose an SVG file"), wxEmptyString, wxEmptyString, "SVG files (*.svg)|*.svg;*.SVG",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = into_u8(dlg.GetPath());
    const std::unique_ptr<std::string> text = read_from_disk(path);
    if (!text || text->empty()) {
        m_status = _u8L("Cannot read that file.");
        return;
    }
    const std::vector<uint32_t> colors = ColorStitchStickerTools::svg_colors(*text);
    const Vec2d                 size   = ColorStitchStickerTools::svg_size_mm(*text);
    if (colors.empty() || size.x() <= 0. || size.y() <= 0.) {
        m_status = _u8L("That SVG has no shape the sticker can use.");
        return;
    }
    if (m_filament_colors.empty())
        m_filament_colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();

    ColorStitchSticker st;
    st.name     = boost::filesystem::path(path).stem().string();
    st.svg_data = *text;
    for (uint32_t rgb : colors) {
        ColorStitchStickerColor col;
        col.rgb        = rgb;
        col.profile_id = flat_recipe_for_filament(nearest_filament(rgb));
        st.colors.push_back(col);
    }
    // Start on the top of the object, centred, 60% of its smaller side.
    const BoundingBoxf3 bb = mo->instance_bounding_box(size_t(m_parent.get_selection().get_instance_idx()));
    StickerPose         pose;
    pose.pos   = Vec3d(bb.center().x(), bb.center().y(), bb.max.z());
    pose.scale = std::max(1e-3, 0.6 * std::min(bb.size().x(), bb.size().y()) / std::max(size.x(), size.y()));
    st.transform = mi->get_transformation().get_matrix().inverse() * compose_pose(pose);

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add ColorStitch sticker"), UndoRedo::SnapshotType::GizmoAction);
    mo->colorstitch_stickers.push_back(std::move(st));
    m_sticker_sel           = int(mo->colorstitch_stickers.size()) - 1;
    m_sticker_preview_dirty = true;
    m_status                = _u8L("Sticker added on the top. Use \"Place on a face\" and click a flat top or bottom face to move it.");
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}

bool GLGizmoColorStitchPainter::place_sticker_at_mouse(size_t index, const Vec2d &mouse_position)
{
    ModelObject         *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const ModelInstance *mi = selected_instance();
    if (mo == nullptr || mi == nullptr || index >= mo->colorstitch_stickers.size())
        return false;
    update_raycast_cache(mouse_position, wxGetApp().plater()->get_camera(), mesh_trafo_matrices());
    if (m_rr.mesh_id < 0) {
        m_status = _u8L("Click on the model.");
        return false;
    }
    const ModelVolume *mv  = nullptr;
    int                idx = -1;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part() && ++idx == m_rr.mesh_id) {
            mv = v;
            break;
        }
    if (mv == nullptr)
        return false;
    const Transform3d inst = mi->get_transformation().get_matrix();
    const Transform3d vt   = inst * mv->get_matrix();
    const Vec3d       hit  = vt * m_rr.hit.cast<double>();
    const Vec3f       nf   = its_face_normal(mv->mesh().its, int(m_rr.facet));
    const Vec3d       n    = (vt.linear().inverse().transpose() * nf.cast<double>()).normalized();
    if (std::abs(n.z()) < 0.7) {
        m_status = _u8L("Stickers go on flat top or bottom faces: pick a horizontal face.");
        return false;
    }
    ColorStitchSticker &st   = mo->colorstitch_stickers[index];
    StickerPose         pose = decompose_pose(inst * st.transform);
    pose.pos                 = hit;
    pose.down                = n.z() < 0.;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Place ColorStitch sticker"), UndoRedo::SnapshotType::GizmoAction);
    st.transform            = inst.inverse() * compose_pose(pose);
    m_sticker_preview_dirty = true;
    m_status.clear();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
    return true;
}

void GLGizmoColorStitchPainter::rebuild_sticker_preview()
{
    m_sticker_preview.clear();
    m_sticker_preview_dirty = false;
    const ModelObject   *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const ModelInstance *mi = selected_instance();
    if (mo == nullptr || mi == nullptr)
        return;
    const SurfaceEffectProfileManager &mgr  = SurfaceEffectProfileManager::get();
    const Transform3d                  inst = mi->get_transformation().get_matrix();
    m_sticker_preview_inst                  = inst;
    for (const ColorStitchSticker &st : mo->colorstitch_stickers) {
        if (st.hidden || st.svg_data.empty())
            continue;
        const Transform3d to_world = inst * st.transform;
        const double      z        = to_world.translation().z() + (st.faces_down() || to_world.linear().col(2).z() < 0. ? -0.03 : 0.03);
        for (const ColorStitchStickerTools::ColorMask &mask : ColorStitchStickerTools::color_masks(st, to_world)) {
            int pid = st.profile_id;
            if (mask.color_index >= 0 && size_t(mask.color_index) < st.colors.size()) {
                const ColorStitchStickerColor &col = st.colors[size_t(mask.color_index)];
                pid = col.hidden ? 0 : col.profile_id;
            }
            const SurfaceEffectProfile *p = pid > 0 ? mgr.find(pid) : nullptr;
            if (p == nullptr)
                continue;
            const std::vector<Vec3d> tris = triangulate_expolygons_3d(mask.area, z, false);
            if (tris.size() < 3)
                continue;
            indexed_triangle_set its;
            its.vertices.reserve(tris.size());
            for (const Vec3d &v : tris)
                its.vertices.emplace_back(v.cast<float>());
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                its.indices.emplace_back(int(i), int(i + 1), int(i + 2));
                its.indices.emplace_back(int(i), int(i + 2), int(i + 1)); // both windings: seen from above and from below
            }
            StickerPreviewPart part;
            part.model = std::make_shared<GLModel>();
            part.model->init_from(its);
            part.color = argb_to_color(swatch_argb(*p));
            part.color.a(0.9f);
            m_sticker_preview.push_back(std::move(part));
        }
    }
}

void GLGizmoColorStitchPainter::render_sticker_preview()
{
    const ModelObject   *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const ModelInstance *mi = selected_instance();
    if (mo == nullptr || mi == nullptr || mo->colorstitch_stickers.empty())
        return;
    if (m_sticker_preview_dirty || m_sticker_preview_inst.matrix() != mi->get_transformation().get_matrix().matrix())
        rebuild_sticker_preview();
    if (m_sticker_preview.empty())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    for (StickerPreviewPart &part : m_sticker_preview) {
        part.model->set_color(part.color);
        part.model->render();
    }
    shader->stop_using();
}

void GLGizmoColorStitchPainter::render_stickers()
{
    ModelObject         *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const ModelInstance *mi = selected_instance();
    if (mo == nullptr || mi == nullptr)
        return;
    const float width = m_imgui->scaled(16.f);
    m_imgui->bold_text(_u8L("SVG stickers"));
    if (m_imgui->button(_L("Load SVG...")))
        load_sticker();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("A drawing placed on a flat top or bottom face. Every colour of the SVG prints with a recipe of its own (flat filament "
                            "recipes by default). Rough: flat colours only, no gradients."),
                         width);
    std::vector<ColorStitchSticker> &pile = mo->colorstitch_stickers;
    if (pile.empty())
        return;

    int to_delete = -1, to_swap = -1;
    for (size_t i = 0; i < pile.size(); ++i) {
        ImGui::PushID(int(i) + 1000);
        bool shown = !pile[i].hidden;
        if (ImGui::Checkbox("##show", &shown)) {
            pile[i].hidden = !shown;
            commit_stickers(_u8L("Show or hide ColorStitch sticker"));
        }
        ImGui::SameLine();
        if (ImGui::Selectable(pile[i].name.c_str(), int(i) == m_sticker_sel))
            m_sticker_sel = int(i);
        ImGui::PopID();
    }
    if (m_sticker_sel < 0 || size_t(m_sticker_sel) >= pile.size())
        return;

    ColorStitchSticker &st   = pile[size_t(m_sticker_sel)];
    const Transform3d   inst = mi->get_transformation().get_matrix();
    StickerPose         pose = decompose_pose(inst * st.transform);
    const Vec2d         size = ColorStitchStickerTools::svg_size_mm(st.svg_data);
    const BoundingBoxf3 bb   = mo->instance_bounding_box(size_t(m_parent.get_selection().get_instance_idx()));
    ImGui::Separator();

    bool edited = false, finished = false;
    float x = float(pose.pos.x()), y = float(pose.pos.y()), spin = float(Geometry::rad2deg(pose.spin));
    float w = float(size.x() * pose.scale);
    ImGui::PushItemWidth(m_imgui->scaled(5.f));
    ImGui::AlignTextToFramePadding();
    m_imgui->text("X");
    ImGui::SameLine();
    edited |= ImGui::DragFloat("##st_x", &x, 0.2f, 0.f, 0.f, "%.2f");
    finished |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    m_imgui->text("Y");
    ImGui::SameLine();
    edited |= ImGui::DragFloat("##st_y", &y, 0.2f, 0.f, 0.f, "%.2f");
    finished |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Rotation"));
    ImGui::SameLine();
    edited |= ImGui::DragFloat("##st_spin", &spin, 1.f, -360.f, 360.f, "%.0f");
    finished |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Width (mm)"));
    ImGui::SameLine();
    edited |= ImGui::DragFloat("##st_w", &w, 0.2f, 0.5f, 1000.f, "%.1f");
    finished |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopItemWidth();
    bool mirror = pose.mirror;
    if (m_imgui->bbl_checkbox(_L("Mirror"), mirror)) {
        pose.mirror = mirror;
        edited = finished = true;
    }
    ImGui::SameLine();
    int side = pose.down ? 1 : 0;
    if (combo_items("##st_side", side, { _u8L("Top"), _u8L("Bottom") }, m_imgui->scaled(5.f))) {
        pose.down  = side == 1;
        pose.pos.z() = pose.down ? bb.min.z() : bb.max.z();
        edited = finished = true;
    }
    if (edited) {
        pose.pos.x() = x;
        pose.pos.y() = y;
        pose.spin    = Geometry::deg2rad(double(spin));
        if (size.x() > 0.)
            pose.scale = std::max(1e-3, double(w) / size.x());
        st.transform            = inst.inverse() * compose_pose(pose);
        m_sticker_preview_dirty = true;
        m_reslice_pending       = true;
    }
    if (finished) {
        m_reslice_pending = false;
        commit_stickers(_u8L("Edit ColorStitch sticker"));
    }

    if (m_imgui->button(m_sticker_placing ? _L("Click a face...") : _L("Place on a face")))
        m_sticker_placing = !m_sticker_placing;
    ImGui::SameLine();
    if (m_imgui->button(_L("Up")) && size_t(m_sticker_sel) + 1 < pile.size())
        to_swap = m_sticker_sel;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Moves the sticker up the pile: the topmost sticker wins where they overlap."), width);
    ImGui::SameLine();
    if (m_imgui->button(_L("Delete")))
        to_delete = m_sticker_sel;

    // Recipe of every colour of the drawing.
    const SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    std::vector<std::string>           names { _u8L("(not printed)") };
    std::vector<int>                   ids { 0 };
    for (const SurfaceEffectProfile &p : mgr.list()) {
        names.push_back(p.name);
        ids.push_back(p.id);
    }
    bool colors_changed = false;
    for (size_t c = 0; c < st.colors.size(); ++c) {
        ColorStitchStickerColor &col = st.colors[c];
        ImGui::PushID(int(c) + 2000);
        ImGui::ColorButton("##rgb", ImVec4(float((col.rgb >> 16) & 0xFFu) / 255.f, float((col.rgb >> 8) & 0xFFu) / 255.f, float(col.rgb & 0xFFu) / 255.f, 1.f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        int sel = 0;
        for (size_t k = 0; k < ids.size(); ++k)
            if (ids[k] == col.profile_id)
                sel = int(k);
        if (combo_items("##rec", sel, names, m_imgui->scaled(8.f))) {
            col.profile_id = ids[size_t(sel)];
            colors_changed = true;
        }
        ImGui::SameLine();
        bool shown = !col.hidden;
        if (ImGui::Checkbox("##on", &shown)) {
            col.hidden     = !shown;
            colors_changed = true;
        }
        ImGui::PopID();
    }
    if (colors_changed)
        commit_stickers(_u8L("Edit ColorStitch sticker colours"));
    const bool needs_bottom = pose.down;
    for (const ColorStitchStickerColor &col : st.colors)
        if (const SurfaceEffectProfile *p = mgr.find(col.profile_id))
            if (needs_bottom && !SurfacePassStack::from_json(p->stack_bottom_json).any_effect()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_WARNING);
                m_imgui->text_wrapped(_L("A sticker on a bottom face needs recipes with a Bottom zone."), width);
                ImGui::PopStyleColor();
                break;
            }

    if (to_swap >= 0) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reorder ColorStitch stickers"), UndoRedo::SnapshotType::GizmoAction);
        std::swap(pile[size_t(to_swap)], pile[size_t(to_swap) + 1]);
        m_sticker_sel           = to_swap + 1;
        m_sticker_preview_dirty = true;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
    if (to_delete >= 0) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete ColorStitch sticker"), UndoRedo::SnapshotType::GizmoAction);
        pile.erase(pile.begin() + to_delete);
        m_sticker_sel           = -1;
        m_sticker_placing       = false;
        m_sticker_preview_dirty = true;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
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
    render_stickers();

    ImGui::Separator();
    m_imgui->text_wrapped(_L("Click a face to paint the active recipe. Shift + click erases. Top and Penultimate zones print on top surfaces, "
                             "the Bottom zone on bottom surfaces: look at the part from below to paint them."),
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
