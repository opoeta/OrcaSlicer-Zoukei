// Height Adaptive Effects gizmo -- ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// Lean port: effect list + Z->value curve editor drawn over the object's real layer bands.
#include "GLGizmoHeightAdaptiveEffects.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <imgui/imgui.h>
#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

const std::vector<GLGizmoHeightAdaptiveEffects::EffectDef>& GLGizmoHeightAdaptiveEffects::effects()
{
    // Ranges are the EDITOR axis, not a hard constraint on the engine (the engine clamps what it needs to).
    // Stepped by default for everything that stacks layer on layer (line widths): inside each band the spacing is
    // constant so the lines still stack. Fuzzy skin parameters are surface texture, nothing stacks: ramp.
    static const std::vector<EffectDef> defs = {
        { "neotko_hae_infill_width",         L("Sparse infill width"),         "sparse_infill_line_width", HeightAdaptive::Interp::Stepped, true,  0.2,  0.8 },
        { "neotko_hae_outer_wall_width",     L("Outer wall width"),            "outer_wall_line_width",    HeightAdaptive::Interp::Stepped, true,  0.2,  0.8 },
        { "neotko_hae_inner_wall_width",     L("Inner wall width"),            "inner_wall_line_width",    HeightAdaptive::Interp::Stepped, true,  0.2,  0.8 },
        { "neotko_hae_fuzzy_thickness",      L("Fuzzy skin thickness"),        "fuzzy_skin_thickness",     HeightAdaptive::Interp::Smooth,  true,  0.0,  1.0 },
        { "neotko_hae_fuzzy_point_distance", L("Fuzzy skin point distance"),   "fuzzy_skin_point_distance",HeightAdaptive::Interp::Smooth,  true,  0.05, 2.0 },
        { "neotko_hae_fuzzy_scale",          L("Fuzzy skin noise scale"),      "fuzzy_skin_scale",         HeightAdaptive::Interp::Smooth,  true,  0.1,  10.0 },
        // Integer parameter: a curve over it is a staircase by definition (the engine rounds).
        { "neotko_hae_fuzzy_octaves",        L("Fuzzy skin noise octaves"),    "fuzzy_skin_octaves",       HeightAdaptive::Interp::Stepped, false, 1.0,  8.0 },
        { "neotko_hae_fuzzy_persistence",    L("Fuzzy skin noise persistence"),"fuzzy_skin_persistence",   HeightAdaptive::Interp::Smooth,  true,  0.0,  1.0 },
    };
    return defs;
}

GLGizmoHeightAdaptiveEffects::GLGizmoHeightAdaptiveEffects(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoHeightAdaptiveEffects::on_init()
{
    return true;
}

std::string GLGizmoHeightAdaptiveEffects::on_get_name() const
{
    return _u8L("Height Adaptive Effects");
}

bool GLGizmoHeightAdaptiveEffects::on_is_activable() const
{
    return m_parent.get_selection().get_object_idx() >= 0;
}

void GLGizmoHeightAdaptiveEffects::on_set_state()
{
    if (get_state() == Off) {
        m_have_session = false;
        m_drag_node    = -1;
        m_hover_node   = -1;
    }
}

void GLGizmoHeightAdaptiveEffects::ensure_session()
{
    const Selection& sel     = m_parent.get_selection();
    const int        obj_idx = sel.get_object_idx();
    const Model*     model   = sel.get_model();
    if (obj_idx < 0 || model == nullptr || obj_idx >= int(model->objects.size())) {
        m_have_session = false;
        return;
    }
    if (m_have_session && obj_idx == m_object_idx)
        return;

    m_object_idx          = obj_idx;
    const ModelObject* mo = model->objects[obj_idx];

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    m_slicing_params = PrintObject::slicing_parameters(full_config, *mo, float(mo->max_z()), Vec3d::Ones());

    // The Z axis is object-relative and raft-excluded: the same coordinate system as Layer::slice_z, which is
    // what the engine evaluates every curve at.
    m_object_height      = m_slicing_params.object_print_z_uncompensated_height();
    m_first_layer_height = m_slicing_params.first_object_layer_height;
    if (m_first_layer_height <= 0.)
        m_first_layer_height = m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2;

    rebuild_layer_bands();
    load_curves_from_object();

    const std::vector<size_t> listed = listed_effects();
    m_active_effect = listed.empty() ? -1 : int(listed.front());
    m_drag_node     = -1;
    m_hover_node    = -1;
    m_band_key      = -1.;
    m_have_session  = true;
}

void GLGizmoHeightAdaptiveEffects::rebuild_layer_bands()
{
    // The REAL layers, adaptive layer height included (the whole point of this gizmo): ask the same function the
    // slicer asks, fed the object's own layer height profile when it has a usable one.
    m_layer_tops.clear();
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= int(model->objects.size()))
        return;
    const ModelObject* mo = model->objects[m_object_idx];
    if (!m_slicing_params.valid || m_object_height <= 0.)
        return;

    std::vector<coordf_t> profile = mo->layer_height_profile.get();
    const bool profile_usable = profile.size() >= 4 && profile.size() % 2 == 0 && profile.front() <= 1e-6 &&
                                std::abs(profile[profile.size() - 2] - m_object_height) < 1e-3;
    if (!profile_usable) {
        double h = m_slicing_params.layer_height;
        if (h <= 0.)
            h = m_slicing_params.max_layer_height > 0. ? m_slicing_params.max_layer_height : 0.2;
        profile = { 0., m_first_layer_height, m_object_height, h };
    }

    const bool precise_z = wxGetApp().preset_bundle->full_config().opt_bool("precise_z_height");
    // Pairs of (bottom, top) per layer, object-relative, raft excluded.
    const std::vector<coordf_t> bounds = generate_object_layers(m_slicing_params, profile, precise_z);
    m_layer_tops.reserve(bounds.size() / 2);
    for (size_t i = 1; i < bounds.size(); i += 2)
        m_layer_tops.emplace_back(double(bounds[i]));
}

void GLGizmoHeightAdaptiveEffects::load_curves_from_object()
{
    const Model* model = m_parent.get_selection().get_model();
    m_states.assign(effects().size(), EffectState{});
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= int(model->objects.size()))
        return;
    const ModelObject* mo = model->objects[m_object_idx];

    for (size_t e = 0; e < effects().size(); ++e) {
        const EffectDef& def = effects()[e];
        m_states[e].interp   = def.default_interp;
        if (!mo->config.has(def.config_key))
            continue;
        const auto* opt = dynamic_cast<const ConfigOptionString*>(mo->config.option(def.config_key));
        if (opt == nullptr || opt->value.empty())
            continue;
        const auto curve   = HeightAdaptive::HeightCurve::parse(opt->value, def.default_interp);
        m_states[e].interp = curve.interp();
        m_states[e].nodes  = curve.nodes();
        // The list is rebuilt from the curves the object really carries (a committed curve has >= 2 nodes).
        m_states[e].added = m_states[e].nodes.size() >= 2;
    }
}

double GLGizmoHeightAdaptiveEffects::base_value(size_t effect_idx) const
{
    const EffectDef& d        = effects()[effect_idx];
    const double     fallback = 0.5 * (d.range_lo + d.range_hi);
    const Model*     model    = m_parent.get_selection().get_model();
    const ModelObject* mo     = (model != nullptr && m_object_idx >= 0 && m_object_idx < int(model->objects.size())) ?
                                    model->objects[m_object_idx] : nullptr;
    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
    const ConfigOption* opt = (mo != nullptr && mo->config.has(d.base_key)) ? mo->config.option(d.base_key) : full.option(d.base_key);
    if (opt == nullptr)
        return fallback;

    double nozzle = 0.4;
    if (const auto* nd = full.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!nd->values.empty())
            nozzle = nd->values.front();

    double v = fallback;
    if (const auto* fp = dynamic_cast<const ConfigOptionFloatOrPercent*>(opt)) {
        v = fp->percent ? fp->value * 0.01 * nozzle : fp->value;
        if (v <= 0.) // 0 = automatic width
            v = nozzle;
    } else if (const auto* f = dynamic_cast<const ConfigOptionFloat*>(opt)) {
        v = f->value;
    } else if (const auto* i = dynamic_cast<const ConfigOptionInt*>(opt)) {
        v = double(i->value);
    } else if (const auto* p = dynamic_cast<const ConfigOptionPercent*>(opt)) {
        v = p->value;
    }
    return std::clamp(v, d.range_lo, d.range_hi);
}

double GLGizmoHeightAdaptiveEffects::snap_z(double z) const
{
    if (m_layer_tops.empty())
        return z;
    auto it = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z);
    if (it == m_layer_tops.begin())
        return *it;
    if (it == m_layer_tops.end())
        return m_layer_tops.back();
    const double hi = *it, lo = *(it - 1);
    return (z - lo <= hi - z) ? lo : hi;
}

int GLGizmoHeightAdaptiveEffects::layers_between(double z_lo, double z_hi) const
{
    if (m_layer_tops.empty() || z_hi <= z_lo)
        return 0;
    const auto lo = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z_lo);
    const auto hi = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z_hi);
    return int(std::distance(lo, hi));
}

HeightAdaptive::Interp GLGizmoHeightAdaptiveEffects::interp_of(size_t effect_idx) const
{
    return effect_idx < m_states.size() ? m_states[effect_idx].interp : effects()[effect_idx].default_interp;
}

HeightAdaptive::HeightCurve GLGizmoHeightAdaptiveEffects::curve_of(size_t effect_idx) const
{
    HeightAdaptive::HeightCurve curve(interp_of(effect_idx));
    if (effect_idx < m_states.size())
        curve.set_nodes(m_states[effect_idx].nodes);
    return curve;
}

std::vector<size_t> GLGizmoHeightAdaptiveEffects::listed_effects() const
{
    std::vector<size_t> out;
    for (size_t e = 0; e < effects().size() && e < m_states.size(); ++e)
        if (m_states[e].added)
            out.push_back(e);
    return out;
}

void GLGizmoHeightAdaptiveEffects::add_effect(size_t effect_idx)
{
    if (effect_idx >= m_states.size() || m_object_height <= 0.)
        return;
    EffectState& st = m_states[effect_idx];
    st.added  = true;
    st.interp = effects()[effect_idx].default_interp;
    // Seed a flat curve at the current setting: a no-op until the user draws on it.
    const double v = base_value(effect_idx);
    st.nodes.clear();
    st.nodes.push_back(HeightAdaptive::Node{ 0., v, 0.5 });
    st.nodes.push_back(HeightAdaptive::Node{ m_object_height, v, 0.5 });
    m_active_effect = int(effect_idx);
    commit("Height adaptive effects - Add effect");
}

void GLGizmoHeightAdaptiveEffects::remove_effect(size_t effect_idx)
{
    if (effect_idx >= m_states.size())
        return;
    m_states[effect_idx].nodes.clear();
    m_states[effect_idx].added  = false;
    m_states[effect_idx].interp = effects()[effect_idx].default_interp;
    m_drag_node  = -1;
    m_hover_node = -1;
    if (m_active_effect == int(effect_idx)) {
        const std::vector<size_t> rest = listed_effects();
        m_active_effect = rest.empty() ? -1 : int(rest.front());
    }
    // commit() erases the key of every effect that is not in the list.
    commit("Height adaptive effects - Remove effect");
}

void GLGizmoHeightAdaptiveEffects::commit(const std::string& snapshot_name)
{
    if (!m_have_session || m_object_idx < 0)
        return;
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx >= int(model->objects.size()))
        return;

    wxGetApp().plater()->take_snapshot(snapshot_name);
    ModelObject* mo = const_cast<ModelObject*>(model->objects[m_object_idx]);

    for (size_t e = 0; e < effects().size(); ++e) {
        const EffectDef& def = effects()[e];
        // A curve needs at least two nodes. ERASE the key (don't store "") so an object that never had a curve and
        // one whose curve was deleted are indistinguishable.
        const std::string blob = (m_states[e].added && m_states[e].nodes.size() >= 2) ? curve_of(e).serialize() : std::string();
        if (blob.empty())
            mo->config.erase(def.config_key);
        else
            mo->config.set_key_value(def.config_key, new ConfigOptionString(blob));
    }

    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(size_t(m_object_idx));
}

void GLGizmoHeightAdaptiveEffects::quantize_to_steps(size_t effect_idx, int bands)
{
    // Rewrites a smooth curve as a staircase of `bands` steps that follows its shape, every edge snapped to a real
    // layer boundary: "looks like a ramp" while keeping a constant spacing inside each band.
    if (effect_idx >= m_states.size() || m_object_height <= 0. || bands < 2)
        return;
    const auto curve = curve_of(effect_idx);
    if (curve.empty())
        return;

    std::vector<HeightAdaptive::Node> out;
    out.reserve(size_t(bands));
    for (int i = 0; i < bands; ++i) {
        const double z_lo = m_object_height * double(i) / double(bands);
        const double z_hi = m_object_height * double(i + 1) / double(bands);
        HeightAdaptive::Node n;
        n.z       = (i == 0) ? 0. : snap_z(z_lo);
        n.value   = curve.at(0.5 * (z_lo + z_hi), 0.); // middle of the band is representative of it
        n.tension = 0.;
        if (!out.empty() && n.z <= out.back().z + 1e-9)
            continue; // snapping collapsed two bands onto the same layer
        out.push_back(n);
    }
    if (out.size() >= 2) {
        m_states[effect_idx].nodes  = std::move(out);
        m_states[effect_idx].interp = HeightAdaptive::Interp::Stepped;
    }
}

double GLGizmoHeightAdaptiveEffects::misalignment_per_layer_mm(size_t effect_idx, double* out_pct) const
{
    // Estimated XY shift of the sparse infill lines between two CONSECUTIVE layers caused by a width ramp, at the
    // far edge of the object. Only meaningful for the effect that drives infill spacing, and only for ramps.
    if (out_pct != nullptr)
        *out_pct = 0.;
    if (effect_idx >= m_states.size() || std::string(effects()[effect_idx].config_key) != "neotko_hae_infill_width")
        return 0.;
    if (interp_of(effect_idx) != HeightAdaptive::Interp::Smooth)
        return 0.;
    const auto curve = curve_of(effect_idx);
    if (curve.empty() || m_object_height <= 0.)
        return 0.;

    double       far_dist = 0.;
    const Model* model    = m_parent.get_selection().get_model();
    if (model != nullptr && m_object_idx >= 0 && m_object_idx < int(model->objects.size())) {
        BoundingBoxf3 bb;
        for (size_t i = 0; i < model->objects[m_object_idx]->instances.size(); ++i)
            bb.merge(model->objects[m_object_idx]->instance_bounding_box(i));
        if (bb.defined)
            far_dist = 0.5 * std::hypot(bb.size().x(), bb.size().y());
    }
    if (far_dist <= 0.)
        return 0.;

    const double layer_h = m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2;
    double       worst = 0., worst_pct = 0.;
    constexpr int kSamples = 64;
    for (int i = 0; i < kSamples; ++i) {
        const double z  = m_object_height * double(i) / double(kSamples - 1);
        const double dz = std::max(1e-3, 0.5 * layer_h);
        const double w0 = curve.at(std::max(0., z - dz), 0.);
        const double w1 = curve.at(std::min(m_object_height, z + dz), 0.);
        const double w  = curve.at(z, 0.);
        if (w <= 1e-6)
            continue;
        const double rel_per_layer = std::abs(w1 - w0) / (2. * dz) * layer_h / w;
        const double shift         = far_dist * rel_per_layer;
        if (shift > worst) {
            worst     = shift;
            worst_pct = 100. * shift / w;
        }
    }
    if (out_pct != nullptr)
        *out_pct = worst_pct;
    return worst;
}

void GLGizmoHeightAdaptiveEffects::build_band_model(double z_mm)
{
    m_band_model.reset();

    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= int(model->objects.size()))
        return;
    const ModelObject* mo = model->objects[m_object_idx];

    BoundingBoxf3 bb;
    for (size_t i = 0; i < mo->instances.size(); ++i)
        bb.merge(mo->instance_bounding_box(i));
    if (!bb.defined)
        return;

    const double zc  = bb.min.z() + z_mm;
    const double pad = 1.5;
    const double ht  = std::max(0.15, 0.5 * m_slicing_params.layer_height);
    const double x0 = bb.min.x() - pad, x1 = bb.max.x() + pad;
    const double y0 = bb.min.y() - pad, y1 = bb.max.y() + pad;
    const double z0 = zc - ht, z1 = zc + ht;

    indexed_triangle_set its;
    its.vertices = {
        Vec3f(float(x0), float(y0), float(z0)), Vec3f(float(x1), float(y0), float(z0)),
        Vec3f(float(x1), float(y1), float(z0)), Vec3f(float(x0), float(y1), float(z0)),
        Vec3f(float(x0), float(y0), float(z1)), Vec3f(float(x1), float(y0), float(z1)),
        Vec3f(float(x1), float(y1), float(z1)), Vec3f(float(x0), float(y1), float(z1)),
    };
    const int box_tris[12][3] = {
        {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
        {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
        {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0},
    };
    its.indices.reserve(12);
    for (const auto& t : box_tris)
        its.indices.emplace_back(t[0], t[1], t[2]);
    m_band_model.init_from(its);
}

void GLGizmoHeightAdaptiveEffects::on_render()
{
    // Show, on the object itself, which slice the focused node affects (world space, flat shader).
    const int eff  = m_active_effect;
    const int node = (m_drag_node >= 0) ? m_drag_node : m_hover_node;
    if (!m_have_session || eff < 0 || eff >= int(m_states.size()))
        return;
    if (node < 0 || node >= int(m_states[size_t(eff)].nodes.size()))
        return;

    const double z_mm = m_states[size_t(eff)].nodes[size_t(node)].z;
    const double key  = eff * 1e6 + z_mm;
    if (key != m_band_key) {
        build_band_model(z_mm);
        m_band_key = key;
    }
    if (!m_band_model.is_initialized())
        return;

    const bool dragging = (m_drag_node >= 0);
    m_band_model.set_color(dragging ? ColorRGBA(1.00f, 0.75f, 0.16f, 0.42f) : ColorRGBA(0.00f, 0.59f, 0.53f, 0.30f));

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_band_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoHeightAdaptiveEffects::render_editor(float width, float height)
{
    if (m_active_effect < 0 || m_active_effect >= int(m_states.size()))
        return;
    const size_t     e   = size_t(m_active_effect);
    const EffectDef& def = effects()[e];
    EffectState&     st  = m_states[e];

    ImGuiIO&     io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hae_graph", ImVec2(width, height));
    const bool  hovered     = ImGui::IsItemHovered();
    const bool  deactivated = ImGui::IsItemDeactivated();
    ImDrawList* dl          = ImGui::GetWindowDrawList();

    const double z_max = std::max(m_object_height, 1e-6);
    const double v_lo  = def.range_lo;
    const double v_hi  = std::max(def.range_hi, v_lo + 1e-6);
    const bool   stepped = interp_of(e) == HeightAdaptive::Interp::Stepped;

    auto to_screen = [&](double z, double v) -> ImVec2 {
        const float u = float((v - v_lo) / (v_hi - v_lo));
        const float w = float(1.0 - z / z_max);
        return ImVec2(p0.x + u * width, p0.y + w * height);
    };
    auto from_screen = [&](const ImVec2& s) -> std::pair<double, double> {
        const double u = std::clamp(double(s.x - p0.x) / double(width), 0.0, 1.0);
        const double w = std::clamp(double(s.y - p0.y) / double(height), 0.0, 1.0);
        return { v_lo + u * (v_hi - v_lo), z_max * (1.0 - w) }; // { value, z }
    };

    const ImU32 col_canvas = IM_COL32(24, 26, 30, 235);
    const ImU32 col_grid   = IM_COL32(255, 255, 255, 28);
    const ImU32 col_layer  = IM_COL32(255, 255, 255, 22);
    const ImU32 col_text   = IM_COL32(200, 205, 210, 210);
    const ImU32 col_curve  = IM_COL32(60, 200, 190, 255);
    const ImU32 col_glow   = IM_COL32(60, 200, 190, 46);
    const ImU32 col_border = IM_COL32(110, 115, 120, 255);

    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), col_canvas, 6.0f);

    // The real layer bands (thinned out when they are denser than ~3 px).
    if (!m_layer_tops.empty()) {
        const float px_per_layer = height * float(m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2) / float(z_max);
        const size_t stride = px_per_layer >= 3.f ? 1 : size_t(std::ceil(3.f / std::max(px_per_layer, 0.01f)));
        for (size_t i = 0; i < m_layer_tops.size(); i += stride) {
            const float y = to_screen(m_layer_tops[i], v_lo).y;
            dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p0.x + width - 1.f, y), col_layer, 1.0f);
        }
    }

    // Axis ticks: value across the top, Z up the left.
    for (int t = 0; t <= 4; ++t) {
        const float  fx = float(t) / 4.f;
        const double v  = v_lo + (v_hi - v_lo) * fx;
        const float  xv = p0.x + fx * width;
        dl->AddLine(ImVec2(xv, p0.y + 1.f), ImVec2(xv, p0.y + height - 1.f), col_grid, 1.0f);
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%.2f", v);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        float        lx  = (t == 0) ? p0.x + 3.f : (t == 4) ? p0.x + width - tsz.x - 3.f : xv - tsz.x * 0.5f;
        lx               = std::clamp(lx, p0.x + 3.f, p0.x + width - tsz.x - 3.f);
        dl->AddText(ImVec2(lx, p0.y + 2.f), col_text, lbl);
    }
    for (int t = 0; t <= 4; ++t) {
        const double z = z_max * double(t) / 4.;
        const float  y = to_screen(z, v_lo).y;
        dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p0.x + width - 1.f, y), col_grid, 1.0f);
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%.1f", z);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        const float  ly  = std::clamp(y - tsz.y * 0.5f, p0.y + tsz.y + 4.f, p0.y + height - tsz.y - 2.f);
        dl->AddText(ImVec2(p0.x + 4.f, ly), col_text, lbl);
    }

    // The curve (sampled), valid with two or more nodes.
    const HeightAdaptive::HeightCurve curve = curve_of(e);
    if (st.nodes.size() >= 2) {
        constexpr int kSamples = 240;
        ImVec2 prev{};
        for (int i = 0; i <= kSamples; ++i) {
            const double z = z_max * double(i) / double(kSamples);
            const double v = std::clamp(curve.at(z, v_lo), v_lo, v_hi);
            const ImVec2 cur = to_screen(z, v);
            if (i > 0) {
                dl->AddLine(prev, cur, col_glow, 6.0f);
                dl->AddLine(prev, cur, col_curve, 2.2f);
            }
            prev = cur;
        }
    }

    // Nodes.
    int hovered_node = -1;
    for (size_t i = 0; i < st.nodes.size(); ++i) {
        const ImVec2 c = to_screen(st.nodes[i].z, std::clamp(st.nodes[i].value, v_lo, v_hi));
        if (hovered && std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) <= 8.0f)
            hovered_node = int(i);
        const bool  dragged = int(i) == m_drag_node;
        const bool  lit     = dragged || hovered_node == int(i);
        const float r       = dragged ? 6.5f : (lit ? 6.0f : 4.5f);
        dl->AddCircleFilled(c, r, col_curve);
        dl->AddCircle(c, r + 1.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
    }
    dl->AddRect(p0, ImVec2(p0.x + width, p0.y + height), col_border, 6.0f, 0, 1.0f);

    m_hover_node = hovered ? hovered_node : -1;

    // Click on empty area: add a node. Click on a node: start dragging it.
    if (m_drag_node < 0 && hovered_node >= 0 && io.MouseClicked[0]) {
        m_drag_node = hovered_node;
    } else if (hovered && hovered_node < 0 && m_drag_node < 0 && io.MouseClicked[0]) {
        const auto [v, z0] = from_screen(io.MousePos);
        const double z     = stepped ? snap_z(z0) : z0;
        HeightAdaptive::Node n;
        n.z       = z;
        n.value   = std::clamp(v, v_lo, v_hi);
        n.tension = 0.5;
        st.nodes.push_back(n);
        // Sort and collapse nodes sharing a Z (the last one wins).
        st.nodes = curve_of(e).nodes();
        commit("Height adaptive effects - Add point");
    }

    if (m_drag_node >= 0 && m_drag_node < int(st.nodes.size()) && ImGui::IsItemActive()) {
        const auto [v, z0] = from_screen(io.MousePos);
        const size_t i     = size_t(m_drag_node);
        const double z_lo  = (i > 0) ? st.nodes[i - 1].z + 0.02 : 0.;
        const double z_hi  = (i + 1 < st.nodes.size()) ? st.nodes[i + 1].z - 0.02 : z_max;
        if (z_hi > z_lo) {
            double z = stepped ? snap_z(z0) : z0;
            if (z < z_lo || z > z_hi)
                z = std::clamp(z0, z_lo, z_hi); // snapping would collide with a neighbour: keep it free
            st.nodes[i].z = z;
        }
        st.nodes[i].value = std::clamp(v, v_lo, v_hi);
        m_hover_node = m_drag_node;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
    if (m_drag_node >= 0 && deactivated) {
        st.nodes = curve_of(e).nodes();
        commit("Height adaptive effects - Move point");
        m_drag_node = -1;
    }

    // Right-click a node: delete it.
    if (hovered && hovered_node >= 0 && io.MouseClicked[1] && m_drag_node < 0) {
        st.nodes.erase(st.nodes.begin() + hovered_node);
        m_hover_node = -1;
        commit("Height adaptive effects - Delete point");
    }

    // Live readout: the Z under the cursor and what the curve is worth there (or the focused node).
    if (hovered || m_drag_node >= 0) {
        std::string label;
        const int   focus = (m_drag_node >= 0) ? m_drag_node : m_hover_node;
        if (focus >= 0 && focus < int(st.nodes.size())) {
            label = "Z " + format(float(st.nodes[size_t(focus)].z), 2) + " mm   " + format(float(st.nodes[size_t(focus)].value), 3);
        } else {
            const auto [v, z] = from_screen(io.MousePos);
            label = "Z " + format(float(z), 2) + " mm";
            if (st.nodes.size() >= 2)
                label += "   " + format(float(curve.at(z, v_lo)), 3);
        }
        const ImVec2 tsz = ImGui::CalcTextSize(label.c_str());
        const ImVec2 lp(p0.x + width - tsz.x - 10.f, p0.y + height - tsz.y - 8.f);
        dl->AddRectFilled(ImVec2(lp.x - 4.f, lp.y - 2.f), ImVec2(lp.x + tsz.x + 4.f, lp.y + tsz.y + 2.f), IM_COL32(20, 22, 26, 235), 4.f);
        dl->AddText(lp, IM_COL32(240, 240, 240, 255), label.c_str());
    }
}

void GLGizmoHeightAdaptiveEffects::on_render_input_window(float x, float y, float bottom_limit)
{
    ensure_session();

    const float win_w = 400.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    ImGui::PushTextWrapPos(win_w - 16.f);

    if (!m_have_session) {
        ImGui::TextWrapped("%s", _u8L("Select a single object to edit its height adaptive effects.").c_str());
        ImGui::PopTextWrapPos();
        GizmoImguiEnd();
        return;
    }

    ImGui::TextWrapped("%s", _u8L("Draw a curve over the object's real layer bands (the horizontal lines) to change a "
                                  "setting with height. An object with no curve prints exactly as before.").c_str());
    ImGui::Dummy(ImVec2(0.f, 2.f));

    // --- list of the effects used by this object ---
    const std::vector<size_t> listed = listed_effects();
    int remove_idx = -1;
    for (size_t e : listed) {
        ImGui::PushID(int(e));
        const bool active = int(e) == m_active_effect;
        std::string row = _u8L(effects()[e].label) + " (" + std::to_string(m_states[e].nodes.size()) + ")";
        if (ImGui::Selectable(row.c_str(), active, 0, ImVec2(win_w - 70.f, 0.f)))
            m_active_effect = int(e);
        ImGui::SameLine();
        if (ImGui::SmallButton(_u8L("Remove").c_str()))
            remove_idx = int(e);
        ImGui::PopID();
    }
    if (remove_idx >= 0)
        remove_effect(size_t(remove_idx));

    // --- add an effect ---
    {
        std::vector<size_t> avail;
        for (size_t e = 0; e < effects().size(); ++e)
            if (!m_states[e].added)
                avail.push_back(e);
        if (!avail.empty()) {
            if (m_add_choice < 0 || m_add_choice >= int(avail.size()))
                m_add_choice = 0;
            ImGui::SetNextItemWidth(win_w - 100.f);
            if (ImGui::BeginCombo("##hae_add", _u8L(effects()[avail[size_t(m_add_choice)]].label).c_str())) {
                for (size_t k = 0; k < avail.size(); ++k) {
                    if (ImGui::Selectable(_u8L(effects()[avail[k]].label).c_str(), int(k) == m_add_choice))
                        m_add_choice = int(k);
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(_u8L("Add").c_str()))
                add_effect(avail[size_t(m_add_choice)]);
        }
    }

    if (m_active_effect >= 0 && m_active_effect < int(m_states.size()) && m_states[size_t(m_active_effect)].added) {
        const size_t     e   = size_t(m_active_effect);
        const EffectDef& def = effects()[e];
        EffectState&     st  = m_states[e];

        ImGui::Separator();

        // Mode: steps hold the value constant inside each band; ramp varies it continuously.
        if (def.mode_selectable) {
            int mode = st.interp == HeightAdaptive::Interp::Stepped ? 0 : 1;
            bool changed = false;
            changed |= ImGui::RadioButton(_u8L("Steps").c_str(), &mode, 0);
            ImGui::SameLine();
            changed |= ImGui::RadioButton(_u8L("Ramp").c_str(), &mode, 1);
            if (changed) {
                st.interp = mode == 0 ? HeightAdaptive::Interp::Stepped : HeightAdaptive::Interp::Smooth;
                if (st.interp == HeightAdaptive::Interp::Stepped)
                    for (auto& n : st.nodes)
                        n.z = snap_z(n.z);
                st.nodes = curve_of(e).nodes();
                commit("Height adaptive effects - Mode");
            }
        } else {
            ImGui::TextDisabled("%s", _u8L("Integer setting: always steps.").c_str());
        }

        ImGui::Dummy(ImVec2(0.f, 2.f));
        render_editor(win_w - 16.f, 240.f);
        ImGui::TextDisabled("%s", _u8L("Click: add point. Drag: move. Right-click: delete.").c_str());
        if (st.nodes.size() < 2)
            ImGui::TextWrapped("%s", _u8L("At least two points are needed for the curve to take effect.").c_str());

        const bool is_fuzzy = std::string(def.config_key).find("fuzzy") != std::string::npos;
        if (is_fuzzy)
            ImGui::TextWrapped("%s", _u8L("The curve scales fuzzy skin where it is enabled; it does not switch it on. "
                                          "Fuzzy skin thickness reaching 0 turns fuzzy skin off for that layer.").c_str());

        // Ramp-only tools.
        if (st.interp == HeightAdaptive::Interp::Smooth && st.nodes.size() >= 2) {
            float smooth = st.nodes.front().tension >= 0 ? float(st.nodes.front().tension) : 0.5f;
            ImGui::SetNextItemWidth(win_w - 150.f);
            if (ImGui::SliderFloat(_u8L("Smoothness").c_str(), &smooth, 0.0f, 1.0f)) {
                for (auto& n : st.nodes)
                    n.tension = double(smooth);
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                commit("Height adaptive effects - Smoothness");

            ImGui::SetNextItemWidth(80.f);
            ImGui::InputInt(_u8L("Bands").c_str(), &m_quantize_bands, 1, 2);
            m_quantize_bands = std::clamp(m_quantize_bands, 2, 40);
            ImGui::SameLine();
            if (ImGui::Button(_u8L("Staircase").c_str())) {
                quantize_to_steps(e, m_quantize_bands);
                commit("Height adaptive effects - Staircase");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Turns the ramp into layer-aligned bands, keeping its shape.").c_str());

            double pct = 0.;
            const double shift = misalignment_per_layer_mm(e, &pct);
            if (shift > 0.) {
                const bool warn = pct > 25.;
                if (warn)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.15f, 1.0f));
                ImGui::TextWrapped("%s", (_u8L("Line shift per layer") + ": " + format(float(shift), 4) + " mm (" + format(float(pct), 1) + " %)").c_str());
                if (warn)
                    ImGui::PopStyleColor();
            }
        }

        // Steps mode: a band shorter than ~4 layers costs more than it gives.
        if (st.interp == HeightAdaptive::Interp::Stepped && st.nodes.size() >= 2) {
            int shortest = 1 << 30;
            for (size_t i = 0; i + 1 < st.nodes.size(); ++i)
                shortest = std::min(shortest, layers_between(st.nodes[i].z, st.nodes[i + 1].z));
            if (shortest < 4) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.15f, 1.0f));
                ImGui::TextWrapped("%s", _u8L("The shortest band is under 4 layers: the jump costs more than it gives.").c_str());
                ImGui::PopStyleColor();
            }
        }

        // Numeric table of the points.
        ImGui::Checkbox(_u8L("Open Z points").c_str(), &m_show_points);
        if (m_show_points) {
            bool edited = false;
            for (size_t i = 0; i < st.nodes.size(); ++i) {
                ImGui::PushID(int(i));
                double z = st.nodes[i].z, v = st.nodes[i].value;
                ImGui::SetNextItemWidth(110.f);
                const bool ez = ImGui::InputDouble("##z", &z, 0.0, 0.0, "%.3f");
                const bool dz = ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.f);
                const bool ev = ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.3f");
                const bool dv = ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::TextDisabled("L%d", int(std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z) - m_layer_tops.begin()) + 1);
                if (ez || ev) {
                    st.nodes[i].z     = std::clamp(z, 0., m_object_height);
                    st.nodes[i].value = std::clamp(v, def.range_lo, def.range_hi);
                }
                edited |= (dz || dv);
                ImGui::PopID();
            }
            if (edited) {
                st.nodes = curve_of(e).nodes();
                commit("Height adaptive effects - Edit points");
            }
        }

        ImGui::Separator();
        if (ImGui::Button(_u8L("Clear").c_str())) {
            st.nodes.clear();
            commit("Height adaptive effects - Clear");
        }
        ImGui::SameLine();
        if (ImGui::Button(_u8L("Refresh layers").c_str()))
            rebuild_layer_bands();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Re-read the layer bands after changing the layer height profile.").c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _u8L("Z is measured from the bottom of the object.").c_str());
    }

    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
}

}} // namespace Slic3r::GUI
