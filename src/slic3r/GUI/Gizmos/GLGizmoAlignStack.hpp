// Align & Stack gizmo: lean port of GLGizmoAlignStack from OrcaFS-NeotkoCM (Neotko, AGPL-3.0).
// Only the "place against / align flush" part is ported (no Construct, no booleans).
#ifndef slic3r_GLGizmoAlignStack_hpp_
#define slic3r_GLGizmoAlignStack_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/BoundingBox.hpp"

#include <vector>

namespace Slic3r {
class ModelObject;
namespace GUI {

// Relates exactly two objects: #1 (anchor, stays) and #2 (the one that moves). Objects are picked by
// clicking them in the scene while the gizmo is open (a third click replaces #2).
class GLGizmoAlignStack : public GLGizmoBase
{
public:
    GLGizmoAlignStack(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent& mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    enum class Op { None, XNeg, XPos, YNeg, YPos, CenterXY, OnTop, CenterZ, Bed };

    std::vector<int> m_ordered;          // object indices: [0] = #1 anchor, [1] = #2 mover
    float            m_gap_mm     = 0.01f; // Z gap when stacking
    bool             m_flush_mode = false; // false = place against, true = align flush
    Op               m_hover_op   = Op::None;
    GLModel          m_ghost_mesh;
    int              m_ghost_obj  = -1;
    size_t           m_ghost_nvol = 0;

    const ModelObject* object_of(int idx) const;
    BoundingBoxf3      world_bbox(int idx) const;
    bool               op_delta(Op op, Vec3d& delta) const;
    void               apply_op(Op op);
    void               translate_object(int idx, const Vec3d& delta);
    void               click_object(int idx);
    void               remove_pick(size_t i);
    void               seed_from_selection();
    void               prune();
    void               render_ghost();
};

}} // namespace Slic3r::GUI

#endif
