// Height Adaptive Effects gizmo.
// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0 -- lean port: effect list + Z->value curve editor over the
// object's real layer bands, for sparse infill / wall line widths and the fuzzy skin parameters.
// (The undocumented XY expansion effect, effect presets and the Libre Mode gating are not ported.)
#ifndef slic3r_GLGizmoHeightAdaptiveEffects_hpp_
#define slic3r_GLGizmoHeightAdaptiveEffects_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Feature/HeightAdaptive/HeightCurve.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Each effect is one serialized curve in a hidden per-object config key (neotko_hae_*), so it travels in the
// 3mf and goes through undo/redo like any other object setting. An object with no curve slices exactly as before.
class GLGizmoHeightAdaptiveEffects : public GLGizmoBase
{
public:
    GLGizmoHeightAdaptiveEffects(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    struct EffectDef
    {
        const char*            config_key;
        const char*            label;
        const char*            base_key;  // the stock setting this curve overrides (seeds new curves)
        HeightAdaptive::Interp default_interp;
        bool                   mode_selectable;
        double                 range_lo;  // editor axis, in the effect's units
        double                 range_hi;
    };
    static const std::vector<EffectDef>& effects();

    struct EffectState
    {
        std::vector<HeightAdaptive::Node> nodes;  // fewer than 2 = effect off
        HeightAdaptive::Interp            interp = HeightAdaptive::Interp::Stepped;
        bool                              added  = false; // the effect is in this object's list
    };

    bool                     m_have_session       = false;
    int                      m_object_idx         = -1;
    double                   m_object_height      = 0.;
    double                   m_first_layer_height = 0.2;
    SlicingParameters        m_slicing_params;
    std::vector<double>      m_layer_tops;  // top Z of every real layer, object-relative (same axis the engine uses)
    std::vector<EffectState> m_states;      // parallel to effects()

    int  m_active_effect = -1;
    int  m_add_choice    = 0;
    int  m_quantize_bands = 6;
    bool m_show_points   = false;

    int m_drag_node  = -1;
    int m_hover_node = -1;

    GLModel m_band_model;
    double  m_band_key = -1.;

    void   ensure_session();
    void   rebuild_layer_bands();
    void   load_curves_from_object();
    double base_value(size_t effect_idx) const;
    double snap_z(double z) const;
    int    layers_between(double z_lo, double z_hi) const;
    HeightAdaptive::HeightCurve curve_of(size_t effect_idx) const;
    HeightAdaptive::Interp      interp_of(size_t effect_idx) const;
    std::vector<size_t>         listed_effects() const;
    void   add_effect(size_t effect_idx);
    void   remove_effect(size_t effect_idx);
    void   quantize_to_steps(size_t effect_idx, int bands);
    double misalignment_per_layer_mm(size_t effect_idx, double* out_pct) const;
    void   commit(const std::string& snapshot_name);
    void   build_band_model(double z_mm);
    void   render_editor(float width, float height);
};

}} // namespace Slic3r::GUI

#endif
