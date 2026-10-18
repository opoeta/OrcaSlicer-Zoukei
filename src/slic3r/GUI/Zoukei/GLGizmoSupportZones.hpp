// Orca Zoukei: Support Zones / Stumps gizmo, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// A reduced port of NeotkoCM's GLGizmoSupportZones (2.4.4 / 2.4.5). It builds SUPPORT_ENFORCER volumes that the
// classic support generator treats as "support zones" (libslic3r/Feature/SupportZones/SupportZoneEngine):
//  - Whole patch / Round / Square footprint: an aimed pillar, one solid from the picked downward surface to the
//    landing, leaning at the chosen angle and dropping plumb after the knee;
//  - Paint footprint: a block tree, a flat head over the painted faces, one or more stumps on the plate and nothing
//    in between (the engine grows that stretch toward the stumps).
// Plus a cut tool (a SUPPORT_BLOCKER marked neotko_support_cut that also cuts columns coming from above).
//
// Not ported (see the port report): re-editing a zone from its saved gesture, the elevation drawing and zone cards,
// the coverage (gap) map, per-zone roof filament, merging / subtracting cuts.
#ifndef slic3r_GUI_Zoukei_GLGizmoSupportZones_hpp_
#define slic3r_GUI_Zoukei_GLGizmoSupportZones_hpp_

#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace GUI {

class GLGizmoSupportZones : public GLGizmoBase
{
public:
    GLGizmoSupportZones(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    bool               on_is_activable() const override;
    void               on_render() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;
    void               on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    enum class FootShape : int { Patch = 0, Round = 1, Square = 2, Brush = 3 };

    // A stump of a block tree (or the landing of a pillar), in WORLD coordinates.
    struct StumpSpot
    {
        Vec3d p      { Vec3d::Zero() };
        bool  on_bed { true };
    };

    // One row of the zone list: one SUPPORT_ENFORCER volume of the selected object.
    struct ZoneRow
    {
        int         volume_idx { -1 };
        std::string name;
        bool        is_zone { false };   // carries neotko_zone_lean_deg (made by this tool)
        double      lean_deg { 0. };
        size_t      lit { 0 };           // probe cells over downward surface
        bool        sterile { true };
    };

    // The footprint as a 2D mask in WORLD XY, plus the plane of the picked face (fallback roof) and the height
    // map of the surface under it (NaN where there is none). Each mask owns its height map.
    struct ZoneMask
    {
        ExPolygons area;
        Vec3d      seed_p { Vec3d::Zero() };
        Vec3d      seed_n { Vec3d(0., 0., -1.) };
        double     z_low  { 0. };
        double     z_high { 0. };
        bool       ok     { false };
        Vec2d              hm_origin { Vec2d::Zero() };
        double             hm_step   { 0.5 };
        int                hm_nx     { 0 };
        int                hm_ny     { 0 };
        std::vector<float> hm_z;
        std::vector<Vec3f> hm_n;
    };

    // The pillar axis: T (roof), [M] (lowest point of the roof), [K] (knee), B (foot).
    struct PillarAxis
    {
        double z_low { 0. }, z_high { 0. }, z_bot_ext { 0. }, z_knee { 0. };
        Vec2d  c_top { Vec2d::Zero() }, shift { Vec2d::Zero() };
        bool   wraps { false }, has_knee { false };
        std::vector<std::pair<double, Vec2d>> rings;
    };

    // Head (painted faces above the floor) and foot (painted faces below the floor) of a block tree.
    struct HeadFoot
    {
        bool       ok        { false };
        ExPolygons head;
        double     head_bot  { 0. };
        double     head_top  { 0. };
        ExPolygons foot;
        double     foot_bot  { 0. };
        double     foot_top  { 0. };
        double     floor_z   { 0. };
    };

    struct Candidate
    {
        int    facet_idx { -1 };
        double distance { 0. };
        Vec3d  world_pos { Vec3d::Zero() };
        Vec3d  world_normal { -Vec3d::UnitZ() };
        bool   past_threshold { false };
    };

    // --- object, raycaster ---------------------------------------------------------------------
    const ModelObject *current_object() const;
    int                current_object_idx() const;
    void               ensure_raycaster();
    void               clear_pick();

    std::unique_ptr<MeshRaycaster> m_raycaster;
    int                            m_raycaster_obj_idx = -1;
    int                            m_mesh_obj_idx      = -1;
    Transform3d                    m_world_trafo { Transform3d::Identity() };
    TriangleMesh                   m_mesh;
    std::vector<Vec3f>             m_face_normals;
    std::vector<Vec3i32>           m_face_neighbors;

    // --- picking ---------------------------------------------------------------------------------
    void             update_candidates(const Vec2d &mouse_pos);
    const Candidate *live_candidate() const;
    std::vector<int> collect_region(int facet_idx) const;
    bool             resolve_landing(const Vec2d &mouse_pos, Vec3d &out_pos, bool &out_on_bed);
    bool             resolve_landing_plumb(Vec3d &out_pos, bool &out_on_bed);
    float            overhang_normal_z_cut() const;

    std::vector<Candidate> m_candidates;
    int                    m_candidate_idx = 0;
    Vec2d                  m_cand_mouse { Vec2d(-1e9, -1e9) };
    Transform3d            m_cand_trafo { Transform3d::Identity() };
    Vec2d                  m_hover_mouse_pos { Vec2d::Zero() };
    bool                   m_have_hover_pos = false;
    Vec2d                  m_mouse_down_pos { Vec2d::Zero() };
    bool                   m_mouse_down = false;

    bool  m_target_pick_mode  = false;
    bool  m_landing_pick_mode = false;
    bool  m_stump_pick_mode   = false;
    bool  m_has_target        = false;
    int   m_target_facet_idx  = -1;
    Vec3d m_target_world_pos { Vec3d::Zero() };
    Vec3d m_target_world_normal { -Vec3d::UnitZ() };
    bool  m_has_landing       = false;
    bool  m_landing_locked    = false;
    Vec3d m_landing_world_pos { Vec3d::Zero() };
    bool  m_landing_on_bed    = true;

    // --- footprint -------------------------------------------------------------------------------
    FootShape m_foot_shape   = FootShape::Square;
    float     m_foot_size_mm = 8.f;
    bool      m_shape_covers = false;
    float     m_footprint_shrink_mm = 0.f;   // head edge (positive = inward)
    float     m_footprint_base_mm   = 0.f;   // foot edge
    float     m_lean_angle_deg      = 45.f;
    float     m_overhang_threshold_deg = 30.f;
    bool      m_threshold_seeded    = false;
    bool      m_show_overhangs      = true;
    // Zone options written to the volume (NeotkoCM default: all off).
    bool      m_opt_roof_only = false;
    bool      m_opt_solid     = false;
    bool      m_opt_land_only = false;

    const std::vector<int> &region_for(int seed_facet) const;
    mutable std::vector<int> m_region_cache;
    mutable int              m_region_cache_facet = -1;
    mutable Transform3d      m_region_cache_trafo { Transform3d::Identity() };
    mutable ExPolygons       m_region_area_cache;

    // Height map of the roof (one vertical ray per node, only under the footprint).
    void          ensure_heightmap(ZoneMask &mk, const BoundingBox &area_bb, double z_ref, int max_cells) const;
    static double z_at(const ZoneMask &mk, const Vec2d &p);
    static Vec3d  n_at(const ZoneMask &mk, const Vec2d &p);

    bool            build_mask(int seed_facet, const Vec2d &centre_xy, ZoneMask &out, int max_cells) const;
    const ZoneMask *mask() const;
    void            invalidate_patch();
    mutable ZoneMask    m_mask_cache;
    mutable int         m_mask_cache_facet = -1;
    mutable int         m_mask_cache_shape = -1;
    mutable float       m_mask_cache_size  = -1.f;
    mutable Vec2d       m_mask_cache_centre { Vec2d(1e9, 1e9) };
    mutable size_t      m_mask_cache_stamp = size_t(-1);
    mutable Transform3d m_mask_cache_trafo { Transform3d::Identity() };
    // Bumped every time the mask is rebuilt; the footprint outline is cached against it.
    mutable size_t             m_mask_version = 0;
    mutable std::vector<Vec2d> m_foot_cache;
    mutable size_t             m_foot_cache_version = size_t(-1);
    mutable float              m_foot_cache_shrink  = 1e9f;

    static std::vector<Vec2d> resample_ring(const ExPolygons &area, int n_pts);
    std::vector<Vec2d>        ring_outline_world(double edge_mm) const;
    std::vector<Vec2d>        target_footprint_world() const;
    Vec2d                     footprint_centroid() const;
    double                    max_inset_mm() const;
    double                    max_outset_mm() const;

    // --- brush (block tree) ----------------------------------------------------------------------
    size_t     paint_facets(int seed, const Vec3d &hit_world, double radius, bool erase);
    void       clear_painted();
    ExPolygons painted_area_world() const;
    bool       paint_at(const Vec2d &mouse_pos, bool erase);
    void       repaint_from_stamps();
    bool       block_tree_mode() const { return m_foot_shape == FootShape::Brush; }

    struct Stamp { Vec3d p { Vec3d::Zero() }; double r { 4. }; };
    std::vector<char>             m_painted;
    std::vector<int>              m_painted_list;
    mutable std::vector<uint32_t> m_visit_stamp;
    mutable uint32_t              m_visit_epoch = 0;
    std::vector<Stamp>            m_stamps;
    size_t                        m_stamp_stamp = 0;
    bool                          m_painting    = false;
    bool                          m_paint_erase = false;
    bool                          m_paint_overhang_only = true;
    float                         m_paint_cut_deg = 30.f;
    mutable ExPolygons            m_painted_area;
    mutable size_t                m_painted_area_stamp = size_t(-1);
    mutable Transform3d           m_painted_area_trafo { Transform3d::Identity() };

    std::vector<StumpSpot> all_stumps() const;
    const HeadFoot        &head_foot() const;
    double                 block_tree_gap_mm() const;
    std::vector<StumpSpot> m_extra_stumps;
    float                  m_stump_size_mm = 8.f;
    static constexpr double STUMP_HEIGHT_MM = 2.0;
    mutable HeadFoot    m_hf_cache;
    mutable size_t      m_hf_stamp { size_t(-1) };
    mutable Transform3d m_hf_trafo { Transform3d::Identity() };
    mutable double      m_hf_floor { 0. };
    mutable double      m_hf_fbot  { 0. };

    // --- the solid ---------------------------------------------------------------------------------
    bool pillar_axis(PillarAxis &out) const;
    bool build_pillar_mesh(TriangleMesh &out_world) const;
    bool build_block_tree_mesh(TriangleMesh &out_world) const;

    double support_line_width_mm() const;
    double worst_layer_height_mm() const;
    double lean_top_z() const;
    double reach_radius_mm(double z_landing) const;
    bool   landing_out_of_reach() const;

    SupportType effective_support_type() const;
    bool        support_style_not_snug() const;
    void        ensure_snug_style();
    bool        seed_support_defaults(ModelObject &mo);
    void        apply_only_my_zones(bool only_mine);
    bool        m_forced_snug = false;

    // --- preview / rendering -------------------------------------------------------------------
    void build_face_model(GLModel &model, int facet_idx, const Vec2d &centre_xy, bool hover, const ColorRGBA &col);
    void render_pick_overlays();
    void update_preview();
    void render_preview();
    void render_landing_marks();

    GLModel   m_hover_model;
    int       m_hover_model_facet  = -1;
    bool      m_hover_model_past   = false;
    Vec2d     m_hover_model_centre { Vec2d::Zero() };
    int       m_hover_model_key    = -1;
    size_t    m_hover_model_stamps = size_t(-1);
    GLModel   m_target_model;
    int       m_target_model_facet = -1;
    int       m_target_model_key   = -1;
    size_t    m_target_model_stamps = size_t(-1);
    GLModel   m_preview_model;
    bool      m_preview_dirty = true;
    GLModel   m_marks_model;

    // --- zones -------------------------------------------------------------------------------------
    void rebuild_zone_rows();
    void create_pillar();
    void delete_zone(int volume_idx);
    std::string current_gesture_json(const Transform3d &inst, const Vec3d &lock_offset) const;
    std::vector<ZoneRow> m_zones;
    bool                 m_zones_dirty = true;

    // --- cut ----------------------------------------------------------------------------------------
    bool               m_cut_mode = false;
    std::vector<Vec2d> m_cut_points;
    GLModel            m_cut_overlay;
    static constexpr double CUT_CLOSE_RADIUS_PX = 8.;
    static constexpr double CLICK_SLOP_PX       = 4.0;
    void render_cut_polygon();
    bool build_cut_mesh(const std::vector<Vec2d> &screen_pts, TriangleMesh &out_world) const;
    void create_cut(std::vector<Vec2d> screen_pts);

    // --- panel -------------------------------------------------------------------------------------
    void render_panel();
    void render_zone_list();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_GLGizmoSupportZones_hpp_
