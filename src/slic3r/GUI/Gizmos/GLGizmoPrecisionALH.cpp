// Precision Adaptive Layer Height gizmo -- ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// Lean port: point/tension curve editor only; "Adapt to Color" and "Slope recolor" (WIP upstream) are not ported.
#include "GLGizmoPrecisionALH.hpp"

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

namespace {
// Stretches only the Z of every pair of [z0,h0,z1,h1,...] so the last Z equals new_height (heights untouched).
// Same idea as upstream's rescale_layer_height_profile: a profile saved before the object was scaled in Z.
bool rescale_profile_z(std::vector<coordf_t>& profile, double new_height)
{
    if (profile.size() < 4 || (profile.size() & 1) != 0 || new_height <= 1e-4)
        return false;
    const double old_height = profile[profile.size() - 2];
    if (old_height <= 1e-4 || std::abs(old_height - new_height) <= 1e-3)
        return false;
    const double k = new_height / old_height;
    for (size_t i = 0; i < profile.size(); i += 2)
        profile[i] *= k;
    profile[profile.size() - 2] = new_height;
    return true;
}
} // namespace

GLGizmoPrecisionALH::GLGizmoPrecisionALH(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoPrecisionALH::on_init()
{
    return true;
}

std::string GLGizmoPrecisionALH::on_get_name() const
{
    return _u8L("Precision Adaptive Layer Height");
}

bool GLGizmoPrecisionALH::on_is_activable() const
{
    return m_parent.get_selection().get_object_idx() >= 0;
}

void GLGizmoPrecisionALH::on_set_state()
{
    if (get_state() == Off) {
        m_have_session   = false;
        m_dragging_point = -1;
        m_hover_point    = -1;
    }
}

void GLGizmoPrecisionALH::ensure_session()
{
    const Selection& sel     = m_parent.get_selection();
    const int        obj_idx = sel.get_object_idx();
    const Model*     model   = sel.get_model();
    if (obj_idx < 0 || model == nullptr || obj_idx >= int(model->objects.size())) {
        m_have_session = false;
        return;
    }
    if (m_have_session && obj_idx == m_object_idx)
        return; // same object: keep in-progress edits across a panel redraw

    m_object_idx          = obj_idx;
    const ModelObject* mo = model->objects[obj_idx];

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    m_slicing_params = PrintObject::slicing_parameters(full_config, *mo, float(mo->max_z()), Vec3d::Ones());

    // Use the exact values the engine validates the profile against.
    m_object_height      = m_slicing_params.object_print_z_uncompensated_height();
    m_first_layer_height = m_slicing_params.first_object_layer_height;
    if (m_first_layer_height <= 0.0)
        m_first_layer_height = m_slicing_params.layer_height > 0.0 ? m_slicing_params.layer_height : 0.2;

    load_points_from_profile(mo->layer_height_profile.get());

    m_have_session   = true;
    m_dragging_point = -1;
    m_hover_point    = -1;
    m_band_key       = -1.0;
}

void GLGizmoPrecisionALH::seed_flat_profile()
{
    double h = m_slicing_params.layer_height;
    if (h <= 0.0)
        h = m_slicing_params.max_layer_height > 0.0 ? m_slicing_params.max_layer_height : 0.2;

    m_points.clear();
    m_points.push_back(ALHPoint{ 0.0, m_first_layer_height, 0.0 });
    m_points.push_back(ALHPoint{ m_object_height, h, 0.0 });
}

void GLGizmoPrecisionALH::load_points_from_profile(const std::vector<coordf_t>& profile_in)
{
    // A profile denser than this almost certainly comes from the stock brush (per-pixel samples), not from a
    // reasonable set of draggable control points: fall back to a flat seed.
    constexpr size_t kMaxPointsToImport = 80;

    // The saved profile is in absolute Z: after the object is scaled in Z its last Z is stale. Stretch it first.
    std::vector<coordf_t> profile = profile_in;
    m_rescaled_on_load            = rescale_profile_z(profile, m_object_height);

    if (profile.size() >= 4 && profile.size() <= kMaxPointsToImport * 2 && profile.size() % 2 == 0) {
        std::vector<ALHPoint> pts;
        pts.reserve(profile.size() / 2);
        for (size_t i = 0; i + 1 < profile.size(); i += 2)
            pts.push_back(ALHPoint{ profile[i], profile[i + 1], 0.0 });
        // Must span from the base to the object height (generate_object_layers() hard constraint).
        if (pts.size() >= 2 && pts.front().z_mm <= 1e-6 && pts.back().z_mm >= m_object_height - 1e-3) {
            pts.front().z_mm      = 0.0;
            pts.front().height_mm = m_first_layer_height;
            pts.back().z_mm       = m_object_height;
            m_points              = std::move(pts);
            return;
        }
    }
    m_rescaled_on_load = false;
    seed_flat_profile();
}

std::vector<double> GLGizmoPrecisionALH::compute_tangents() const
{
    const size_t        n = m_points.size();
    std::vector<double> m(n, 0.0);
    if (n < 2)
        return m;

    std::vector<double> d(n - 1, 0.0);
    for (size_t i = 0; i + 1 < n; ++i) {
        const double dz = m_points[i + 1].z_mm - m_points[i].z_mm;
        d[i]            = (dz > 1e-9) ? (m_points[i + 1].height_mm - m_points[i].height_mm) / dz : 0.0;
    }
    m[0]     = d.front();
    m[n - 1] = d.back();
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = 0.5 * (d[i - 1] + d[i]);

    // Fritsch-Carlson correction: the cubic never overshoots past the two endpoint heights of a segment.
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0) {
            m[i]     = 0.0;
            m[i + 1] = 0.0;
            continue;
        }
        double alpha = m[i] / d[i];
        double beta  = m[i + 1] / d[i];
        if (alpha < 0.0) {
            m[i]  = 0.0;
            alpha = 0.0;
        }
        if (beta < 0.0) {
            m[i + 1] = 0.0;
            beta     = 0.0;
        }
        const double a2b2 = alpha * alpha + beta * beta;
        if (a2b2 > 9.0) {
            const double tau = 3.0 / std::sqrt(a2b2);
            m[i]             = tau * alpha * d[i];
            m[i + 1]         = tau * beta * d[i];
        }
    }
    return m;
}

double GLGizmoPrecisionALH::blended_height(size_t seg_idx, double t, const std::vector<double>& tangents) const
{
    const ALHPoint& p0     = m_points[seg_idx];
    const ALHPoint& p1     = m_points[seg_idx + 1];
    const double    linear = p0.height_mm + t * (p1.height_mm - p0.height_mm);

    const double tension = std::clamp(p0.tension, 0.0, 1.0);
    if (tension <= 1e-6)
        return linear;

    const double dz = p1.z_mm - p0.z_mm;
    const double t2 = t * t, t3 = t2 * t;
    const double h00   = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10   = t3 - 2.0 * t2 + t;
    const double h01   = -2.0 * t3 + 3.0 * t2;
    const double h11   = t3 - t2;
    const double cubic = h00 * p0.height_mm + h10 * dz * tangents[seg_idx] + h01 * p1.height_mm + h11 * dz * tangents[seg_idx + 1];

    const double blended = (1.0 - tension) * linear + tension * cubic;
    return std::clamp(blended, m_slicing_params.min_layer_height, m_slicing_params.max_layer_height);
}

std::vector<coordf_t> GLGizmoPrecisionALH::build_profile_vector() const
{
    std::vector<coordf_t> out;
    if (m_points.size() < 2)
        return out;

    const std::vector<double> tangents = compute_tangents();
    // First pair MUST be exactly [0, first_object_layer_height] or the slicer discards the whole profile.
    out.push_back(0.0);
    out.push_back(m_first_layer_height);

    constexpr int kSamples = 16;
    for (size_t i = 0; i + 1 < m_points.size(); ++i) {
        const ALHPoint& p0 = m_points[i];
        const ALHPoint& p1 = m_points[i + 1];
        if (p0.tension <= 1e-6) {
            // Straight segment: the engine lerps linearly between consecutive pairs, no need to densify.
            out.push_back(p1.z_mm);
            out.push_back(p1.height_mm);
            continue;
        }
        for (int s = 1; s <= kSamples; ++s) {
            const double t = double(s) / double(kSamples);
            out.push_back(p0.z_mm + t * (p1.z_mm - p0.z_mm));
            out.push_back(blended_height(i, t, tangents));
        }
    }
    // Last Z MUST equal object_print_z_uncompensated_height() exactly.
    if (out.size() >= 2)
        out[out.size() - 2] = m_object_height;
    return out;
}

void GLGizmoPrecisionALH::commit(const std::string& snapshot_name)
{
    if (!m_have_session || m_object_idx < 0)
        return;
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx >= int(model->objects.size()))
        return;

    wxGetApp().plater()->take_snapshot(snapshot_name);
    ModelObject* mo = const_cast<ModelObject*>(model->objects[m_object_idx]);
    mo->layer_height_profile.set(build_profile_vector());
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(size_t(m_object_idx));
}

void GLGizmoPrecisionALH::build_band_model(int idx)
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

    // A thin slab at the control point's world Z, spanning the object's XY footprint plus a small margin.
    const double zc  = bb.min.z() + m_points[size_t(idx)].z_mm;
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

void GLGizmoPrecisionALH::on_render()
{
    // Highlight, on the object itself, the Z band of the control point being dragged or hovered.
    const int idx = (m_dragging_point >= 0) ? m_dragging_point : m_hover_point;
    if (!m_have_session || idx < 0 || idx >= int(m_points.size()))
        return;

    const double key = idx * 1e6 + m_points[size_t(idx)].z_mm;
    if (key != m_band_key) {
        build_band_model(idx);
        m_band_key = key;
    }
    if (!m_band_model.is_initialized())
        return;

    const bool dragging = (m_dragging_point >= 0);
    m_band_model.set_color(dragging ? ColorRGBA(1.00f, 0.75f, 0.16f, 0.42f)   // amber while dragging
                                    : ColorRGBA(0.00f, 0.59f, 0.53f, 0.30f)); // teal on hover

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix()); // vertices are in world space
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_band_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoPrecisionALH::render_curve_editor(float width, float height)
{
    ImGuiIO&     io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##alh_band", ImVec2(width, height));
    const bool hovered          = ImGui::IsItemHovered();
    const bool band_deactivated = ImGui::IsItemDeactivated();
    ImDrawList* dl              = ImGui::GetWindowDrawList();

    const double z_max = std::max(m_object_height, 1e-6);
    const double h_min = m_slicing_params.min_layer_height > 0.0 ? m_slicing_params.min_layer_height : 0.01;
    const double h_max = std::max(m_slicing_params.max_layer_height, h_min + 1e-6);

    auto to_screen = [&](double z, double h) -> ImVec2 {
        const float u = float((h - h_min) / (h_max - h_min)); // across width
        const float v = float(1.0 - z / z_max);               // top..bottom
        return ImVec2(p0.x + u * width, p0.y + v * height);
    };
    auto from_screen = [&](const ImVec2& s) -> std::pair<double, double> {
        const double u = std::clamp(double(s.x - p0.x) / double(width), 0.0, 1.0);
        const double v = std::clamp(double(s.y - p0.y) / double(height), 0.0, 1.0);
        return { h_min + u * (h_max - h_min), z_max * (1.0 - v) }; // { height, z }
    };

    const ImU32 col_canvas = IM_COL32(24, 26, 30, 235);
    const ImU32 col_grid   = IM_COL32(255, 255, 255, 28);
    const ImU32 col_text   = IM_COL32(200, 205, 210, 210);
    const ImU32 col_curve  = IM_COL32(60, 200, 190, 255);
    const ImU32 col_glow   = IM_COL32(60, 200, 190, 46);
    const ImU32 col_locked = IM_COL32(140, 140, 145, 255);
    const ImU32 col_end    = IM_COL32(255, 190, 50, 255);
    const ImU32 col_border = IM_COL32(110, 115, 120, 255);

    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), col_canvas, 6.0f);

    // Grid and axis labels: height across the top, Z up the left, five ticks each.
    for (int t = 0; t <= 4; ++t) {
        const float  fx = float(t) / 4.f;
        const double h  = h_min + (h_max - h_min) * fx;
        const float  xv = p0.x + fx * width;
        dl->AddLine(ImVec2(xv, p0.y + 1.f), ImVec2(xv, p0.y + height - 1.f), col_grid, 1.0f);
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%.2f", h);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        float        lx  = (t == 0) ? p0.x + 3.f : (t == 4) ? p0.x + width - tsz.x - 3.f : xv - tsz.x * 0.5f;
        lx               = std::clamp(lx, p0.x + 3.f, p0.x + width - tsz.x - 3.f);
        dl->AddText(ImVec2(lx, p0.y + 2.f), col_text, lbl);
    }
    for (int t = 0; t <= 4; ++t) {
        const double z = z_max * double(t) / 4.;
        const float  y = to_screen(z, h_min).y;
        dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p0.x + width - 1.f, y), col_grid, 1.0f);
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%.1f", z);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        const float  ly  = std::clamp(y - tsz.y * 0.5f, p0.y + tsz.y + 4.f, p0.y + height - tsz.y - 2.f);
        dl->AddText(ImVec2(p0.x + 4.f, ly), col_text, lbl);
    }

    // Curve: per-segment sampled polyline.
    const std::vector<double> tangents = compute_tangents();
    constexpr int             kDrawSamples = 24;
    for (size_t i = 0; i + 1 < m_points.size(); ++i) {
        ImVec2 prev = to_screen(m_points[i].z_mm, m_points[i].height_mm);
        for (int s = 1; s <= kDrawSamples; ++s) {
            const double t   = double(s) / double(kDrawSamples);
            const double z   = m_points[i].z_mm + t * (m_points[i + 1].z_mm - m_points[i].z_mm);
            const double h   = blended_height(i, t, tangents);
            const ImVec2 cur = to_screen(z, h);
            dl->AddLine(prev, cur, col_glow, 6.0f);
            dl->AddLine(prev, cur, col_curve, 2.2f);
            prev = cur;
        }
    }

    // Points: bottom is the fixed first layer (grey), top is amber (height only), interior points teal.
    int hovered_point = -1;
    for (size_t i = 0; i < m_points.size(); ++i) {
        const ImVec2 c       = to_screen(m_points[i].z_mm, m_points[i].height_mm);
        const bool   locked  = point_is_locked(int(i));
        const bool   is_top  = (i + 1 == m_points.size());
        const bool   dragged = (int(i) == m_dragging_point);
        if (!locked && hovered && std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) <= 8.0f)
            hovered_point = int(i);
        const bool  lit = dragged || hovered_point == int(i);
        const float r   = dragged ? 6.5f : (lit ? 6.0f : 4.5f);
        const ImU32 col = locked ? col_locked : (is_top ? col_end : col_curve);
        dl->AddCircleFilled(c, r, col);
        dl->AddCircle(c, r + 1.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
    }

    dl->AddRect(p0, ImVec2(p0.x + width, p0.y + height), col_border, 6.0f, 0, 1.0f);

    m_hover_point = hovered ? hovered_point : -1;

    // Drag lifecycle: start on click-on-point, live-move while held, commit on release. The release is detected
    // through IsItemDeactivated() (ImGui's own active-id state), not a raw mouse-up flag that can be missed.
    if (m_dragging_point < 0 && hovered_point > 0 && io.MouseClicked[0]) {
        m_dragging_point = hovered_point;
    } else if (hovered && hovered_point < 0 && m_dragging_point < 0 && io.MouseClicked[0]) {
        // Click on an empty area: add a point, sorted into place by z.
        const auto [h, z]     = from_screen(io.MousePos);
        size_t     insert_at  = m_points.size();
        for (size_t i = 0; i < m_points.size(); ++i)
            if (z < m_points[i].z_mm) {
                insert_at = i;
                break;
            }
        if (insert_at > 0 && insert_at < m_points.size()) {
            const double z_lo = m_points[insert_at - 1].z_mm + 0.02;
            const double z_hi = m_points[insert_at].z_mm - 0.02;
            if (z_hi > z_lo) {
                const double z_clamped = std::clamp(z, z_lo, z_hi);
                m_points.insert(m_points.begin() + std::ptrdiff_t(insert_at), ALHPoint{ z_clamped, std::clamp(h, h_min, h_max), 0.0 });
                commit("Precision layer height - Add point");
            }
        }
    }

    if (m_dragging_point >= 0 && ImGui::IsItemActive()) {
        const auto [h, z] = from_screen(io.MousePos);
        ALHPoint&  pt     = m_points[size_t(m_dragging_point)];
        const bool is_end = (m_dragging_point == 0 || size_t(m_dragging_point + 1) == m_points.size());
        if (!is_end) {
            const double z_lo = m_points[size_t(m_dragging_point - 1)].z_mm + 0.02;
            const double z_hi = m_points[size_t(m_dragging_point + 1)].z_mm - 0.02;
            if (z_hi > z_lo)
                pt.z_mm = std::clamp(z, z_lo, z_hi);
        }
        pt.height_mm  = std::clamp(h, h_min, h_max);
        m_hover_point = m_dragging_point;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame(); // keep repainting every frame while held
    }
    if (m_dragging_point >= 0 && band_deactivated) {
        commit("Precision layer height - Move point");
        m_dragging_point = -1;
    }

    // Delete: right-click a point (the two endpoints can't be deleted).
    if (hovered && hovered_point > 0 && size_t(hovered_point) + 1 < m_points.size() && io.MouseClicked[1]) {
        m_points.erase(m_points.begin() + hovered_point);
        m_hover_point = -1;
        commit("Precision layer height - Delete point");
    }

    // Numeric feedback for the point being moved or hovered.
    const int shown_idx = (m_dragging_point >= 0) ? m_dragging_point : m_hover_point;
    if (shown_idx >= 0 && size_t(shown_idx) < m_points.size()) {
        const ALHPoint& pt    = m_points[size_t(shown_idx)];
        const ImVec2    c     = to_screen(pt.z_mm, pt.height_mm);
        const std::string label = "Z " + format(float(pt.z_mm), 2) + " mm   H " + format(float(pt.height_mm), 3) + " mm";
        const ImVec2    tsz   = ImGui::CalcTextSize(label.c_str());
        const ImVec2    lp(std::clamp(c.x - tsz.x * 0.5f, p0.x + 8.f, p0.x + width - tsz.x - 8.f), c.y - tsz.y - 10.0f);
        dl->AddRectFilled(ImVec2(lp.x - 4.f, lp.y - 2.f), ImVec2(lp.x + tsz.x + 4.f, lp.y + tsz.y + 2.f), IM_COL32(20, 22, 26, 235), 4.f);
        dl->AddRect(ImVec2(lp.x - 4.f, lp.y - 2.f), ImVec2(lp.x + tsz.x + 4.f, lp.y + tsz.y + 2.f), col_curve, 4.f);
        dl->AddText(lp, IM_COL32(240, 240, 240, 255), label.c_str());
    }
}

void GLGizmoPrecisionALH::on_render_input_window(float x, float y, float bottom_limit)
{
    ensure_session();

    const float win_w = 380.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    ImGui::PushTextWrapPos(win_w - 16.f);

    if (!m_have_session) {
        ImGui::TextWrapped("%s", _u8L("Select a single object to edit its layer height curve.").c_str());
    } else {
        ImGui::TextWrapped("%s", _u8L("Click to add a point, drag to move it, right-click to delete it. "
                                      "The bottom point is the fixed first layer and the top point can "
                                      "only move in height.").c_str());
        if (m_rescaled_on_load) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.15f, 1.0f));
            ImGui::TextWrapped("%s", _u8L("The object's height changed: the saved curve has been stretched to the new "
                                          "height. Layer heights are untouched; only the Z of each point moved.").c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        render_curve_editor(win_w - 16.0f, 220.0f);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Text("%s", (_u8L("Min layer height") + ": " + format(float(m_slicing_params.min_layer_height), 3) + " mm").c_str());
        ImGui::Text("%s", (_u8L("Max layer height") + ": " + format(float(m_slicing_params.max_layer_height), 3) + " mm").c_str());

        if (m_points.size() > 2) {
            ImGui::Separator();
            ImGui::TextUnformatted(_u8L("Tension per segment").c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", _u8L("(0 = straight, 1 = smooth)").c_str());
            // Range label in a measured column, slider width set explicitly (an overflowing item would widen
            // an AlwaysAutoResize window).
            std::vector<std::string> range_lbl(m_points.size() - 1);
            float                    range_w = 0.f;
            for (size_t i = 0; i + 1 < m_points.size(); ++i) {
                range_lbl[i] = format(float(m_points[i].z_mm), 1) + " - " + format(float(m_points[i + 1].z_mm), 1) + " mm";
                range_w      = std::max(range_w, ImGui::CalcTextSize(range_lbl[i].c_str()).x);
            }
            range_w += ImGui::GetStyle().ItemSpacing.x;
            const float slider_w = win_w - 16.f - range_w - 2.f * ImGui::GetStyle().ItemSpacing.x;
            for (size_t i = 0; i + 1 < m_points.size(); ++i) {
                float t = float(m_points[i].tension);
                ImGui::TextDisabled("%s", range_lbl[i].c_str());
                ImGui::SameLine(range_w);
                ImGui::SetNextItemWidth(slider_w);
                const std::string label = "##tension" + std::to_string(i);
                if (ImGui::SliderFloat(label.c_str(), &t, 0.0f, 1.0f)) {
                    m_points[i].tension = std::clamp(double(t), 0.0, 1.0);
                    m_parent.set_as_dirty();
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                    commit("Precision layer height - Tension");
            }
        }

        ImGui::Separator();
        if (ImGui::Button(_u8L("Reset").c_str())) {
            seed_flat_profile();
            commit("Precision layer height - Reset");
        }
    }

    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
}

}} // namespace Slic3r::GUI
