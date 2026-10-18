#ifndef libslic3r_BumpMapping_hpp_
#define libslic3r_BumpMapping_hpp_

// Bump Mapping for Orca Zoukei: a PNG becomes physical relief.
//   * Wall bump: perimeter offset (Arachne walls only), outward-only, slope-limited across layers.
//   * ZBump:     Z modulation of the top solid infill, sampled point by point along every fill line.
// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0 (Feature/TextureBump and Feature/ZBump).
// Simplified for this fork: one config per object (PrintObjectConfig), no painted zones, no
// reinforcement passes. ZBump reuses the existing per-point Z path machinery of the Z anti-aliasing
// feature (ExtrusionPath::z_contoured), so GCode.cpp needs no change. Everything is off by default
// and does nothing (G-code unchanged) while the *_enabled keys are false.

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {
class PerimeterGenerator;
class PrintObject;
class PrintObjectConfig;
class PrintRegionConfig;
class Layer;
} // namespace Slic3r

namespace Slic3r::Feature::Bump {

// Decoded greyscale image cached by path (any 8-bit PNG: colour is converted to luma).
std::shared_ptr<const png::ImageGreyscale> load_image(const std::string &path);
// Bilinear sample at (u, v), both wrapped into [0,1). Result in [0,1].
double sample_bilinear(const png::ImageGreyscale &img, double u, double v);

// ---------------------------------------------------------------- wall bump

struct WallBumpConfig
{
    bool        enabled = false;
    std::string image_path;
    double      thickness_mm      = 0.4;
    double      scale_mm          = 20.0; // vertical (Z) size of one image tile
    int         repeat_u          = 1;
    int         projection        = 0;    // 0 planar, 1 cylindrical, 2 cubic (4 faces)
    double      point_distance_mm = 0.4;
    double      max_angle_rad     = 0.7853981633974483;

    bool operator==(const WallBumpConfig &r) const
    {
        return enabled == r.enabled && image_path == r.image_path && thickness_mm == r.thickness_mm && scale_mm == r.scale_mm &&
               repeat_u == r.repeat_u && projection == r.projection && point_distance_mm == r.point_distance_mm &&
               max_angle_rad == r.max_angle_rad;
    }
    bool operator!=(const WallBumpConfig &r) const { return !(*this == r); }
};

WallBumpConfig resolve_wall_bump_config(const PrintObjectConfig &object_config);
// True only when the key is on AND the settings are usable (image set, positive sizes).
bool           wall_bump_active(const PrintObjectConfig &object_config);

// Per-object table [layer][column] of outward displacement in mm, already slope limited.
class WallBumpTable
{
public:
    void   build(const WallBumpConfig &cfg, const BoundingBoxf3 &object_bounds_mm, const std::vector<coordf_t> &layer_z);
    double sample(double u_canonical, int layer_idx) const;
    bool   empty() const { return m_num_columns == 0 || m_num_layers == 0; }
    const BoundingBoxf3  &bounds() const { return m_bounds_mm; }
    const WallBumpConfig &config() const { return m_config; }

private:
    size_t              m_num_columns = 0;
    size_t              m_num_layers  = 0;
    std::vector<double> m_values;
    BoundingBoxf3       m_bounds_mm;
    WallBumpConfig      m_config;
};

// Lazily built (thread safe) table for the object, nullptr while wall bump is off or unusable.
// The pointer stays valid until the object's bump settings or layers change.
const WallBumpTable *get_wall_table(const PrintObject *object);

// Applied right after fuzzy skin on Arachne walls. `total_loops` is the 0-based index of the
// innermost wall of the island (the innermost wall is left at its nominal position).
void apply_wall_bump(Arachne::ExtrusionLine *extrusion, const PerimeterGenerator &perimeter_generator, bool is_contour, int total_loops);

// ---------------------------------------------------------------- ZBump (top surface)

struct ZBumpConfig
{
    bool        enabled = false;
    std::string image_path;
    double      thickness_mm      = 0.4;
    double      scale_mm          = 20.0;
    int         repeat            = 1;
    double      offset_x_mm       = 0.0;
    double      offset_y_mm       = 0.0;
    double      edge_margin_mm    = 3.0;
    double      max_slope         = 0.5;
    double      relief_segment_mm = 1.0;

    bool operator==(const ZBumpConfig &r) const
    {
        return enabled == r.enabled && image_path == r.image_path && thickness_mm == r.thickness_mm && scale_mm == r.scale_mm &&
               repeat == r.repeat && offset_x_mm == r.offset_x_mm && offset_y_mm == r.offset_y_mm && edge_margin_mm == r.edge_margin_mm &&
               max_slope == r.max_slope && relief_segment_mm == r.relief_segment_mm;
    }
    bool operator!=(const ZBumpConfig &r) const { return !(*this == r); }
};

ZBumpConfig resolve_zbump_config(const PrintObjectConfig &object_config);

class ZBumpHeightMap
{
public:
    void   build(const ZBumpConfig &cfg, const BoundingBoxf3 &object_bounds_mm);
    double sample_xy(double x_mm, double y_mm) const;
    bool   empty() const { return m_cols == 0 || m_rows == 0; }
    const ZBumpConfig &config() const { return m_config; }

private:
    size_t              m_cols = 0;
    size_t              m_rows = 0;
    std::vector<double> m_values;
    BoundingBoxf3       m_bounds_mm;
    ZBumpConfig         m_config;
};

// Safe single-pass relief height: 0.8 x nozzle - layer height.
double compute_pass_cap_mm(double nozzle_diameter_mm, double layer_height_mm);

// Called from Layer::make_fills() for the fill entities [first_entity, end) just produced for one
// top-surface expolygon. Gives the paths a per-vertex Z (z_contoured) in place. Returns without
// touching anything when ZBump is off, when Z anti-aliasing is on for the region, or on layer 0.
void apply_zbump_to_range(const Layer &layer, const PrintRegionConfig &region_config, const ExPolygon &top_surface,
                          ExtrusionEntitiesPtr &entities, size_t first_entity);

} // namespace Slic3r::Feature::Bump

#endif // libslic3r_BumpMapping_hpp_
