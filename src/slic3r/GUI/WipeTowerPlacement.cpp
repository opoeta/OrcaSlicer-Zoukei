#include "WipeTowerPlacement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"

namespace Slic3r { namespace GUI {

bool wipe_tower_optimal_pos_enabled()
{
    const AppConfig *app_config = wxGetApp().app_config;
    if (!app_config)
        return false;
    return app_config->get("auto_optimize_wipe_tower_placement") != "false";
}

bool compute_optimal_wipe_tower_min(const Polygons &inflated_parts,
                                    const Vec2d    &plate_center,
                                    const Vec2d    &default_tower_center,
                                    const Vec2d    &tower_size,
                                    double          tower_clearance,
                                    Vec2d          &out_wt_min)
{
    if (inflated_parts.empty())
        return false;
    if (tower_size.x() <= EPSILON || tower_size.y() <= EPSILON)
        return false;

    // 1. Convex hull of all inflated part contours. Hulling the merged point set is equivalent to hulling
    //    their union, so no Clipper boolean is needed.
    Points pts;
    for (const Polygon &p : inflated_parts)
        pts.insert(pts.end(), p.points.begin(), p.points.end());
    const Polygon hull = Geometry::convex_hull(std::move(pts));
    if (hull.points.size() < 3)
        return false; // degenerate (collinear / single sliver)

    // 2. Ray origin: the hull's area centroid. It is guaranteed to lie inside the Minkowski sum below.
    const Vec2d C = unscale(hull.centroid());

    // 3. Ray direction: plate centre -> the printer's default tower position.
    Vec2d d = default_tower_center - plate_center;
    if (d.norm() <= EPSILON)
        return false; // the default tower sits on the plate centre: no usable direction
    d.normalize();

    // 4. M = hull (+) (-R): hull vertices offset by the four corners of the tower rectangle. The rectangle is grown
    //    by the clearance so the tower keeps its share of the gap; the real half-extents recover the true corner.
    const double gap = std::max(0.0, tower_clearance);
    const double hw_real = tower_size.x() / 2.0, hh_real = tower_size.y() / 2.0;
    const double hw = hw_real + gap, hh = hh_real + gap;
    Points msum_pts;
    msum_pts.reserve(hull.points.size() * 4);
    for (const Point &v : hull.points) {
        const Vec2d vd = unscale(v);
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                msum_pts.emplace_back(Point::new_scale(Vec2d(vd.x() + sx * hw, vd.y() + sy * hh)));
    }
    Polygon msum = Geometry::convex_hull(std::move(msum_pts));
    if (msum.points.size() < 3)
        return false;
    // Do not depend on the winding convex_hull() returns: edge normals below must face outwards.
    if (!msum.is_counter_clockwise())
        msum.make_counter_clockwise();

    // 5. Ray exit: C is inside the convex, CCW msum, so the exit parameter is the smallest positive hit
    //    among the edges the ray actually faces.
    double t_exit = std::numeric_limits<double>::infinity();
    const size_t n = msum.points.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2d a = unscale(msum.points[i]);
        const Vec2d b = unscale(msum.points[(i + 1) % n]);
        const Vec2d e = b - a;
        Vec2d nrm(e.y(), -e.x()); // outward normal of a CCW contour
        if (nrm.norm() <= EPSILON) continue;
        nrm.normalize();
        const double denom = nrm.dot(d);
        if (denom <= EPSILON) continue; // edge faces away from / is parallel to the ray
        const double t = nrm.dot(a - C) / denom;
        if (t < t_exit) t_exit = t;
    }
    if (!std::isfinite(t_exit) || t_exit < 0.0)
        return false;

    // 6. The ray solved the centre of the clearance-grown rectangle, which is also the centre of the real one.
    const Vec2d tower_center = C + t_exit * d;
    out_wt_min = Vec2d(tower_center.x() - hw_real, tower_center.y() - hh_real);
    return true;
}

// The tower position PartPlateList::set_default_wipe_tower_pos_for_plate() starts from, plate-local.
static Vec2d machine_default_wipe_tower_pos(const DynamicPrintConfig &config)
{
    float x = WIPE_TOWER_DEFAULT_X_POS;
    float y = WIPE_TOWER_DEFAULT_Y_POS;
    const auto *structure = config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
    if (structure && structure->value == PrinterStructure::psI3) {
        x = I3_WIPE_TOWER_DEFAULT_X_POS;
        y = I3_WIPE_TOWER_DEFAULT_Y_POS;
    }
    return Vec2d(x, y);
}

bool compute_plate_optimal_wipe_tower_pos(PartPlate &plate, int plate_idx, const DynamicPrintConfig &config, Vec2d &out_local_pos)
{
    if (plate.is_locked() || plate.empty())
        return false;
    const auto *wt_x = config.option<ConfigOptionFloats>("wipe_tower_x");
    const auto *wt_y = config.option<ConfigOptionFloats>("wipe_tower_y");
    if (wt_x == nullptr || wt_y == nullptr || plate_idx >= int(wt_x->values.size()) || plate_idx >= int(wt_y->values.size()))
        return false;

    // Printing by object only keeps a tower when the plate holds exactly one printable instance.
    PrintSequence seq = plate.get_print_seq();
    if (seq == PrintSequence::ByDefault) {
        const auto *global_seq = config.option<ConfigOptionEnum<PrintSequence>>("print_sequence");
        if (global_seq != nullptr)
            seq = global_seq->value;
    }
    if (seq == PrintSequence::ByObject && plate.printable_instance_size() != 1)
        return false;

    // Tower size and the extent of its brim / stabilisation cone: the outline of the arrange polygon.
    Vec3d wt_pos, wt_size;
    const arrangement::ArrangePolygon wt_ap = plate.estimate_wipe_tower_polygon(config, plate_idx, wt_pos, wt_size);
    if (wt_size.x() <= EPSILON || wt_size.y() <= EPSILON)
        return false;
    const Vec2d tower_size(wt_size.x(), wt_size.y());
    const BoundingBox ap_bb = get_extents(wt_ap.poly.contour);
    const double brim_total = std::max({0., wt_pos.x() - unscaled(ap_bb.min.x()), wt_pos.y() - unscaled(ap_bb.min.y()),
                                        unscaled(ap_bb.max.x()) - (wt_pos.x() + tower_size.x()),
                                        unscaled(ap_bb.max.y()) - (wt_pos.y() + tower_size.y())});

    const Vec3d plate_origin = plate.get_origin();

    // The objects the tower must stay clear of, inflated the way arrange inflates them.
    arrangement::ArrangeParams params;
    params.min_obj_distance = 0; // auto: let every item's brim/support drive the inflation
    Polygons inflated_parts;
    {
        arrangement::ArrangePolygons aps;
        const ModelObjectPtrs objects = plate.get_objects();
        for (int obj_idx = 0; obj_idx < int(objects.size()); ++obj_idx) {
            ModelObject *mo = objects[obj_idx];
            if (mo == nullptr)
                continue;
            for (int inst_idx = 0; inst_idx < int(mo->instances.size()); ++inst_idx) {
                if (!mo->instances[inst_idx]->is_printable() || !plate.contain_instance_totally(obj_idx, inst_idx))
                    continue;
                aps.emplace_back(get_instance_arrange_poly(mo->instances[inst_idx], config));
            }
        }
        if (aps.empty())
            return false;

        arrangement::update_selected_items_inflation(aps, &config, params);
        for (const arrangement::ArrangePolygon &ap : aps) {
            Polygon c = ap.poly.contour;
            c.translate(ap.translation.x(), ap.translation.y());
            // Arrange floors every item's inflation at its minimum separation.
            const coord_t infl = std::max(ap.inflation, scaled<coord_t>(WIPE_TOWER_ARRANGE_GAP));
            if (infl > 0) {
                Polygons grown = offset(c, float(infl));
                for (Polygon &g : grown)
                    inflated_parts.emplace_back(std::move(g));
            } else {
                inflated_parts.emplace_back(std::move(c));
            }
        }
    }

    const Vec2d def_local = machine_default_wipe_tower_pos(config);
    const Vec2d default_tower_center(plate_origin.x() + def_local.x() + tower_size.x() / 2.0,
                                     plate_origin.y() + def_local.y() + tower_size.y() / 2.0);
    const Vec2d plate_size   = plate.get_size();
    const Vec2d plate_center(plate_origin.x() + plate_size.x() / 2.0, plate_origin.y() + plate_size.y() / 2.0);

    // Hug the objects: 2 x brim + arrange gap + extra clearance.
    const double clearance = 2.0 * brim_total + WIPE_TOWER_ARRANGE_GAP + WIPE_TOWER_OPTIMAL_POS_EXTRA_CLEARANCE;
    Vec2d optimal_world;
    if (!compute_optimal_wipe_tower_min(inflated_parts, plate_center, default_tower_center, tower_size, clearance, optimal_world))
        return false;

    const Vec2d local(optimal_world.x() - plate_origin.x(), optimal_world.y() - plate_origin.y());

    // Reachable range of the tower's min corner (same margins as estimate_wipe_tower_polygon()).
    const double margin = WIPE_TOWER_MARGIN + brim_total;
    if (local.x() < margin - EPSILON || local.x() > plate_size.x() - tower_size.x() - margin + EPSILON ||
        local.y() < margin - EPSILON || local.y() > plate_size.y() - tower_size.y() - margin + EPSILON)
        return false;

    // Non-rectangular beds: the whole tower has to stay on the printable outline.
    {
        Polygons bed{plate.get_shared_printable_polygon()};
        if (!bed.empty() && !bed.front().empty()) {
            bed.front().translate(Point(-scaled<coord_t>(plate_origin.x()), -scaled<coord_t>(plate_origin.y())));
            const BoundingBox tower(Point::new_scale(local.x(), local.y()),
                                    Point::new_scale(local.x() + tower_size.x(), local.y() + tower_size.y()));
            const Vec2f move = WipeTower::move_box_inside_polygon(tower, bed, scaled<coord_t>(margin));
            if (std::abs(move.x()) > 0.01f || std::abs(move.y()) > 0.01f)
                return false;
        }
    }

    // Exclusion zones (world coordinates), tested against the tower with its brim.
    {
        const double x0 = optimal_world.x() - brim_total - WIPE_TOWER_ARRANGE_GAP;
        const double y0 = optimal_world.y() - brim_total - WIPE_TOWER_ARRANGE_GAP;
        const double x1 = optimal_world.x() + tower_size.x() + brim_total + WIPE_TOWER_ARRANGE_GAP;
        const double y1 = optimal_world.y() + tower_size.y() + brim_total + WIPE_TOWER_ARRANGE_GAP;
        for (const BoundingBoxf3 &r : plate.get_exclude_areas())
            if (x0 < r.max.x() && x1 > r.min.x() && y0 < r.max.y() && y1 > r.min.y())
                return false;
    }

    out_local_pos = local;
    return true;
}

int optimize_wipe_towers(PartPlateList &plate_list, int only_plate_idx)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return 0;
    DynamicPrintConfig &proj_cfg = bundle->project_config;
    auto *wt_x = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x");
    auto *wt_y = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y");
    if (wt_x == nullptr || wt_y == nullptr)
        return 0;
    const auto *prime = bundle->prints.get_edited_preset().config.option<ConfigOptionBool>("enable_prime_tower");
    if (prime == nullptr || !prime->value)
        return 0;

    const DynamicPrintConfig full_config = bundle->full_config();
    int moved = 0;
    for (int idx = 0; idx < plate_list.get_plate_count(); ++idx) {
        if (only_plate_idx >= 0 && idx != only_plate_idx)
            continue;
        PartPlate *plate = plate_list.get_plate(idx);
        if (plate == nullptr || idx >= int(wt_x->values.size()) || idx >= int(wt_y->values.size()))
            continue;
        Vec2d pos;
        if (!compute_plate_optimal_wipe_tower_pos(*plate, idx, full_config, pos))
            continue;
        if (std::abs(wt_x->values[idx] - pos.x()) <= 1e-3 && std::abs(wt_y->values[idx] - pos.y()) <= 1e-3)
            continue;
        BOOST_LOG_TRIVIAL(info) << boost::format("wipe tower of plate %1% moved next to the objects: {%2%, %3%} -> {%4%, %5%}")
            % (idx + 1) % wt_x->values[idx] % wt_y->values[idx] % pos.x() % pos.y();
        ConfigOptionFloat x_opt(pos.x());
        ConfigOptionFloat y_opt(pos.y());
        wt_x->set_at(&x_opt, idx, 0);
        wt_y->set_at(&y_opt, idx, 0);
        ++moved;
    }
    return moved;
}

}} // namespace Slic3r::GUI
