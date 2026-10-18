// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoArachne "Hybrid v2" wall recipe (NeoArachnePlan case E and
// NeoArachneInterior of NeotkoCM). See NeoArachneHybrid.hpp.
#include "NeoArachneHybrid.hpp"

#include "../Arachne/WallToolPaths.hpp"
#include "../Arachne/utils/ExtrusionLine.hpp"
#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../PerimeterGenerator.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../SurfaceCollection.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace Slic3r { namespace NeoArachne {

HybridConfig hybrid_config_from_object(const PrintObjectConfig &oc)
{
    HybridConfig c;
    c.allowed_overlap_pct = oc.neoarachne_overlap_pct.value;
    c.min_bead_width_pct  = oc.neoarachne_min_bead_pct.value;
    c.max_bead_width_pct  = oc.neoarachne_max_bead_pct.value;
    return c;
}

ExPolygons run_interior(PerimeterGenerator &g, const Polygons &outline_polys, int inset_count, const HybridConfig &cfg)
{
    if (inset_count <= 0 || outline_polys.empty())
        return union_ex(outline_polys);

    // Every bead is an interior bead for Arachne: the outer wall has already been printed by Classic.
    const coord_t perimeter_spacing = g.perimeter_flow.scaled_spacing();

    Arachne::WallToolPathsParams params = Arachne::make_paths_params(g.layer_id, *g.object_config, *g.print_config);

    // The params live in mm as plain floats (WallToolPaths scales them itself).
    const double nozzle_diameter = g.print_config->nozzle_diameter.get_at(std::max(0, g.config->outer_wall_filament_id.value - 1));
    if (nozzle_diameter > 0.0) {
        params.min_bead_width   = float(nozzle_diameter * cfg.min_bead_width_pct / 100.0);
        params.min_feature_size = float(nozzle_diameter * cfg.min_feature_pct / 100.0);
    }
    params.keep_short_tails = cfg.keep_short_tails;
    params.max_bead_width_pct = float(cfg.max_bead_width_pct);
    // Cap the inner widening: beads keep the nominal width and the residue becomes closure (gap fill) beads.
    params.neoarachne_enabled        = true;
    params.neoarachne_cap_widening   = true;
    params.neoarachne_pin_outer      = false;
    params.neoarachne_hysteresis_pct = 0.0;

    Arachne::WallToolPaths wtp(outline_polys, perimeter_spacing, perimeter_spacing, size_t(inset_count), 0, g.layer_height, params);
    const std::vector<Arachne::VariableWidthLines> &perimeters = wtp.getToolPaths();

    // Group the beads by island: getToolPaths() is sorted by inset index across ALL islands, which would make the
    // printer ring every island first and fill them afterwards. Each island bucket keeps its inset order and is
    // appended as one non-sortable entity, so the outer collection still chains the islands by proximity.
    const ExPolygons islands = union_ex(outline_polys);
    const size_t     n_islands = islands.size();
    std::vector<ExtrusionEntityCollection> per_island(n_islands);
    ExtrusionEntityCollection              shared_bucket;

    auto find_island = [&](const Arachne::ExtrusionLine &el) -> size_t {
        if (n_islands == 0 || el.junctions.empty())
            return n_islands;
        const Point &p = el.junctions.front().p;
        for (size_t i = 0; i < n_islands; ++i)
            if (islands[i].contains(p))
                return i;
        return n_islands;
    };

    for (const Arachne::VariableWidthLines &perimeter : perimeters) {
        for (const Arachne::ExtrusionLine &ext_line : perimeter) {
            if (ext_line.empty())
                continue;

            // Odd beads are the variable width closure beads between two walls: they behave like gap fill, so they
            // print at the gap infill speed and retraction rules, not as inner walls.
            const ExtrusionRole role = ext_line.is_odd ? erGapFill : erPerimeter;
            ExtrusionPaths      paths;
            extrusion_paths_append(paths, ext_line, role, g.perimeter_flow);
            if (paths.empty())
                continue;

            const int inset_idx = int(ext_line.inset_idx);
            for (ExtrusionPath &p : paths)
                p.inset_idx = inset_idx;

            const size_t bucket_idx = find_island(ext_line);
            ExtrusionEntityCollection &dst = (bucket_idx < n_islands) ? per_island[bucket_idx] : shared_bucket;

            if (ext_line.is_closed) {
                ExtrusionLoop loop(std::move(paths), elrDefault);
                // Keep the hole / contour distinction for the seam placer: sample it before forcing CCW.
                const bool is_hole = loop.is_clockwise();
                loop.make_counter_clockwise();
                loop.set_loop_role(is_hole ? elrHole : elrDefault);
                loop.inset_idx = inset_idx;
                dst.append(std::move(loop));
            } else {
                ExtrusionMultiPath mp;
                mp.paths.reserve(paths.size());
                mp.paths.emplace_back(std::move(paths.front()));
                mp.inset_idx = inset_idx;
                for (auto it = std::next(paths.begin()); it != paths.end(); ++it) {
                    if (mp.paths.back().last_point() != it->first_point()) {
                        dst.append(ExtrusionMultiPath(std::move(mp)));
                        mp = ExtrusionMultiPath();
                        mp.inset_idx = inset_idx;
                    }
                    mp.paths.emplace_back(std::move(*it));
                }
                dst.append(ExtrusionMultiPath(std::move(mp)));
            }
        }
    }

    for (ExtrusionEntityCollection &bucket : per_island) {
        if (bucket.empty())
            continue;
        bucket.no_sort = true;
        g.loops->append(bucket);
    }
    if (!shared_bucket.empty()) {
        shared_bucket.no_sort = true;
        g.loops->append(shared_bucket);
    }

    // The area left for the infill. Arachne's inner contour can legitimately be empty (thin regions, few insets);
    // then fall back to the residue shrunk by the full wall thickness, which never overlaps the walls.
    const Polygons &arachne_reported = wtp.getInnerContour();
    if (!arachne_reported.empty())
        return union_ex(arachne_reported);
    return offset_ex(union_ex(outline_polys), -float(inset_count) * float(perimeter_spacing));
}

void run_hybrid(PerimeterGenerator &g)
{
    const PrintRegionConfig *original_cfg = g.config;
    const HybridConfig       cfg          = hybrid_config_from_object(*g.object_config);

    const int orig_wall_loops = original_cfg->wall_loops.value;
    if (orig_wall_loops < 1) {
        g.process_classic();
        return;
    }

    // A local copy: other LayerRegions of the same PrintRegion may be slicing in parallel, so the shared config
    // is never touched. Classic prints the outer wall only; Arachne integrates the gap fill; the thin-wall and
    // alternate-extra-wall logic would overlap with Arachne's own work in the interior.
    PrintRegionConfig modified_cfg = *original_cfg;
    modified_cfg.wall_loops.value = 1;
    std::fill(modified_cfg.gap_infill_speed.values.begin(), modified_cfg.gap_infill_speed.values.end(), 0.);
    modified_cfg.alternate_extra_wall.value = false;
    modified_cfg.detect_thin_wall.value     = false;

    ExPolygons original_slice;
    original_slice.reserve(g.slices->surfaces.size());
    for (const Surface &s : g.slices->surfaces)
        original_slice.push_back(s.expolygon);

    const size_t loops_before = g.loops->entities.size();
    g.config = &modified_cfg;
    g.process_classic();
    g.config = original_cfg;
    const size_t loops_after_classic = g.loops->entities.size();

    // The same "force a single wall" rules as the classic generator.
    int effective_walls = orig_wall_loops;
    const bool one_wall_first = original_cfg->only_one_wall_first_layer && original_cfg->bottom_shell_layers.value > 0;
    const bool one_wall_top   = original_cfg->only_one_wall_top && original_cfg->top_shell_layers.value > 0;
    if (g.layer_id == g.object_config->raft_layers && one_wall_first)
        effective_walls = 1;
    if (effective_walls > 1 && one_wall_top && g.upper_slices == nullptr)
        effective_walls = 1;
    if (effective_walls <= 1)
        return;

    // The residue for Arachne: the slice shrunk by the outer wall spacing, less the allowed overlap.
    const coord_t    ext_perimeter_spacing = g.ext_perimeter_flow.scaled_spacing();
    const double     overlap_frac          = std::clamp(cfg.allowed_overlap_pct / 100.0, 0.0, 1.0);
    const coord_t    residual_offset       = coord_t(double(ext_perimeter_spacing) * (1.0 - overlap_frac));
    const ExPolygons residual              = offset_ex(original_slice, -float(residual_offset));
    if (residual.empty())
        return; // nothing but the outer wall fits

    const ExPolygons arachne_inner = run_interior(g, to_polygons(residual), effective_walls - 1, cfg);

    // Classic left the infill area as if there were no inner walls: clip it to what Arachne left over.
    {
        Surfaces        clipped;
        const Surfaces &src = g.fill_surfaces->surfaces;
        clipped.reserve(src.size());
        for (const Surface &s : src) {
            ExPolygons isect = intersection_ex(ExPolygons{ s.expolygon }, arachne_inner);
            for (ExPolygon &ex : isect)
                clipped.emplace_back(Surface(s, std::move(ex)));
        }
        g.fill_surfaces->surfaces = std::move(clipped);
    }
    if (g.fill_no_overlap != nullptr && !g.fill_no_overlap->empty())
        *g.fill_no_overlap = intersection_ex(*g.fill_no_overlap, arachne_inner);

    // wall_sequence: classic emitted its block first and the interior second (outer -> inner), so only the other
    // sequences need the two blocks swapped. Inner-outer-inner is approximated as inner-outer.
    if (original_cfg->wall_sequence != WallSequence::OuterInner) {
        const size_t inners_end = g.loops->entities.size();
        if (loops_before < loops_after_classic && loops_after_classic < inners_end) {
            auto it_begin  = g.loops->entities.begin() + loops_before;
            auto it_middle = g.loops->entities.begin() + loops_after_classic;
            auto it_end    = g.loops->entities.begin() + inners_end;
            std::rotate(it_begin, it_middle, it_end);
        }
    }
}

}} // namespace Slic3r::NeoArachne
