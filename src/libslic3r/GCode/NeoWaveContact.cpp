// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoWave support, contact layer. See NeoWaveContact.hpp.
#include "NeoWaveContact.hpp"

#include "../BoundingBox.hpp"
#include "../ExtrusionEntity.hpp"
#include "../GCodeWriter.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace NeoWaveContact {

static bool roof_contains(const ExPolygons &roof, const BoundingBox &roof_bb, const Point &p)
{
    if (!roof_bb.contains(p))
        return false;
    for (const ExPolygon &ex : roof)
        if (ex.contains(p))
            return true;
    return false;
}

const ExPolygons *roof_below(const ExtrusionPath &path, const Layer *layer, const FullPrintConfig &config)
{
    if (path.role() != erBridgeInfill || layer == nullptr || layer->id() == 0 || path.polyline.size() < 2)
        return nullptr;
    const PrintObject *object = layer->object();
    if (object == nullptr)
        return nullptr;
    const PrintObjectNeoConfig &neo = object->neo_config();
    if (!neo.neowave_contact.value || neo.neowave_contact_amplitude.value <= 1e-9)
        return nullptr;
    const auto support_layers = object->support_layers();
    if (support_layers.empty())
        return nullptr;

    // The highest support layer at or below the bottom of this layer, within the top Z distance (plus a layer).
    const double        bottom_z = layer->bottom_z();
    const SupportLayer *below    = nullptr;
    for (size_t i = support_layers.size(); i-- > 0;)
        if (support_layers[i]->print_z <= bottom_z + EPSILON) {
            below = support_layers[i];
            break;
        }
    const double tol = std::max(0., config.support_top_z_distance.value) + layer->height + EPSILON;
    if (below == nullptr || bottom_z - below->print_z > tol || below->support_islands.empty())
        return nullptr;

    // The bridge has to cross the roof at least partly.
    BoundingBox path_bb;
    for (const Point3 &p : path.polyline.points)
        path_bb.merge(p.to_point());
    if (!path_bb.overlap(get_extents(below->support_islands)))
        return nullptr;
    return &below->support_islands;
}

void extrude(std::string &gcode, GCodeWriter &writer, const ExtrusionPath &path, const ExPolygons &roof,
             const PrintObjectNeoConfig &neo, const FullPrintConfig &config, double nominal_z, double F, double e_per_mm,
             const std::function<Vec2d(const Point &)> &point_to_gcode, const std::string &comment)
{
    const double amplitude = neo.neowave_contact_amplitude.value;
    const double width     = std::max(double(path.width), 0.);
    double       period    = neo.neowave_contact_period.value;
    if (period < 1e-9)
        period = width;
    // A Z wave finer than about one line width is unprintable; the floor also bounds the segment count.
    period = std::max(period, std::max(0.2, width));

    // Cap the XY speed so the Z axis stays within its limit: the steepest slope of A*|sin(2*pi*s/P)| is 2*pi*A/P.
    double max_z_speed = 20.;
    if (!config.machine_max_speed_z.values.empty() && config.machine_max_speed_z.values.front() > 0.)
        max_z_speed = std::min(max_z_speed, config.machine_max_speed_z.values.front());
    const double xy_speed_max = max_z_speed * period / (2. * PI * amplitude);
    const double wave_F       = std::min(F, xy_speed_max * 60.);
    if (wave_F < F - EPSILON && wave_F > 0.)
        writer.set_speed(gcode, wave_F, "", "");

    const BoundingBox roof_bb      = get_extents(roof);
    const int         n_per_period = 8;
    const double      seg_target   = period / double(n_per_period);
    const int         max_segs     = 4096; // per line, hang guard
    const bool        no_extrusion = path.is_force_no_extrusion();
    double            dist         = 0.;
    for (const Line3 &line : path.polyline.lines()) {
        const double length = line.length() * SCALING_FACTOR;
        if (length < EPSILON)
            continue;
        const double dE = e_per_mm * length;
        const Point  pa = line.a.to_point();
        const Point  pb = line.b.to_point();
        const Vec2d  ga = point_to_gcode(pa);
        const Vec2d  gb = point_to_gcode(pb);

        BoundingBox line_bb(pa, pa);
        line_bb.merge(pb);
        if (!line_bb.overlap(roof_bb)) {
            // Nowhere near the roof: one move at the layer Z.
            writer.extrude_to_xyz(gcode, Vec3d(gb.x(), gb.y(), nominal_z), dE, comment, no_extrusion);
            dist += length;
            continue;
        }
        const int    n_segs = std::clamp(int(std::ceil(length / seg_target)), 1, max_segs);
        const double dE_seg = dE / double(n_segs);
        for (int i = 0; i < n_segs; ++i) {
            const double t  = double(i + 1) / double(n_segs);
            const Vec2d  pt = ga + t * (gb - ga);
            const Point  sp(coord_t(double(pa.x()) + t * double(pb.x() - pa.x())), coord_t(double(pa.y()) + t * double(pb.y() - pa.y())));
            double z_off = 0.;
            if (roof_contains(roof, roof_bb, sp)) {
                // Upward only: valleys at the layer Z touch the roof, the nozzle never goes below it.
                const double phase = (dist + length * t) / period * 2. * PI;
                z_off = amplitude * std::abs(std::sin(phase));
            }
            writer.extrude_to_xyz(gcode, Vec3d(pt.x(), pt.y(), nominal_z + z_off), dE_seg, comment, no_extrusion);
        }
        dist += length;
    }
    gcode += writer.travel_to_z(nominal_z, "NeoWave contact: restore layer Z");
}

}} // namespace Slic3r::NeoWaveContact
