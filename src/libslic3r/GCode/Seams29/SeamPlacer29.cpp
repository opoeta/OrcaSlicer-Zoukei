///|/ Ported from PrusaSlicer 2.9 (src/libslic3r/GCode/SeamPlacer.cpp)
///|/ Copyright (c) Prusa Research 2020 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena,
///|/ Pavel Mikuš @Godrak
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "libslic3r/GCode/Seams29/SeamPlacer29.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <boost/log/trivial.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/Seams29/SeamShells.hpp"
#include "libslic3r/GCode/Seams29/SeamAligned.hpp"
#include "libslic3r/GCode/Seams29/SeamRear.hpp"
#include "libslic3r/GCode/Seams29/SeamRandom.hpp"
#include "libslic3r/GCode/Seams29/SeamGeometry.hpp"

namespace Slic3r::Seams {

static Perimeters::LayerPerimeters get_perimeters(
    const PrintObject &print_object,
    const Params &params,
    const ModelInfo::Painting &painting,
    const std::function<void(void)> &throw_if_canceled
) {
    throw_if_canceled();
    const LayerPtrs layers(print_object.layers().begin(), print_object.layers().end());
    const std::vector<Geometry::Extrusions> extrusions{Geometry::get_extrusions(layers)};
    const Perimeters::LayerInfos layer_infos{Perimeters::get_layer_infos(
        layers, params.perimeter.elephant_foot_compensation
    )};
    const std::vector<Geometry::BoundedPolygons> projected{
        Geometry::project_to_geometry(extrusions, params.max_distance)
    };
    throw_if_canceled();
    return Perimeters::create_perimeters(projected, layer_infos, painting, params.perimeter);
}

static unsigned get_seam_choice_value(const SeamChoice &seam_choice, const Perimeters::Perimeter& perimeter) {
    const unsigned previous_value{Perimeters::get_point_value(
        perimeter.point_types[seam_choice.previous_index],
        perimeter.point_classifications[seam_choice.previous_index]
    )};

    const unsigned next_value{Perimeters::get_point_value(
        perimeter.point_types[seam_choice.next_index],
        perimeter.point_classifications[seam_choice.next_index]
    )};

    return std::max(previous_value, next_value);
}

// ORCA: Assemble zgap. Within a layer the seams of the larger contours stay where alignment put them;
// a seam closer than min_distance to one of them slides along its own perimeter, both ways, to the
// nearest point that is clear and no worse a seam (no blocker or overhang it did not already have).
// The search depends only on the geometry, so a shell keeps the same detour from layer to layer.
static void separate_layer_seams(std::vector<SeamPerimeterChoice> &seams, const double min_distance)
{
    if (seams.size() < 2)
        return;
    std::vector<std::size_t> order(seams.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&seams](std::size_t a, std::size_t b) {
        const Vec2d size_a = unscaled(seams[a].bounding_box.size());
        const Vec2d size_b = unscaled(seams[b].bounding_box.size());
        return size_a.x() * size_a.y() > size_b.x() * size_b.y();
    });

    const double min_distance_sq = min_distance * min_distance;
    std::vector<Vec2d> taken;
    const auto is_free = [&taken, min_distance_sq](const Vec2d &position) {
        for (const Vec2d &t : taken)
            if ((t - position).squaredNorm() < min_distance_sq)
                return false;
        return true;
    };
    const auto never_stop = [](const Perimeters::Perimeter &, const std::size_t) { return false; };

    for (const std::size_t index : order) {
        SeamPerimeterChoice &seam = seams[index];
        const Perimeters::Perimeter &perimeter = seam.perimeter;
        if (!perimeter.is_degenerate && perimeter.positions.size() > 2 && !is_free(seam.choice.position)) {
            double length = 0.;
            for (std::size_t i = 0; i < perimeter.positions.size(); ++i)
                length += (perimeter.positions[i] - perimeter.positions[i + 1 == perimeter.positions.size() ? 0 : i + 1]).norm();
            const unsigned acceptable_value = std::min(get_seam_choice_value(seam.choice, perimeter), 5u);
            const double step = 0.25;
            bool moved = false;
            for (double offset = step; !moved && offset < 0.5 * length; offset += step) {
                for (const Geometry::Direction1D direction : {Geometry::Direction1D::forward, Geometry::Direction1D::backward}) {
                    const std::optional<SeamChoice> candidate = Perimeters::offset_along_perimeter(seam.choice, perimeter, offset, direction, never_stop);
                    if (candidate && get_seam_choice_value(*candidate, perimeter) >= acceptable_value && is_free(candidate->position)) {
                        seam.choice = *candidate;
                        moved = true;
                        break;
                    }
                }
            }
        }
        taken.push_back(seam.choice.position);
    }
}

static std::vector<std::vector<SeamPerimeterChoice>> precalculate_seams(
    const PrintObject &print_object,
    const Params &params,
    Perimeters::LayerPerimeters &&layer_perimeters,
    const std::function<void(void)> &throw_if_canceled
) {
    const SeamPosition seam_position = print_object.config().seam_position.value;
    switch (seam_position) {
    case spAligned:
    case spAlignedBack:
    case spAssembleZgap: {
        // ORCA: trafo_sliced(), the frame the layers were sliced in, as the classic SeamPlacer uses.
        const Transform3d transformation{print_object.trafo_sliced()};
        const ModelVolumePtrs &volumes{print_object.model_object()->volumes};

        ModelInfo::Visibility::Params visibility_params = params.visibility;
        visibility_params.prefer_back = seam_position == spAlignedBack;
        const ModelInfo::Visibility points_visibility{transformation, volumes, visibility_params, throw_if_canceled};
        throw_if_canceled();
        const Aligned::VisibilityCalculator visibility_calculator{
            points_visibility, params.convex_visibility_modifier,
            params.concave_visibility_modifier};

        Shells::Shells<> shells{Shells::create_shells(std::move(layer_perimeters), params.max_distance)};
        std::vector<std::vector<SeamPerimeterChoice>> seams = Aligned::get_object_seams(
            std::move(shells), visibility_calculator, params.aligned
        );
        if (seam_position == spAssembleZgap) {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, seams.size()), [&seams, &params](const tbb::blocked_range<size_t> &range) {
                for (size_t layer_index = range.begin(); layer_index < range.end(); ++layer_index)
                    separate_layer_seams(seams[layer_index], params.assemble_min_distance);
            });
        }
        return seams;
    }
    case spRear: {
        return Rear::get_object_seams(std::move(layer_perimeters), params.rear_tolerance, params.rear_y_offset);
    }
    case spRandom: {
        return Random::get_object_seams(std::move(layer_perimeters), params.random_seed);
    }
    case spNearest:
    default:
        // Nothing to precalculate, the seam follows the nozzle.
        return {};
    }
}

Params Placer::get_params(const PrintObject &print_object) {
    const PrintObjectConfig &config = print_object.config();
    Params params{};

    params.perimeter.elephant_foot_compensation = config.elefant_foot_compensation.value;
    if (config.raft_layers.value > 0) {
        params.perimeter.elephant_foot_compensation = 0.0;
    }
    params.random_seed = 1653710332u;

    params.aligned.max_detour = 1.0;
    params.aligned.continuity_modifier = 2.0;
    params.convex_visibility_modifier = 1.1;
    params.concave_visibility_modifier = 0.9;
    params.perimeter.overhang_threshold = 55.0 * M_PI / 180.0;
    params.perimeter.convex_threshold = 10.0 * M_PI / 180.0;
    params.perimeter.concave_threshold = 15.0 * M_PI / 180.0;

    params.staggered_inner_seams = config.staggered_inner_seams.value;

    params.max_nearest_detour = 1.0;
    params.rear_tolerance = 1.0;
    params.rear_y_offset = 20;
    params.aligned.jump_visibility_threshold = 0.6;
    params.max_distance = 5.0;
    params.perimeter.oversampling_max_distance = 0.2;
    params.perimeter.embedding_threshold = 0.5;
    params.perimeter.painting_radius = 0.05;
    params.perimeter.simplification_epsilon = 0.001;
    params.perimeter.smooth_angle_arm_length = 0.5;
    params.perimeter.sharp_angle_arm_length = 0.25;

    params.visibility.raycasting_visibility_samples_count = 30000;
    params.visibility.fast_decimation_triangle_count_target = 16000;
    params.visibility.sqr_rays_per_sample_point = 5;

    return params;
}

void Placer::clear()
{
    this->params.clear();
    this->seams_per_object.clear();
    this->perimeters_per_layer.clear();
}

void Placer::init(
    const std::vector<const PrintObject *> &objects,
    const std::function<void(void)> &throw_if_canceled
) {
    BOOST_LOG_TRIVIAL(debug) << "SeamPlacer29: init: start";
    this->clear();

    for (const PrintObject *print_object : objects) {
        throw_if_canceled();
        const Params object_params = get_params(*print_object);
        const ModelInfo::Painting painting{print_object->trafo_sliced(), print_object->model_object()->volumes};
        Perimeters::LayerPerimeters perimeters{get_perimeters(*print_object, object_params, painting, throw_if_canceled)};

        if (print_object->config().seam_position.value == spNearest)
            this->perimeters_per_layer[print_object] = std::move(perimeters);
        else
            this->seams_per_object[print_object] = precalculate_seams(*print_object, object_params, std::move(perimeters), throw_if_canceled);
        this->params[print_object] = object_params;
        throw_if_canceled();
    }

    BOOST_LOG_TRIVIAL(debug) << "SeamPlacer29: init: end";
}

static const SeamPerimeterChoice &choose_closest_seam(
    const std::vector<SeamPerimeterChoice> &seams, const Polygon &loop_polygon
) {
    BoundingBoxes choose_from;
    choose_from.reserve(seams.size());
    for (const SeamPerimeterChoice &choice : seams) {
        choose_from.push_back(choice.bounding_box);
    }

    const std::size_t choice_index{
        Geometry::pick_closest_bounding_box(loop_polygon.bounding_box(), choose_from).first};

    return seams[choice_index];
}

static std::pair<std::size_t, Vec2d> project_to_extrusion_loop(
    const SeamChoice &seam_choice,
    const Perimeters::Perimeter &perimeter,
    const AABBTreeLines::LinesDistancer<Linef> &distancer
) {
    const bool is_at_vertex{seam_choice.previous_index == seam_choice.next_index};
    const Vec2d edge{
        perimeter.positions[seam_choice.next_index] -
        perimeter.positions[seam_choice.previous_index]};
    const Vec2d normal{
        is_at_vertex ?
            Geometry::get_polygon_normal(perimeter.positions, seam_choice.previous_index, 0.1) :
            Geometry::get_normal(edge)};

    double depth{distancer.distance_from_lines<false>(seam_choice.position)};
    const Vec2d final_position{seam_choice.position - normal * depth};

    auto [_, loop_line_index, loop_point] = distancer.distance_from_lines_extra<false>(final_position);
    return {loop_line_index, loop_point};
}

static Geometry::Direction1D get_direction(
    const bool flipped,
    const Polygon &perimeter_polygon,
    const Polygon &loop_polygon
) {
    using Dir = Geometry::Direction1D;

    Dir result{flipped ? Dir::forward : Dir::backward};

    // In rare cases it may happen that the original geometry perimeter
    // polygon has different direction to the actual extrusion loop.
    // In that case the logic is exactly opposite.
    if (perimeter_polygon.is_clockwise() != loop_polygon.is_clockwise()) {
        result = result == Dir::forward ? Dir::backward : Dir::forward;
    }
    return result;
}

// ORCA: the signed distance of the seam to the outline of the layer below, interpolated along its edge.
static float lower_layer_distance(const SeamChoice &seam_choice, const Perimeters::Perimeter &perimeter)
{
    const std::vector<double> &distances = perimeter.lower_layer_distances;
    if (distances.size() != perimeter.positions.size())
        return std::numeric_limits<float>::lowest();
    if (seam_choice.previous_index == seam_choice.next_index)
        return float(distances[seam_choice.previous_index]);
    const Vec2d a = perimeter.positions[seam_choice.previous_index];
    const Vec2d b = perimeter.positions[seam_choice.next_index];
    const double length = (b - a).norm();
    const double t = length > EPSILON ? std::clamp((seam_choice.position - a).norm() / length, 0., 1.) : 0.;
    return float((1. - t) * distances[seam_choice.previous_index] + t * distances[seam_choice.next_index]);
}

static Point finalize_seam_position(
    const ExtrusionLoop &loop,
    SeamChoice seam_choice,
    const Perimeters::Perimeter &perimeter,
    const bool staggered_inner_seams,
    float &overhang
) {
    using Perimeters::offset_along_perimeter;
    using Perimeters::PointOnPerimeter;

    const Polygon loop_polygon{Geometry::to_polygon(loop)};
    const bool do_staggering{staggered_inner_seams && loop.role() == erPerimeter};
    const double loop_width{loop.paths.empty() ? 0.0 : double(loop.paths.front().width)};

    const Polygon perimeter_polygon{Geometry::scaled(perimeter.positions)};
    const AABBTreeLines::LinesDistancer<Linef> distancer{Geometry::to_unscaled_lines(loop_polygon)};

    auto [loop_line_index, loop_point]{
        project_to_extrusion_loop(seam_choice, perimeter, distancer)};

    const Geometry::Direction1D offset_direction{get_direction(false, perimeter_polygon, loop_polygon)};

    const auto offset_stop_condition{
        [choice_value = get_seam_choice_value(seam_choice, perimeter)](
            const Perimeters::Perimeter &perimeter, const std::size_t index
        ) {
            const unsigned current_point_value{Perimeters::get_point_value(
                perimeter.point_types[index], perimeter.point_classifications[index]
            )};
            if (
                current_point_value < choice_value
                && (
                    perimeter.point_types[index] == Perimeters::PointType::blocker
                    || perimeter.point_classifications[index] == Perimeters::PointClassification::overhang
                )
            ) {
                return true;
            }
            return false;
        }};

    // erPerimeter is an inner perimeter.
    if (do_staggering) {
        const double depth = (loop_point - seam_choice.position).norm() -
            loop_width / 2.0;

        const double staggering_offset{depth};

        std::optional<PointOnPerimeter> staggered_point{offset_along_perimeter(
            {seam_choice.previous_index, seam_choice.next_index, loop_point},
            perimeter,
            staggering_offset,
            offset_direction,
            offset_stop_condition
        )};

        if (staggered_point) {
            seam_choice = *staggered_point;
            std::tie(loop_line_index, loop_point) = project_to_extrusion_loop(seam_choice, perimeter, distancer);
        }
    }

    // ORCA: the classic SeamPlacer's unsupported distance is measured from the extrusion centerline
    // plus 0.4 of its width; the perimeter here is the outline, half a width further out.
    const float lower_distance = lower_layer_distance(seam_choice, perimeter);
    overhang = lower_distance == std::numeric_limits<float>::lowest() ? lower_distance : lower_distance - 0.1f * float(loop_width);

    return Point(Slic3r::scaled(loop_point));
}

struct NearestCorner {
    Vec2d prefered_position;

    std::optional<SeamChoice> operator()(
        const Perimeters::Perimeter &perimeter,
        const Perimeters::PointType point_type,
        const Perimeters::PointClassification point_classification
    ) const {
        std::optional<SeamChoice> corner_candidate;
        double min_distance{std::numeric_limits<double>::infinity()};
        for (std::size_t i{0}; i < perimeter.positions.size(); ++i) {
            if (perimeter.point_types[i] == point_type &&
                perimeter.point_classifications[i] == point_classification &&
                perimeter.angle_types[i] != Perimeters::AngleType::smooth) {
                const Vec2d &point{perimeter.positions[i]};
                const double distance{(prefered_position - point).norm()};
                if (!corner_candidate || distance < min_distance) {
                    corner_candidate = {i, i, point};
                    min_distance = distance;
                }
            }
        }
        return corner_candidate;
    }
};

static std::pair<SeamChoice, std::size_t> place_seam_near(
    const std::vector<Perimeters::BoundedPerimeter> &layer_perimeters,
    const ExtrusionLoop &loop,
    const Point &position,
    const double max_detour
) {
    BoundingBoxes choose_from;
    choose_from.reserve(layer_perimeters.size());
    for (const Perimeters::BoundedPerimeter &perimeter : layer_perimeters) {
        choose_from.push_back(perimeter.bounding_box);
    }

    const Polygon loop_polygon{Geometry::to_polygon(loop)};

    const std::size_t choice_index{
        Geometry::pick_closest_bounding_box(loop_polygon.bounding_box(), choose_from).first};

    const NearestCorner nearest_corner{Slic3r::unscaled(position)};
    const std::optional<SeamChoice> corner_choice{
        Seams::maybe_choose_seam_point(layer_perimeters[choice_index].perimeter, nearest_corner)};

    if (corner_choice) {
        return {*corner_choice, choice_index};
    }

    const Seams::Aligned::Impl::Nearest nearest{Slic3r::unscaled(position), max_detour};
    const SeamChoice nearest_choice{
        Seams::choose_seam_point(layer_perimeters[choice_index].perimeter, nearest)};

    return {nearest_choice, choice_index};
}

static int get_perimeter_count(const Layer *layer){
    int count{0};
    for (const LayerRegion *layer_region : layer->regions()) {
        for (const ExtrusionEntity *ex_entity : layer_region->perimeters.entities) {
            if (ex_entity->is_collection()) { //collection of inner, outer, and overhang perimeters
                count += int(static_cast<const ExtrusionEntityCollection*>(ex_entity)->entities.size());
            }
            else {
                count += 1;
            }
        }
    }
    return count;
}

static bool usable(const Perimeters::Perimeter &perimeter)
{
    return !perimeter.positions.empty() && perimeter.point_types.size() == perimeter.positions.size();
}

Point Placer::place_seam(const Layer *layer, const ExtrusionLoop &loop, const Point &last_pos, float &overhang) const
{
    overhang = std::numeric_limits<float>::lowest();
    const PrintObject *po = layer->object();
    // Must not be called with supprot layer.
    assert(dynamic_cast<const SupportLayer *>(layer) == nullptr);
    // Object layer IDs are incremented by the number of raft layers.
    assert(layer->id() >= po->slicing_parameters().raft_layers());
    const size_t layer_index = layer->id() - po->slicing_parameters().raft_layers();
    const auto params_it = this->params.find(po);
    // ORCA: a loop the seam data does not cover (no external perimeter was found on its layer) starts
    // where the nozzle is, as GCode::extrude_loop() does without a SeamPlacer.
    const Point fallback = last_pos;
    if (params_it == this->params.end())
        return fallback;
    const Params &object_params = params_it->second;

    if (po->config().seam_position.value == spNearest) {
        const auto it = this->perimeters_per_layer.find(po);
        if (it == this->perimeters_per_layer.end() || layer_index >= it->second.size() || it->second[layer_index].empty())
            return fallback;
        const std::vector<Perimeters::BoundedPerimeter> &perimeters{it->second[layer_index]};
        const auto [seam_choice, perimeter_index] =
            place_seam_near(perimeters, loop, last_pos, object_params.max_nearest_detour);
        if (!usable(perimeters[perimeter_index].perimeter))
            return fallback;
        return finalize_seam_position(
            loop, seam_choice, perimeters[perimeter_index].perimeter,
            object_params.staggered_inner_seams, overhang
        );
    } else {
        const auto it = this->seams_per_object.find(po);
        if (it == this->seams_per_object.end() || layer_index >= it->second.size() || it->second[layer_index].empty())
            return fallback;
        const std::vector<SeamPerimeterChoice> &seams_on_perimeters{it->second[layer_index]};

        // Special case.
        // If there are only two perimeters and the current perimeter is hole (clockwise).
        const int perimeter_count{get_perimeter_count(layer)};
        const bool has_2_or_3_perimeters{perimeter_count == 2 || perimeter_count == 3};
        // ORCA: Assemble zgap keeps the hole's own, separated seam.
        if (has_2_or_3_perimeters && po->config().seam_position.value != spAssembleZgap) {
            if (seams_on_perimeters.size() == 2 &&
                seams_on_perimeters[0].perimeter.is_hole !=
                    seams_on_perimeters[1].perimeter.is_hole) {
                const SeamPerimeterChoice &seam_perimeter_choice{
                    seams_on_perimeters[0].perimeter.is_hole ? seams_on_perimeters[1] :
                                                               seams_on_perimeters[0]};
                if (!usable(seam_perimeter_choice.perimeter))
                    return fallback;
                return finalize_seam_position(
                    loop, seam_perimeter_choice.choice, seam_perimeter_choice.perimeter,
                    object_params.staggered_inner_seams, overhang
                );
            }
        }

        const SeamPerimeterChoice &seam_perimeter_choice{
            choose_closest_seam(seams_on_perimeters, Geometry::to_polygon(loop))};
        if (!usable(seam_perimeter_choice.perimeter))
            return fallback;
        return finalize_seam_position(
            loop, seam_perimeter_choice.choice, seam_perimeter_choice.perimeter,
            object_params.staggered_inner_seams, overhang
        );
    }
}
} // namespace Slic3r::Seams
