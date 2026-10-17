///|/ Ported from PrusaSlicer 2.9 (src/libslic3r/GCode/SeamPlacer.hpp)
///|/ Copyright (c) Prusa Research 2020 - 2022 Pavel Mikuš @Godrak, Lukáš Matěna @lukasmatena,
///|/ Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
// ORCA: PrusaSlicer 2.9's seam placer, run behind Orca's SeamPlacer when the print object asks for
// it (seam_algorithm = prusaslicer_2_9). It only picks the point a loop starts at: Orca's scarf joint,
// seam gap and wipe stay in GCode::extrude_loop(), so PrusaSlicer's own scarf seam is not ported.
// Orca's extra seam positions are mapped onto it: Aligned back = Aligned with front facing surfaces
// counted as more visible, Assemble zgap = Aligned, then the seams of a layer kept 2 mm apart.
#ifndef libslic3r_Seams29_SeamPlacer29_hpp_
#define libslic3r_Seams29_SeamPlacer29_hpp_

#include <functional>
#include <unordered_map>
#include <vector>

#include "libslic3r/GCode/Seams29/SeamAligned.hpp"
#include "libslic3r/GCode/Seams29/SeamPerimeters.hpp"
#include "libslic3r/GCode/Seams29/SeamChoice.hpp"
#include "libslic3r/GCode/Seams29/ModelVisibility.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {
class PrintObject;
class Layer;
class ExtrusionLoop;
}

namespace Slic3r::Seams {

using ObjectSeams =
    std::unordered_map<const PrintObject *, std::vector<std::vector<SeamPerimeterChoice>>>;
using ObjectLayerPerimeters = std::unordered_map<const PrintObject *, Perimeters::LayerPerimeters>;

struct Params
{
    double max_nearest_detour;
    double rear_tolerance;
    double rear_y_offset;
    Aligned::Params aligned;
    double max_distance{};
    unsigned random_seed{};
    double convex_visibility_modifier{};
    double concave_visibility_modifier{};
    Perimeters::PerimeterParams perimeter;
    ModelInfo::Visibility::Params visibility;
    bool staggered_inner_seams{};
    // ORCA: Assemble zgap - least distance between two seams of one layer.
    double assemble_min_distance{2.0};
};

class Placer
{
public:
    static Params get_params(const PrintObject &print_object);

    void init(
        const std::vector<const PrintObject *> &objects,
        const std::function<void(void)> &throw_if_canceled
    );

    bool handles(const PrintObject *print_object) const { return this->params.count(print_object) > 0; }

    // Where the loop should start. `overhang` receives the signed distance (mm) of that point to the
    // outline of the layer below, offset like the classic SeamPlacer's unsupported distance, or the
    // lowest float when there is no layer below.
    Point place_seam(const Layer *layer, const ExtrusionLoop &loop, const Point &last_pos, float &overhang) const;

    void clear();

private:
    std::unordered_map<const PrintObject *, Params> params;
    ObjectSeams seams_per_object;
    ObjectLayerPerimeters perimeters_per_layer;
};

} // namespace Slic3r::Seams

#endif // libslic3r_Seams29_SeamPlacer29_hpp_
