// Support Zones / Stumps - ported from OrcaFS-NeotkoCM (Neotko), NEOTKO_SUPPORTZONES_TAG s284..s343g.
//
// A support zone is a SUPPORT_ENFORCER volume written by the Support Zones gizmo. Besides saying WHAT
// to hold up (like any enforcer), the part of the block below the object says WHERE the column may
// come down: the classic support generator walks the accumulated projection of each zone along the
// axis the block describes (the corridor), at most dz * tan(lean) per layer.
//
// A block with a vertical gap between its top band (the head) and its bottom band (the stumps) is a
// "block tree": between the two bands the column is steered toward the nearest stump and contracts
// onto it, spending the same per-layer budget.
//
// Gate: an enforcer volume is a zone ONLY when it carries the hidden key neotko_zone_lean_deg. Plain
// enforcers keep the stock behaviour, so with no zones on the plate the G-code is byte-identical.
//
// Per-zone filament (NeotkoCM's support families, F4) is NOT ported: zone areas print with the
// object's support filaments.

#ifndef slic3r_SupportZoneEngine_hpp_
#define slic3r_SupportZoneEngine_hpp_

#include <functional>
#include <utility>
#include <vector>

#include "../../Point.hpp"
#include "../../Polygon.hpp"

namespace Slic3r {

class Layer;
class ModelObject;
class ModelVolume;
class PrintObject;

namespace SupportZones {

// SUPPORT_ENFORCER volume that carries neotko_zone_lean_deg (written by the gizmo).
bool is_zone_volume(const ModelVolume &v);
// SUPPORT_BLOCKER volume with neotko_support_cut = true: it also cuts the columns coming from above.
bool is_cut_volume(const ModelVolume &v);
bool object_has_zones(const ModelObject &mo);

// One support zone, sliced on its own (no union across volumes, identity kept).
struct ZoneSlices
{
    // Position among the zones of the object = creation order. Deterministic priority.
    size_t                 priority { 0 };
    const ModelVolume     *model_volume { nullptr };
    // Lean of the zone in degrees, never 0 here (0 in the config means "default").
    double                 lean_deg { 0. };
    // The block IS the column (its section is printed on every layer it crosses).
    bool                   solid { false };
    // The column only lands in its last layers, never on the shelves it crosses.
    bool                   land_only { false };
    // Seeds contact only where the block ends at the top (its roof), not where it passes through.
    bool                   roof_only { false };
    // Indexed by object layer.
    std::vector<Polygons>  slices;
    // Per object layer: 1 where the block really ends at the top. Computed once, compensating the
    // lean of the block (the layer above is translated by the block's own shift before the diff).
    std::vector<char>      roof_layer;
};

// Slices every zone of the object at the object layers. Empty when the object has no zone.
std::vector<ZoneSlices> slice_zones(const PrintObject &object);

// The cut blockers, united, per object layer. Empty when there is none.
std::vector<Polygons> slice_cutters(const PrintObject &object);

// Roof bands of the roof-only zones per object layer, expanded by `roof_expand` (scaled). The slices
// of those zones are subtracted from `enforcers_layers`, so they stop seeding the classic way along
// their whole height. Returns an empty vector when there is no roof-only zone.
std::vector<Polygons> roof_only_seeds(const PrintObject &object, std::vector<Polygons> &enforcers_layers, float roof_expand);

// The per-layer descent of the zones inside PrintObjectSupportMaterial::bottom_contact_layers_and_layer_support_areas().
// Every method is a no-op when empty().
class ZoneDescent
{
public:
    // Projects raw areas to the support grid: returns { area printed on this layer, carry that keeps descending }.
    using ProjectFn = std::function<std::pair<Polygons, Polygons>(const Layer &layer, const Polygons &raw)>;

    ZoneDescent(const PrintObject &object, double support_line_width_mm, double support_closing_radius_mm,
                double gap_xy_mm, bool buildplate_only);

    bool empty() const { return m_zones.empty(); }

    // 1) Top of the layer loop, BEFORE the new contacts of this layer are merged: walks every zone
    //    accumulator from layer_id + 1 down to layer_id along its corridor (or toward its stumps).
    //    What does not fit the stumps is handed to the general streams and falls plumb.
    void descend(int layer_id, const Layer &layer, double dz_mm, Polygons &overhangs_projection, Polygons &enforcers_projection);

    // 2) Inside the contact collection: carve the pieces that belong to a zone out of the general
    //    stream (the regularised halo included) and route them to the zone accumulators.
    void route_contacts(size_t idx_object_layer, Polygons &polygons_new, Polygons &enforcers_new);

    // 3) True when some zone still has work on this layer (its accumulator or its own section).
    bool layer_has_work(int layer_id) const;

    // 4) Moves the accumulators to this layer's raw areas and adds the block sections of solid /
    //    roof-only zones, trimmed by the object with the support XY gap.
    void begin_layer(int layer_id, const Layer &layer);
    bool has_raw() const;

    // 5) What the bottom contact detector must see: the general source plus the zone columns
    //    (land_only zones only in their last layers).
    Polygons bottom_contact_source(int layer_id, const Polygons &base) const;

    // 6) Grid projection per zone, carry clamp, exclusive partition between different recipes and
    //    merge into the printed area of the layer.
    void project_and_merge(int layer_id, const Layer &layer, const ProjectFn &project, Polygons &layer_support_area);

private:
    struct Stump
    {
        Polygons poly;
        Vec2d    c { Vec2d::Zero() };
    };
    struct Zone
    {
        ZoneSlices         s;
        size_t             family { 0 };
        int                bottom_layer { -1 };
        int                top_layer { -1 };
        int                stump_top { -1 };
        bool               guided { false };
        std::vector<Stump> stumps;
        Polygons           projection;     // the accumulator that keeps descending
        Polygons           projection_raw; // this layer's raw area
    };

    bool seeds_section(const Zone &z, size_t layer_id) const;

    std::vector<Zone> m_zones;
    size_t            m_n_families { 0 };
    bool              m_buildplate_only { false };
    double            m_gap_xy_mm { 0. };
    float             m_zone_halo { 0.f };
    double            m_min_half_width { 0. };
};

} // namespace SupportZones
} // namespace Slic3r

#endif // slic3r_SupportZoneEngine_hpp_
