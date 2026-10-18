// Precision Adaptive Layer Height gizmo.
// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0 -- lean port: the point/tension curve editor only
// (the "Adapt to Color" and "Slope recolor" WIP parts are not ported).
#ifndef slic3r_GLGizmoPrecisionALH_hpp_
#define slic3r_GLGizmoPrecisionALH_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Slicing.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Point-based curve editor for ModelObject::layer_height_profile. Exact (Z, height) control points with a
// per-segment tension (0 = straight line, 1 = monotone cubic that never overshoots the segment endpoints).
// Does not touch the stock "Variable layer height" tool.
class GLGizmoPrecisionALH : public GLGizmoBase
{
public:
    GLGizmoPrecisionALH(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    struct ALHPoint
    {
        double z_mm;
        double height_mm;
        double tension; // tension of the segment starting at this point, [0,1]; unused on the last point
    };

    bool                   m_have_session       = false;
    int                    m_object_idx         = -1;
    double                 m_object_height      = 0.0;   // SlicingParameters::object_print_z_uncompensated_height()
    double                 m_first_layer_height = 0.2;   // the profile MUST start with [0, first layer height]
    SlicingParameters      m_slicing_params;
    std::vector<ALHPoint>  m_points;                     // sorted by z; front().z == 0, back().z == object height
    bool                   m_rescaled_on_load   = false; // saved curve was stretched to a new object height

    int m_dragging_point = -1; // index into m_points, -1 = none
    int m_hover_point    = -1;

    GLModel m_band_model;     // translucent slab on the object showing the affected Z
    double  m_band_key = -1.0;

    bool point_is_locked(int idx) const { return idx == 0; }

    void ensure_session();
    void seed_flat_profile();
    void load_points_from_profile(const std::vector<coordf_t>& profile);
    std::vector<double>   compute_tangents() const;
    double                blended_height(size_t seg_idx, double t, const std::vector<double>& tangents) const;
    std::vector<coordf_t> build_profile_vector() const;
    void commit(const std::string& snapshot_name);
    void build_band_model(int idx);
    void render_curve_editor(float width, float height);
};

}} // namespace Slic3r::GUI

#endif
