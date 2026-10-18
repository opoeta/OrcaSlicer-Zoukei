// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoArachne wall generator (NeoArachnePlan dispatch, Hybrid v2 and
// v3 spine recipes, NeoArachneInterior of NeotkoCM). See NeoArachneHybrid.hpp.
#include "NeoArachneHybrid.hpp"

#include "../Arachne/WallToolPaths.hpp"
#include "../Arachne/utils/ExtrusionLine.hpp"
#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"

#include <functional>
#include "../PerimeterGenerator.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../ShortestPath.hpp"
#include "../SurfaceCollection.hpp"
#include "../libslic3r.h"
#include "NeoArachneSpine.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace Slic3r { namespace NeoArachne {

HybridConfig hybrid_config(const PrintObjectConfig &oc, const PrintObjectNeoConfig &rc)
{
    HybridConfig c;
    c.outer_wall                = rc.neoarachne_outer_wall.value;
    c.inner_walls               = rc.neoarachne_inner_walls.value;
    c.allowed_overlap_pct       = oc.neoarachne_overlap_pct.value;
    c.min_bead_width_pct        = oc.neoarachne_min_bead_pct.value;
    c.max_bead_width_pct        = oc.neoarachne_max_bead_pct.value;
    c.min_feature_pct           = rc.neoarachne_min_feature_pct.value;
    c.keep_short_tails          = rc.neoarachne_keep_short_tails.value;
    c.pin_outer_width           = rc.neoarachne_pin_outer_width.value;
    c.bead_count_hysteresis_pct = rc.neoarachne_hysteresis_pct.value;
    c.spine                     = rc.neoarachne_spine.value;
    c.spine_min_width_pct       = rc.neoarachne_spine_min_width_pct.value;
    c.spine_max_width_pct       = rc.neoarachne_spine_max_width_pct.value;
    c.spine_min_length_mm       = rc.neoarachne_spine_min_length.value;
    c.spine_sliver_pct          = rc.neoarachne_spine_sliver_pct.value;
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
    // NeotkoEdge also pins the bead width of the 1-2 bead regions and delays the bead count transitions.
    const bool neotko_edge           = cfg.inner_walls == NeoArachneWallSource::ArachneNeotkoEdge;
    params.neoarachne_enabled        = true;
    params.neoarachne_cap_widening   = true;
    params.neoarachne_pin_outer      = neotko_edge && cfg.pin_outer_width;
    params.neoarachne_hysteresis_pct = neotko_edge ? cfg.bead_count_hysteresis_pct : 0.0;

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
        // no_sort would make GCode extrude the bucket as ONE entity, which extrude_entity() rejects (collections are
        // only unpacked when sortable); the island order is kept by the order the buckets are appended.
        g.loops->append(bucket);
    }
    if (!shared_bucket.empty()) {
        // no_sort would make GCode extrude the bucket as ONE entity, which extrude_entity() rejects (collections are
        // only unpacked when sortable); the island order is kept by the order the buckets are appended.
        g.loops->append(shared_bucket);
    }

    // The area left for the infill. Arachne's inner contour can legitimately be empty (thin regions, few insets);
    // then fall back to the residue shrunk by the full wall thickness, which never overlaps the walls.
    const Polygons &arachne_reported = wtp.getInnerContour();
    if (!arachne_reported.empty())
        return union_ex(arachne_reported);
    return offset_ex(union_ex(outline_polys), -float(inset_count) * float(perimeter_spacing));
}

// The same "force a single wall" rules as the classic generator.
static int effective_wall_count(const PerimeterGenerator &g, const PrintRegionConfig &cfg)
{
    int walls = cfg.wall_loops.value;
    const bool one_wall_first = cfg.only_one_wall_first_layer && cfg.bottom_shell_layers.value > 0;
    const bool one_wall_top   = cfg.only_one_wall_top && cfg.top_shell_layers.value > 0;
    if (g.layer_id == g.object_config->raft_layers && one_wall_first)
        walls = 1;
    if (walls > 1 && one_wall_top && g.upper_slices == nullptr)
        walls = 1;
    return walls;
}

// NeoArachne v3 (NeoArachnePlan run_classic_spine of NeotkoCM), Simplify3D style:
//   1. Classic ONLY for the outer wall (and the thin walls, if enabled), without gap fill.
//   2. Zone = what is left inside the axis of the outer wall and not covered by it.
//   3. Own inner walls, level by level, fixed width. BEFORE each one, every zone:
//        - fits WHOLE in one line <= ceiling -> spine, no more walls there (S3D rule);
//        - no walls left                     -> infill;
//        - otherwise                         -> one more loop (offset -spacing/2), continue inside.
//      This removes the PARTIAL walls of Classic and the crumbs between walls.
//   4. Spine (NeoArachneSpine) and the infill clipped to what is left, with the Classic infill/wall overlap.
static void run_classic_spine(PerimeterGenerator &g, const HybridConfig &cfg, const PrintRegionConfig *original_cfg)
{
    if (original_cfg->wall_loops.value < 1) {
        g.process_classic();
        return;
    }
    const int walls = effective_wall_count(g, *original_cfg);

    // A local copy, as in Hybrid v2: other LayerRegions of the same PrintRegion may be slicing in parallel.
    PrintRegionConfig modified_cfg = *original_cfg;
    modified_cfg.wall_loops.value = 1; // Classic: the outer wall only
    std::fill(modified_cfg.gap_infill_speed.values.begin(), modified_cfg.gap_infill_speed.values.end(), 0.); // the spine replaces the gap fill
    modified_cfg.alternate_extra_wall.value = false; // the inner walls follow the S3D rule

    ExPolygons original_slice;
    original_slice.reserve(g.slices->surfaces.size());
    for (const Surface &s : g.slices->surfaces)
        original_slice.push_back(s.expolygon);

    const size_t loops_before = g.loops->entities.size();
    g.config = &modified_cfg;
    g.process_classic();
    g.config = original_cfg;
    const size_t loops_after_outer = g.loops->entities.size();

    // What the outer wall covers, measured by its spacing (so the first inner wall leaves no gap).
    Polygons outer_cov;
    for (size_t i = loops_before; i < loops_after_outer; ++i)
        g.loops->entities[i]->polygons_covered_by_spacing(outer_cov, float(SCALED_EPSILON));

    // Only the interior: outside the axis of the outer wall the wall and detect_thin_wall rule.
    const float eps           = float(scaled<double>(0.005)); // numeric crumbs, not tips
    const float inside_offset = float(g.ext_perimeter_flow.scaled_spacing()) / 2.f;
    ExPolygons  todo          = diff_ex(offset_ex(original_slice, -inside_offset), outer_cov, ApplySafetyOffset::Yes);
    todo = opening_ex(todo, eps);

    const double line_w = g.perimeter_flow.width();
    SpineParams sp;
    sp.floor_mm      = line_w * cfg.spine_min_width_pct / 100.;
    sp.ceiling_mm    = line_w * cfg.spine_max_width_pct / 100.;
    sp.min_length_mm = cfg.spine_min_length_mm;
    sp.sliver_mm     = line_w * cfg.spine_sliver_pct / 100.;
    const double ceiling_w = scaled<double>(std::max(sp.ceiling_mm, sp.floor_mm));
    const float  spacing   = float(g.perimeter_flow.scaled_spacing());
    // Necks a loop does not reach stay as a zone if they have a body; the corner slivers of the loop (thinner than
    // half the floor) are dropped.
    const float neck_open = float(std::max(double(eps), scaled<double>(sp.floor_mm) / 4.));

    ExPolygons              spine_regions, forced_spine, fill_left;
    std::vector<ExPolygons> levels; // axes of the inner loops; levels[0] = first inner wall
    // Mini loops: a loop enclosing less than ~3 line widths of diameter is not a wall, it is a blob with its own
    // start and stop. It is not made; its zone goes to the spine even above the ceiling (width clipped).
    const double min_loop_len = PI * 3. * scaled<double>(line_w);
    int          loops_left   = walls - 1;
    while (!todo.empty()) {
        ExPolygons next, centres;
        for (ExPolygon &comp : todo) {
            if (fits_in_one_line(comp, ceiling_w)) {
                spine_regions.emplace_back(std::move(comp));
                continue;
            }
            ExPolygons   centre = offset_ex(ExPolygons{ comp }, -spacing / 2.f);
            const size_t n_raw  = centre.size();
            centre.erase(std::remove_if(centre.begin(), centre.end(),
                                        [&](const ExPolygon &c) { return c.contour.length() < min_loop_len; }),
                         centre.end());
            const bool only_tiny = n_raw > 0 && centre.empty(); // only mini loops would fit
            if (loops_left <= 0 || centre.empty()) {
                if (only_tiny)
                    forced_spine.emplace_back(std::move(comp));
                else // walls used up, or neither a loop nor the line fits (user ceiling)
                    fill_left.emplace_back(std::move(comp));
                continue;
            }
            ExPolygons inner = offset_ex(ExPolygons{ comp }, -spacing);
            append(inner, opening_ex(diff_ex(ExPolygons{ comp }, offset_ex(centre, spacing / 2.f)), neck_open));
            append(next, union_ex(inner));
            append(centres, std::move(centre));
        }
        if (!centres.empty())
            levels.emplace_back(std::move(centres));
        --loops_left;
        todo = std::move(next);
    }

    // Spine first (placed below together with the loops of its island). What gives no line goes back to infill.
    ExPolygons           rejected;
    ExtrusionEntitiesPtr spine_ents;
    run_spine(g, spine_regions, sp, nullptr, &rejected, &forced_spine, &spine_ents);
    append(fill_left, std::move(rejected));

    // Inner loops (fixed width, like the classic ones) and the spine, one bucket per island so the printer finishes
    // an island before jumping to the next. The spine is the innermost: first with inner-outer, last with outer-inner.
    const WallSequence ws = original_cfg->wall_sequence;
    {
        const ExPolygons islands = union_ex(original_slice);
        std::vector<ExtrusionEntityCollection> per_island(islands.size() + 1);
        auto island_of = [&](const Point &p) -> size_t {
            for (size_t i = 0; i < islands.size(); ++i)
                if (islands[i].contains(p))
                    return i;
            return islands.size();
        };
        // Spine split by island and chained by proximity (each line reversed as convenient).
        std::vector<ExtrusionEntitiesPtr> spine_by_island(islands.size() + 1);
        // variable_width() may wrap a line in an ExtrusionEntityCollection; the perimeter buckets below are
        // extruded one entity at a time by GCode::extrude_entity(), which only takes paths, multipaths and
        // loops, so unwrap the collections here (otherwise: "Invalid argument supplied to extrude()").
        std::function<void(ExtrusionEntity *)> add_spine = [&](ExtrusionEntity *e) {
            if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(e)) {
                for (ExtrusionEntity *child : coll->entities)
                    add_spine(child);
                coll->entities.clear(); // ownership moved to the island lists
                delete coll;
                return;
            }
            spine_by_island[island_of(e->first_point())].push_back(e);
        };
        for (ExtrusionEntity *e : spine_ents)
            add_spine(e);
        spine_ents.clear();
        for (ExtrusionEntitiesPtr &v : spine_by_island)
            if (v.size() > 1)
                chain_and_reorder_extrusion_entities(v);
        auto emit_spine = [&]() {
            for (size_t i = 0; i < spine_by_island.size(); ++i)
                if (!spine_by_island[i].empty())
                    per_island[i].append(std::move(spine_by_island[i])); // the bucket takes the ownership
        };
        if (ws != WallSequence::OuterInner)
            emit_spine();
        const Flow &pf = g.perimeter_flow;
        auto emit_level = [&](const ExPolygons &centres, int inset) {
            for (const ExPolygon &c : centres) {
                ExtrusionEntityCollection &dst = per_island[island_of(c.contour.first_point())];
                auto add = [&](const Polygon &poly, bool hole) {
                    ExtrusionPath path(erPerimeter, pf.mm3_per_mm(), pf.width(), pf.height());
                    path.polyline  = Polyline3(poly.split_at_first_point());
                    path.inset_idx = inset;
                    ExtrusionLoop loop(std::move(path));
                    loop.make_counter_clockwise();
                    loop.set_loop_role(hole ? elrHole : elrDefault);
                    loop.inset_idx = inset;
                    dst.append(std::move(loop));
                };
                add(c.contour, false);
                for (const Polygon &h : c.holes)
                    add(h, true);
            }
        };
        // Inside each island: outside-in with OuterInner, inside-out otherwise.
        if (ws == WallSequence::OuterInner)
            for (size_t i = 0; i < levels.size(); ++i)
                emit_level(levels[i], int(i) + 1);
        else
            for (size_t i = levels.size(); i-- > 0;)
                emit_level(levels[i], int(i) + 1);
        if (ws == WallSequence::OuterInner)
            emit_spine();
        for (ExtrusionEntityCollection &bucket : per_island)
            if (!bucket.empty()) {
                // no_sort would make GCode extrude the bucket as ONE entity, which extrude_entity() rejects (collections are
                // only unpacked when sortable); the island order is kept by the order the buckets are appended.
                g.loops->append(bucket);
            }
    }
    // Outer / inner order as in Hybrid v2: with inner-outer the inner block goes first. Inner-outer-inner is
    // approximated as inner-outer.
    {
        const size_t inners_end = g.loops->entities.size();
        if (ws != WallSequence::OuterInner && loops_before < loops_after_outer && loops_after_outer < inners_end)
            std::rotate(g.loops->entities.begin() + loops_before, g.loops->entities.begin() + loops_after_outer,
                        g.loops->entities.begin() + inners_end);
    }

    // Infill: only what stayed wide, with the same overlap against the walls as Classic uses.
    {
        const bool                 topbottom = (g.layer_id == 0 || g.upper_slices == nullptr);
        const ConfigOptionPercent &ov_opt    = topbottom ? original_cfg->top_bottom_infill_wall_overlap : original_cfg->infill_wall_overlap;
        const coord_t              base      = coord_t(spacing / 2.f) + g.solid_infill_flow.scaled_spacing() / 2;
        const double               ov        = scale_(ov_opt.get_abs_value(unscale<double>(base)));
        const ExPolygons           fill_area = offset_ex(fill_left, float(ov));
        Surfaces                   clipped;
        clipped.reserve(g.fill_surfaces->surfaces.size());
        for (const Surface &s : g.fill_surfaces->surfaces)
            for (ExPolygon &ex : intersection_ex(ExPolygons{ s.expolygon }, fill_area))
                clipped.emplace_back(Surface(s, std::move(ex)));
        g.fill_surfaces->surfaces = std::move(clipped);
        if (g.fill_no_overlap != nullptr && !g.fill_no_overlap->empty())
            *g.fill_no_overlap = intersection_ex(*g.fill_no_overlap, fill_left);
    }
}

void run_hybrid(PerimeterGenerator &g, const PrintObjectNeoConfig &neo_config)
{
    const PrintRegionConfig *original_cfg = g.config;
    const HybridConfig       cfg          = hybrid_config(*g.object_config, neo_config);

    // Engine choice per region (NeoArachnePlan dispatch of NeotkoCM).
    const bool outer_arachne = cfg.outer_wall != NeoArachneWallSource::Classic;
    const bool inner_arachne = cfg.inner_walls != NeoArachneWallSource::Classic;
    if (outer_arachne) {
        // A variable width outer wall needs the Arachne interior as well: stock Arachne.
        g.process_arachne();
        return;
    }
    if (!inner_arachne) {
        // Classic walls: the v3 spine recipe, or plain Classic with the spine off.
        if (cfg.spine)
            run_classic_spine(g, cfg, original_cfg);
        else
            g.process_classic();
        return;
    }

    // Classic outer + Arachne interior: Hybrid v2.
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

    const int effective_walls = effective_wall_count(g, *original_cfg);
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
