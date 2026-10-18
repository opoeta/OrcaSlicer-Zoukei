// Ported from OrcaFS-NeotkoCM (Neotko, AGPL-3.0): Align & Stack. Lean version, whole objects only.
#include "GLGizmoAlignStack.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Zoukei/LibreMode.hpp" // ported from OrcaFS-NeotkoCM (Neotko): Libre Mode switch
#include "libslic3r/Model.hpp"

#include <imgui/imgui.h>
#include <glad/gl.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

GLGizmoAlignStack::GLGizmoAlignStack(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoAlignStack::on_init()
{
    return true;
}

std::string GLGizmoAlignStack::on_get_name() const
{
    return _u8L("Align & Stack");
}

bool GLGizmoAlignStack::on_is_activable() const
{
    return !m_parent.get_selection().is_empty();
}

void GLGizmoAlignStack::on_set_state()
{
    if (get_state() == On) {
        seed_from_selection();
    } else if (get_state() == Off) {
        m_ordered.clear();
        m_ghost_mesh.reset();
        m_ghost_obj = -1;
        m_hover_op  = Op::None;
    }
}

void GLGizmoAlignStack::data_changed(bool /*is_serializing*/)
{
    prune();
    m_ghost_obj = -1; // the mesh may have changed; rebuilt on next hover
}

void GLGizmoAlignStack::seed_from_selection()
{
    m_ordered.clear();
    for (const auto& kv : m_parent.get_selection().get_content()) {
        if (m_ordered.size() >= 2)
            break;
        m_ordered.push_back((int)kv.first);
    }
}

void GLGizmoAlignStack::prune()
{
    const Model* model = m_parent.get_selection().get_model();
    const int    n     = model ? (int)model->objects.size() : 0;
    m_ordered.erase(std::remove_if(m_ordered.begin(), m_ordered.end(), [n](int i) { return i < 0 || i >= n; }), m_ordered.end());
}

const ModelObject* GLGizmoAlignStack::object_of(int idx) const
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || idx < 0 || idx >= (int)model->objects.size())
        return nullptr;
    const ModelObject* mo = model->objects[idx];
    return mo->instances.empty() ? nullptr : mo;
}

BoundingBoxf3 GLGizmoAlignStack::world_bbox(int idx) const
{
    BoundingBoxf3      out;
    const ModelObject* mo = object_of(idx);
    if (mo == nullptr)
        return out;
    for (size_t i = 0; i < mo->instances.size(); ++i)
        out.merge(mo->instance_bounding_box(i));
    return out;
}

void GLGizmoAlignStack::remove_pick(size_t i)
{
    if (i >= m_ordered.size())
        return;
    const int obj = m_ordered[i];
    m_ordered.erase(m_ordered.begin() + i);
    if (std::find(m_ordered.begin(), m_ordered.end(), obj) == m_ordered.end())
        m_parent.get_selection().remove_object((unsigned int)obj);
    m_ghost_obj = -1;
    m_parent.set_as_dirty();
}

void GLGizmoAlignStack::click_object(int idx)
{
    auto it = std::find(m_ordered.begin(), m_ordered.end(), idx);
    if (it != m_ordered.end()) {
        remove_pick((size_t)(it - m_ordered.begin()));
        return;
    }
    if (m_ordered.size() >= 2)
        remove_pick(1); // a third click replaces #2
    m_ordered.push_back(idx);
    m_parent.get_selection().add_object((unsigned int)idx, false);
    m_ghost_obj = -1;
    m_parent.set_as_dirty();
}

bool GLGizmoAlignStack::on_mouse(const wxMouseEvent& mouse_event)
{
    if (mouse_event.LeftDClick())
        return m_parent.get_first_hover_volume_idx() >= 0;
    if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
        const int hovered = m_parent.get_first_hover_volume_idx();
        if (hovered < 0)
            return false;
        const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
        if (hovered >= (int)volumes.size() || volumes[hovered] == nullptr)
            return false;
        const int obj_idx = volumes[hovered]->object_idx();
        if (object_of(obj_idx) == nullptr)
            return false;
        click_object(obj_idx);
        return true; // keep Orca from starting a drag/selection change
    }
    return false;
}

void GLGizmoAlignStack::translate_object(int idx, const Vec3d& delta)
{
    const ModelObject* mo = object_of(idx);
    if (mo == nullptr || delta.norm() < 1e-9)
        return;
    Selection& sel = m_parent.get_selection();
    for (size_t i = 0; i < mo->instances.size(); ++i)
        sel.translate((unsigned int)idx, (unsigned int)i, delta);
    m_parent.do_move("");
}

// Same maths for the click and for the ghost, so the preview cannot lie.
bool GLGizmoAlignStack::op_delta(Op op, Vec3d& delta) const
{
    delta = Vec3d::Zero();
    if (op == Op::None || m_ordered.size() < 2)
        return false;
    const BoundingBoxf3 a = world_bbox(m_ordered[0]);
    const BoundingBoxf3 b = world_bbox(m_ordered[1]);
    if (!a.defined || !b.defined)
        return false;
    auto place = [&](int axis, int dir) {
        if (m_flush_mode) {
            const double target = dir > 0 ? a.max(axis) : a.min(axis);
            delta(axis) = target - (dir > 0 ? b.max(axis) : b.min(axis));
        } else {
            const double gap = axis == 2 ? (double)m_gap_mm : 0.0;
            delta(axis) = dir > 0 ? (a.max(axis) + gap - b.min(axis)) : (a.min(axis) - gap - b.max(axis));
        }
    };
    switch (op) {
    case Op::XNeg:     place(0, -1); break;
    case Op::XPos:     place(0, +1); break;
    case Op::YNeg:     place(1, -1); break;
    case Op::YPos:     place(1, +1); break;
    case Op::OnTop:    place(2, +1); break;
    case Op::CenterXY: delta(0) = a.center()(0) - b.center()(0); delta(1) = a.center()(1) - b.center()(1); break;
    case Op::CenterZ:  delta(2) = a.center()(2) - b.center()(2); break;
    case Op::Bed:      delta(2) = -b.min.z(); break;
    default: return false;
    }
    return true;
}

void GLGizmoAlignStack::apply_op(Op op)
{
    Vec3d delta;
    if (!op_delta(op, delta))
        return;
    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Stack: place");
    // do_move() drops auto-drop objects back onto the bed, which would undo a stack: an object left
    // above the bed gets Auto drop OFF; "Drop to bed" turns it back on.
    if (Model* model = m_parent.get_selection().get_model()) {
        const int mover = m_ordered[1];
        if (mover >= 0 && mover < (int)model->objects.size()) {
            const BoundingBoxf3 b = world_bbox(mover);
            if (op == Op::Bed)
                model->objects[mover]->auto_drop = true;
            else if (b.defined && b.min.z() + delta.z() > 0.001)
                model->objects[mover]->auto_drop = false;
        }
    }
    translate_object(m_ordered[1], delta);
    m_ghost_obj = -1;
}

void GLGizmoAlignStack::on_render()
{
    prune();
    render_ghost();
}

void GLGizmoAlignStack::render_ghost()
{
    Vec3d delta;
    if (!op_delta(m_hover_op, delta))
        return;
    const int          idx = m_ordered[1];
    const ModelObject* mo  = object_of(idx);
    if (mo == nullptr)
        return;
    if (!m_ghost_mesh.is_initialized() || m_ghost_obj != idx || m_ghost_nvol != mo->volumes.size()) {
        m_ghost_mesh.reset();
        const TriangleMesh mesh = mo->raw_mesh();
        if (mesh.empty())
            return;
        m_ghost_mesh.init_from(mesh.its);
        m_ghost_mesh.set_color(ColorRGBA(0.18f, 0.84f, 0.77f, 0.50f));
        m_ghost_obj  = idx;
        m_ghost_nvol = mo->volumes.size();
    }
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d   placed = Transform3d::Identity();
    placed.translate(delta);
    placed = placed * mo->instances.front()->get_matrix();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * placed);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_ghost_mesh.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoAlignStack::on_render_input_window(float x, float y, float bottom_limit)
{
    prune();
    const float f_scale = m_parent.get_scale();
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGuiWrapper::push_toolbar_style(f_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const size_t n       = m_ordered.size();
    const bool   two     = n >= 2;
    Op           hovered = Op::None;

    for (size_t i = 0; i < n; ++i) {
        const ModelObject* mo   = object_of(m_ordered[i]);
        std::string        name = mo ? mo->name : std::string("?");
        if (name.size() > 12)
            name = name.substr(0, 11) + "~";
        const std::string label = "#" + std::to_string(i + 1) + " " + name + "##chip" + std::to_string(i);
        if (i > 0)
            ImGui::SameLine();
        if (ImGui::Button(label.c_str())) {
            remove_pick(i);
            break;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (i == 0 ? _u8L("#1 stays where it is.") : _u8L("#2 is the one that moves.")).c_str());
    }
    if (n > 0) {
        ImGui::SameLine();
        if (two) {
            if (ImGui::Button((_u8L("Swap") + "##swap").c_str()))
                std::swap(m_ordered[0], m_ordered[1]);
            ImGui::SameLine();
        }
        if (ImGui::Button((_u8L("Reset") + "##reset").c_str())) {
            while (!m_ordered.empty())
                remove_pick(m_ordered.size() - 1);
        }
    }
    if (n < 2) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.f);
        ImGui::TextUnformatted(_u8L("Click an object: it becomes #1 (stays). Click another: #2 (moves).").c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();

    if (ImGui::RadioButton((_u8L("Place against") + "##m0").c_str(), !m_flush_mode))
        m_flush_mode = false;
    ImGui::SameLine();
    if (ImGui::RadioButton((_u8L("Align flush") + "##m1").c_str(), m_flush_mode))
        m_flush_mode = true;

    m_imgui->disabled_begin(!two);
    auto op_button = [&](const std::string& text, Op op, const std::string& tip) {
        if (ImGui::Button((text + "##op" + std::to_string((int)op)).c_str()) && two)
            apply_op(op);
        if (ImGui::IsItemHovered()) {
            hovered = op;
            ImGui::SetTooltip("%s", tip.c_str());
        }
    };
    op_button("X-", Op::XNeg, _u8L("Left side of #1 (X-)."));
    ImGui::SameLine();
    op_button("X+", Op::XPos, _u8L("Right side of #1 (X+)."));
    ImGui::SameLine();
    op_button("Y-", Op::YNeg, _u8L("Front side of #1 (Y-)."));
    ImGui::SameLine();
    op_button("Y+", Op::YPos, _u8L("Back side of #1 (Y+)."));
    ImGui::SameLine();
    op_button(_u8L("On top"), Op::OnTop, _u8L("Top of #1 (Z+). In Place mode it rests on #1 with the Z gap."));
    op_button(_u8L("Center XY"), Op::CenterXY, _u8L("Center #2 on #1 in X and Y."));
    ImGui::SameLine();
    op_button(_u8L("Center Z"), Op::CenterZ, _u8L("Center #2 on #1 in Z."));
    ImGui::SameLine();
    op_button(_u8L("Drop to bed"), Op::Bed, _u8L("Drop #2 onto the bed (Z = 0)."));
    m_imgui->disabled_end();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_u8L("Z gap (mm)").c_str());
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetFontSize() * 6.f);
    ImGui::InputFloat("##alignstack_gap", &m_gap_mm, 0.0f, 0.0f, "%.2f");
    ImGui::PopItemWidth();
    m_gap_mm = std::max(0.f, std::min(m_gap_mm, 50.f));

    if (hovered != m_hover_op) {
        m_hover_op = hovered;
        m_parent.set_as_dirty();
    }

    // Libre Mode (ported from OrcaFS-NeotkoCM, Neotko): the opt-in switch and the Snap & Drag options live next to the stacking tools.
    ImGui::Separator();
    LibreMode::render_imgui_options();

    GizmoImguiEnd();
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}

}} // namespace Slic3r::GUI
