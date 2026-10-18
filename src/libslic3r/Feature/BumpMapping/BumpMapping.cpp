#include "BumpMapping.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

// Ported from OrcaFS-NeotkoCM (Neotko): Feature/TextureBump (wall relief) and Feature/ZBump (top relief).
// Simplified: single per-object config, no painted zones, no reinforcement passes, Arachne walls only.

namespace Slic3r::Feature::Bump {

// ---------------------------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------------------------

std::shared_ptr<const png::ImageGreyscale> load_image(const std::string &path)
{
    static std::mutex                                                                  s_mutex;
    static std::unordered_map<std::string, std::shared_ptr<const png::ImageGreyscale>> s_cache;

    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(path);
    if (it != s_cache.end() && it->second)
        return it->second;

    std::shared_ptr<png::ImageGreyscale> result;
    std::ifstream                        in(path, std::ios::binary);
    if (in.is_open()) {
        std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!buf.empty()) {
            png::ReadBuf rbuf{buf.data(), buf.size()};
            auto         img = std::make_shared<png::ImageGreyscale>();
            if (png::is_png(rbuf) && png::decode_png(rbuf, *img) && img->rows > 0 && img->cols > 0) {
                result = img;
            } else if (png::is_png(rbuf)) {
                // Colour PNG (screenshots, exports): take the luma. decode_colored_png() returns the
                // rows bottom-up, so flip them to match the greyscale decoder.
                png::ImageColorscale cimg;
                if (png::decode_colored_png(rbuf, cimg) && cimg.rows > 0 && cimg.cols > 0 && cimg.bytes_per_pixel >= 1 &&
                    cimg.buf.size() == cimg.rows * cimg.cols * size_t(cimg.bytes_per_pixel)) {
                    auto gimg  = std::make_shared<png::ImageGreyscale>();
                    gimg->rows = cimg.rows;
                    gimg->cols = cimg.cols;
                    gimg->buf.resize(cimg.rows * cimg.cols);
                    const size_t bpp = size_t(cimg.bytes_per_pixel);
                    for (size_t r = 0; r < cimg.rows; ++r)
                        for (size_t c = 0; c < cimg.cols; ++c) {
                            const uint8_t *src  = cimg.buf.data() + ((cimg.rows - 1 - r) * cimg.cols + c) * bpp;
                            uint8_t        luma = src[0];
                            if (bpp >= 3)
                                luma = uint8_t(std::clamp(0.299 * double(src[0]) + 0.587 * double(src[1]) + 0.114 * double(src[2]), 0.0, 255.0));
                            gimg->buf[r * cimg.cols + c] = luma;
                        }
                    result = gimg;
                }
            }
        }
    }
    if (result)
        s_cache[path] = result; // failures are not cached, so fixing the file takes effect on the next slice
    return result;
}

double sample_bilinear(const png::ImageGreyscale &img, double u, double v)
{
    u = u - std::floor(u);
    v = v - std::floor(v);
    const double fx = u * double(img.cols);
    const double fy = v * double(img.rows);
    const size_t x0 = size_t(std::floor(fx)) % img.cols;
    const size_t y0 = size_t(std::floor(fy)) % img.rows;
    const size_t x1 = (x0 + 1) % img.cols;
    const size_t y1 = (y0 + 1) % img.rows;
    const double tx = fx - std::floor(fx);
    const double ty = fy - std::floor(fy);

    const double v00 = double(img.get(y0, x0)) / 255.0;
    const double v10 = double(img.get(y0, x1)) / 255.0;
    const double v01 = double(img.get(y1, x0)) / 255.0;
    const double v11 = double(img.get(y1, x1)) / 255.0;

    const double top    = v00 * (1.0 - tx) + v10 * tx;
    const double bottom = v01 * (1.0 - tx) + v11 * tx;
    return top * (1.0 - ty) + bottom * ty;
}

// ---------------------------------------------------------------------------------------------
// Wall bump
// ---------------------------------------------------------------------------------------------

WallBumpConfig resolve_wall_bump_config(const PrintObjectConfig &oc)
{
    WallBumpConfig cfg;
    cfg.enabled           = oc.bump_wall_enabled.value;
    cfg.image_path        = oc.bump_wall_image.value;
    cfg.thickness_mm      = oc.bump_wall_thickness.value;
    cfg.scale_mm          = oc.bump_wall_scale.value;
    cfg.repeat_u          = std::max(1, oc.bump_wall_repeat.value);
    cfg.projection        = std::clamp(oc.bump_wall_projection.value, 0, 2);
    cfg.point_distance_mm = oc.bump_wall_point_distance.value;
    cfg.max_angle_rad     = oc.bump_wall_max_angle.value * M_PI / 180.0;
    return cfg;
}

bool wall_bump_active(const PrintObjectConfig &oc)
{
    return oc.bump_wall_enabled.value && !oc.bump_wall_image.value.empty() && oc.bump_wall_thickness.value >= 0.001 &&
           oc.bump_wall_point_distance.value >= 0.01 && oc.bump_wall_scale.value > 0.0;
}

namespace {

// Smoothstep weight, 0 at the seam, 1 once dist reaches half_width.
double smoothstep_blend(double dist, double half_width)
{
    const double t = std::clamp(dist / std::max(half_width, 1e-9), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Raw displacement in [-1, 1]. Both sides of every period boundary blend toward the image own
// u=0 column, so the value is continuous across the wrap whether or not the PNG tiles cleanly.
double sample_seam_blended(const png::ImageGreyscale &img, double u_canonical, int num_periods, double seam_half_width, double v)
{
    num_periods    = std::max(num_periods, 1);
    double u_local = std::fmod(u_canonical, 1.0);
    if (u_local < 0.0)
        u_local += 1.0;
    const double phase = std::fmod(u_local * double(num_periods), 1.0);
    const double raw   = sample_bilinear(img, phase, v) * 2.0 - 1.0;
    if (seam_half_width <= 0.0)
        return raw;
    const double dist = std::min(phase, 1.0 - phase);
    if (dist >= seam_half_width)
        return raw;
    const double t      = smoothstep_blend(dist, seam_half_width);
    const double anchor = sample_bilinear(img, 0.0, v) * 2.0 - 1.0;
    return anchor * (1.0 - t) + raw * t;
}

// Wall point (object-local mm, centred like PrintObject::size()) to a canonical u in [0,1).
double compute_u(const Vec3d &point_mm, const Vec2d &perp_dir, int projection, const BoundingBoxf3 &bounds)
{
    const Vec3d center = bounds.center();
    switch (projection) {
    case 1: { // cylindrical around Z
        const double theta = std::atan2(point_mm.y() - center.y(), point_mm.x() - center.x());
        return theta / (2.0 * M_PI) + 0.5;
    }
    case 2: { // cubic: 4 side faces, dominant axis picked from the wall normal
        const double normal_radians = std::atan2(perp_dir.y(), perp_dir.x());
        const double x              = point_mm.x() - center.x();
        const double y              = point_mm.y() - center.y();
        const double half_x         = std::max(bounds.size().x() * 0.5, 1e-6);
        const double half_y         = std::max(bounds.size().y() * 0.5, 1e-6);
        double       face_u         = 0.0;
        int          face_idx       = 0;
        if (normal_radians >= -M_PI_4 && normal_radians < M_PI_4) {
            face_idx = 0;
            face_u   = 0.5 + 0.5 * (y / half_y);
        } else if (normal_radians >= M_PI_4 && normal_radians < 3.0 * M_PI_4) {
            face_idx = 1;
            face_u   = 0.5 - 0.5 * (x / half_x);
        } else if (normal_radians >= -3.0 * M_PI_4 && normal_radians < -M_PI_4) {
            face_idx = 3;
            face_u   = 0.5 + 0.5 * (x / half_x);
        } else {
            face_idx = 2;
            face_u   = 0.5 - 0.5 * (y / half_y);
        }
        return (double(face_idx) + std::clamp(face_u, 0.0, 1.0)) / 4.0;
    }
    case 0:
    default: // planar along X
        return (point_mm.x() - center.x()) / std::max(bounds.size().x(), 1e-6) + 0.5;
    }
}

// Limits the change of displacement between consecutive layers to max_angle, so the relief never
// implies an overhang steeper than configured (support generation only sees the real mesh).
void apply_slope_limiter(const WallBumpConfig &cfg, const std::vector<coordf_t> &layer_z, size_t num_columns, std::vector<double> &values)
{
    const size_t num_layers = layer_z.size();
    if (num_layers < 2 || num_columns == 0)
        return;
    for (size_t col = 0; col < num_columns; ++col) {
        std::vector<bool> violated(num_layers, false);
        bool              any_violation = false;
        for (size_t li = 1; li < num_layers; ++li) {
            const double dz = std::max(layer_z[li] - layer_z[li - 1], 1e-6);
            const double dr = values[li * num_columns + col] - values[(li - 1) * num_columns + col];
            if (std::atan2(std::abs(dr), dz) > cfg.max_angle_rad) {
                violated[li]     = true;
                violated[li - 1] = true;
                any_violation    = true;
            }
        }
        if (!any_violation)
            continue;
        constexpr int       kBlurRadius = 2;
        std::vector<double> blurred(num_layers);
        for (size_t li = 0; li < num_layers; ++li)
            blurred[li] = values[li * num_columns + col];
        for (size_t li = 0; li < num_layers; ++li) {
            if (!violated[li])
                continue;
            double sum   = 0.0;
            int    count = 0;
            for (int off = -kBlurRadius; off <= kBlurRadius; ++off) {
                const long idx = long(li) + off;
                if (idx < 0 || idx >= long(num_layers))
                    continue;
                sum += values[size_t(idx) * num_columns + col];
                ++count;
            }
            if (count > 0)
                blurred[li] = sum / double(count);
        }
        for (size_t li = 0; li < num_layers; ++li)
            values[li * num_columns + col] = blurred[li];
        // Hard clamp: guarantees the angle even where the blur alone was not enough.
        for (size_t li = 1; li < num_layers; ++li) {
            const double dz     = std::max(layer_z[li] - layer_z[li - 1], 1e-6);
            const double max_dr = std::tan(cfg.max_angle_rad) * dz;
            double      &prev   = values[(li - 1) * num_columns + col];
            double      &cur    = values[li * num_columns + col];
            const double dr     = cur - prev;
            if (std::abs(dr) > max_dr)
                cur = prev + std::copysign(max_dr, dr);
        }
    }
}

} // namespace

void WallBumpTable::build(const WallBumpConfig &cfg, const BoundingBoxf3 &object_bounds_mm, const std::vector<coordf_t> &layer_z)
{
    m_values.clear();
    m_num_columns = 0;
    m_num_layers  = 0;
    m_bounds_mm   = object_bounds_mm;
    m_config      = cfg;
    if (!cfg.enabled || cfg.image_path.empty() || layer_z.empty())
        return;
    std::shared_ptr<const png::ImageGreyscale> img = load_image(cfg.image_path);
    if (!img)
        return;

    constexpr size_t kNumColumns    = 256;
    constexpr double kSeamHalfWidth = 0.03;

    m_num_columns = kNumColumns;
    m_num_layers  = layer_z.size();
    m_values.assign(m_num_layers * m_num_columns, 0.0);

    const int    base_periods = cfg.projection == 2 ? 4 : 1;
    const int    num_periods  = base_periods * std::max(cfg.repeat_u, 1);
    const bool   has_seam     = cfg.projection != 0 || num_periods > 1;
    const double scale_mm     = std::max(cfg.scale_mm, 1e-6);

    for (size_t li = 0; li < m_num_layers; ++li) {
        const double v = std::max(layer_z[li], 0.0) / scale_mm;
        for (size_t ci = 0; ci < m_num_columns; ++ci) {
            const double u   = double(ci) / double(m_num_columns);
            const double raw = has_seam ? sample_seam_blended(*img, u, num_periods, kSeamHalfWidth, v) :
                                          (sample_bilinear(*img, u, v) * 2.0 - 1.0);
            // Outward only: remap [-1,1] to [0,1], so the wall never moves into the infill.
            m_values[li * m_num_columns + ci] = ((raw + 1.0) * 0.5) * cfg.thickness_mm;
        }
    }
    apply_slope_limiter(cfg, layer_z, m_num_columns, m_values);
}

double WallBumpTable::sample(double u_canonical, int layer_idx) const
{
    if (empty() || layer_idx < 0 || size_t(layer_idx) >= m_num_layers)
        return 0.0;
    double u = std::fmod(u_canonical, 1.0);
    if (u < 0.0)
        u += 1.0;
    const double fx = u * double(m_num_columns);
    const size_t x0 = size_t(std::floor(fx)) % m_num_columns;
    const size_t x1 = (x0 + 1) % m_num_columns;
    const double tx = fx - std::floor(fx);
    const double v0 = m_values[size_t(layer_idx) * m_num_columns + x0];
    const double v1 = m_values[size_t(layer_idx) * m_num_columns + x1];
    return v0 * (1.0 - tx) + v1 * tx;
}

namespace {
struct WallTableEntry
{
    WallBumpConfig cfg;
    size_t         num_layers = 0;
    coordf_t       last_z     = 0.;
    Vec3crd        size       = Vec3crd::Zero();
    WallBumpTable  table;
};
} // namespace

const WallBumpTable *get_wall_table(const PrintObject *object)
{
    static std::mutex                                              s_mutex;
    static std::unordered_map<const PrintObject *, WallTableEntry> s_cache;

    if (object == nullptr)
        return nullptr;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!wall_bump_active(object->config())) {
        s_cache.erase(object);
        return nullptr;
    }
    const WallBumpConfig cfg    = resolve_wall_bump_config(object->config());
    const auto           layers = object->layers();
    if (layers.empty())
        return nullptr;
    const coordf_t  last_z = layers.back()->slice_z;
    WallTableEntry &e      = s_cache[object];
    if (e.cfg != cfg || e.num_layers != layers.size() || e.last_z != last_z || e.size != object->size() || e.table.empty()) {
        BoundingBoxf3 bounds;
        bounds.min = Vec3d(-unscale_(object->size().x()) / 2.0, -unscale_(object->size().y()) / 2.0, 0.0);
        bounds.max = Vec3d(unscale_(object->size().x()) / 2.0, unscale_(object->size().y()) / 2.0, unscale_(object->size().z()));
        std::vector<coordf_t> layer_z;
        layer_z.reserve(layers.size());
        for (const Layer *l : layers)
            layer_z.push_back(l->slice_z);
        e.table.build(cfg, bounds, layer_z);
        e.cfg        = cfg;
        e.num_layers = layers.size();
        e.last_z     = last_z;
        e.size       = object->size();
    }
    return e.table.empty() ? nullptr : &e.table;
}

namespace {

constexpr double kMinExtrusionWidthMm = 0.05;
constexpr double kMaxWidthMultiplier  = 2.0;

// 1.0 at the outermost wall, tapering to 0 at the innermost one (which stays bonded to the infill).
double effect_scale_for(size_t loop_idx, int total_loops)
{
    if (total_loops <= 0)
        return 1.0;
    return 1.0 - std::min(1.0, double(loop_idx) / double(total_loops));
}

// Same interpolated-point walk as fuzzy skin, but the perpendicular offset comes from the table.
// Position and width are blended by effect_scale: the outermost wall is 100% position, the
// innermost 100% width anchored so its inner edge (facing the infill) never moves.
void wall_bump_junctions(Arachne::ExtrusionJunctions &ext_lines, const PerimeterGenerator &pg, const WallBumpTable &table, double effect_scale)
{
    const WallBumpConfig &cfg            = table.config();
    const double          min_dist_mm    = std::max(cfg.point_distance_mm * 3.0 / 4.0, 0.01);
    const double          step_scaled    = scale_(min_dist_mm);
    double                dist_left_over = step_scaled / 2.0;

    auto *                      p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    for (auto &p1 : ext_lines) {
        if (p0->p == p1.p) { // connect endpoints
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }
        Vec2d  p0p1      = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += step_scaled) {
            Point        pa       = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            const Vec2d  perp_dir = perp(p0p1).cast<double>().normalized();
            const Vec3d  point_mm(unscale_(pa.x()), unscale_(pa.y()), pg.slice_z);
            const double u = compute_u(point_mm, perp_dir, cfg.projection, table.bounds());

            const double r_mm_raw       = table.sample(u, pg.layer_id);
            const double r_position_mm  = r_mm_raw * effect_scale;
            const double width_delta_mm = r_mm_raw * (1.0 - effect_scale);

            const double w_mm            = unscale_(p1.w);
            const double max_rad_mm      = w_mm * kMaxWidthMultiplier;
            const double rad_mm          = std::clamp(w_mm + width_delta_mm, kMinExtrusionWidthMm, std::max(max_rad_mm, kMinExtrusionWidthMm));
            const double center_shift_mm = r_position_mm + (rad_mm - w_mm) / 2.0;

            out.emplace_back(pa + (perp_dir * scale_(center_shift_mm)).cast<coord_t>(), coord_t(scale_(rad_mm)), p1.perimeter_index);
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0             = &p1;
    }

    while (out.size() < 3) {
        if (ext_lines.size() < 2)
            return;
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
    }
    if (ext_lines.back().p == ext_lines.front().p) { // keep the loop closed
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }
    if (out.size() >= 3)
        ext_lines = std::move(out);
}

} // namespace

void apply_wall_bump(Arachne::ExtrusionLine *extrusion, const PerimeterGenerator &pg, bool is_contour, int total_loops)
{
    (void) is_contour; // contours and holes both get the relief (all walls)
    if (pg.wall_bump_table == nullptr || extrusion == nullptr || extrusion->junctions.size() < 3)
        return;
    if (!extrusion->is_closed) // thin / open walls are left alone
        return;
    if (pg.layer_id <= 0) // first layer stays flat for bed adhesion
        return;
    wall_bump_junctions(extrusion->junctions, pg, *pg.wall_bump_table, effect_scale_for(extrusion->inset_idx, total_loops));
}

// ---------------------------------------------------------------------------------------------
// ZBump
// ---------------------------------------------------------------------------------------------

ZBumpConfig resolve_zbump_config(const PrintObjectConfig &oc)
{
    ZBumpConfig cfg;
    cfg.enabled           = oc.zbump_enabled.value;
    cfg.image_path        = oc.zbump_image.value;
    cfg.thickness_mm      = oc.zbump_height.value;
    cfg.scale_mm          = oc.zbump_scale.value;
    cfg.repeat            = std::max(1, oc.zbump_repeat.value);
    cfg.offset_x_mm       = oc.zbump_offset_x.value;
    cfg.offset_y_mm       = oc.zbump_offset_y.value;
    cfg.edge_margin_mm    = oc.zbump_edge_margin.value;
    cfg.max_slope         = oc.zbump_max_slope.value;
    cfg.relief_segment_mm = oc.zbump_segment.value;
    return cfg;
}

namespace {

std::vector<double> gauss_kernel_1d(unsigned int radius)
{
    const unsigned int  size = 2 * radius + 1;
    std::vector<double> k(size);
    const double        sigma        = 0.3 * double(radius - 1) + 0.8;
    const double        two_sq_sigma = 2.0 * sigma * sigma;
    const double        norm         = 1.0 / std::sqrt(M_PI * two_sq_sigma);
    for (unsigned int i = 0; i < size; ++i) {
        const double x = double(i) - double(radius);
        k[i]           = norm * std::exp(-x * x / two_sq_sigma);
    }
    double sum = 0.0;
    for (double v : k)
        sum += v;
    if (sum > 1e-9)
        for (double &v : k)
            v /= sum;
    return k;
}

std::vector<double> gaussian_blur_2d(const std::vector<double> &src, size_t cols, size_t rows, unsigned int radius)
{
    if (radius == 0 || cols == 0 || rows == 0)
        return src;
    const std::vector<double> kernel = gauss_kernel_1d(radius);
    const long                r      = long(radius);
    std::vector<double>       tmp(src.size(), 0.0);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < cols; ++x) {
            double acc = 0.0;
            for (long k = -r; k <= r; ++k)
                acc += src[y * cols + size_t(std::clamp<long>(long(x) + k, 0, long(cols) - 1))] * kernel[size_t(k + r)];
            tmp[y * cols + x] = acc;
        }
    std::vector<double> out(src.size(), 0.0);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < cols; ++x) {
            double acc = 0.0;
            for (long k = -r; k <= r; ++k)
                acc += tmp[size_t(std::clamp<long>(long(y) + k, 0, long(rows) - 1)) * cols + x] * kernel[size_t(k + r)];
            out[y * cols + x] = acc;
        }
    return out;
}

} // namespace

void ZBumpHeightMap::build(const ZBumpConfig &cfg, const BoundingBoxf3 &object_bounds_mm)
{
    m_config    = cfg;
    m_bounds_mm = object_bounds_mm;
    m_cols = m_rows = 0;
    m_values.clear();
    if (!cfg.enabled || cfg.image_path.empty() || cfg.thickness_mm <= 0.0)
        return;
    std::shared_ptr<const png::ImageGreyscale> img = load_image(cfg.image_path);
    if (!img || img->rows == 0 || img->cols == 0)
        return;

    const double width_mm  = std::max(0.01, object_bounds_mm.max.x() - object_bounds_mm.min.x());
    const double height_mm = std::max(0.01, object_bounds_mm.max.y() - object_bounds_mm.min.y());
    const double tile_mm   = std::max(0.1, cfg.scale_mm);
    const int    repeat    = std::max(1, cfg.repeat);

    // Grid density follows the object size (4 samples/mm), times the repeat count.
    constexpr double kSamplesPerMm = 4.0;
    m_cols = size_t(std::clamp(width_mm * kSamplesPerMm * double(repeat), 2.0, 512.0));
    m_rows = size_t(std::clamp(height_mm * kSamplesPerMm * double(repeat), 2.0, 512.0));

    std::vector<double> raw(m_cols * m_rows, 0.0);
    for (size_t r = 0; r < m_rows; ++r) {
        const double y_mm = object_bounds_mm.min.y() + (double(r) + 0.5) / double(m_rows) * height_mm;
        for (size_t c = 0; c < m_cols; ++c) {
            const double x_mm = object_bounds_mm.min.x() + (double(c) + 0.5) / double(m_cols) * width_mm;
            double       u    = std::fmod((x_mm - cfg.offset_x_mm) / tile_mm * double(repeat), 1.0);
            double       v    = std::fmod((y_mm - cfg.offset_y_mm) / tile_mm * double(repeat), 1.0);
            if (u < 0.0)
                u += 1.0;
            if (v < 0.0)
                v += 1.0;
            raw[r * m_cols + c] = sample_bilinear(*img, u, v) * cfg.thickness_mm;
        }
    }

    // Slope limiter: blur radius derived from max_slope so no cell step is steeper than configured.
    const double       cell_mm        = std::min(width_mm / double(m_cols), height_mm / double(m_rows));
    const double       needed_ramp_mm = (cfg.max_slope > 1e-6) ? (cfg.thickness_mm / cfg.max_slope) : 0.0;
    const unsigned int radius         = (unsigned int) std::clamp(std::round(needed_ramp_mm / std::max(0.01, cell_mm)), 0.0, 32.0);
    m_values                          = (radius > 0) ? gaussian_blur_2d(raw, m_cols, m_rows, radius) : std::move(raw);
}

double ZBumpHeightMap::sample_xy(double x_mm, double y_mm) const
{
    if (empty())
        return 0.0;
    const double width_mm  = std::max(0.01, m_bounds_mm.max.x() - m_bounds_mm.min.x());
    const double height_mm = std::max(0.01, m_bounds_mm.max.y() - m_bounds_mm.min.y());
    double       fx        = (x_mm - m_bounds_mm.min.x()) / width_mm * double(m_cols) - 0.5;
    double       fy        = (y_mm - m_bounds_mm.min.y()) / height_mm * double(m_rows) - 0.5;
    fx                     = std::clamp(fx, 0.0, double(m_cols - 1));
    fy                     = std::clamp(fy, 0.0, double(m_rows - 1));
    const size_t x0 = size_t(fx), y0 = size_t(fy);
    const size_t x1 = std::min(x0 + 1, m_cols - 1), y1 = std::min(y0 + 1, m_rows - 1);
    const double tx = fx - double(x0), ty = fy - double(y0);
    const double v0 = m_values[y0 * m_cols + x0] * (1.0 - tx) + m_values[y0 * m_cols + x1] * tx;
    const double v1 = m_values[y1 * m_cols + x0] * (1.0 - tx) + m_values[y1 * m_cols + x1] * tx;
    return v0 * (1.0 - ty) + v1 * ty;
}

double compute_pass_cap_mm(double nozzle_diameter_mm, double layer_height_mm)
{
    return std::max(0.0, 0.8 * nozzle_diameter_mm - layer_height_mm);
}

namespace {

const ZBumpHeightMap &get_or_build_height_map(const void *object_key, const ZBumpConfig &cfg, const BoundingBoxf3 &bounds)
{
    static std::mutex                                       s_mutex;
    static std::unordered_map<const void *, ZBumpHeightMap> s_cache;
    std::lock_guard<std::mutex>                             lock(s_mutex);
    ZBumpHeightMap                                         &m = s_cache[object_key];
    if (m.config() != cfg || (m.empty() && cfg.enabled))
        m.build(cfg, bounds);
    return m;
}

double edge_ramp_factor(double dist_to_edge_mm, double margin_mm)
{
    if (margin_mm <= 1e-6)
        return 1.0;
    const double t = std::clamp(dist_to_edge_mm / margin_mm, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double distance_to_boundary_mm(const ExPolygon &expoly, const Vec2d &pt_mm)
{
    const Point pt(scaled<coord_t>(pt_mm.x()), scaled<coord_t>(pt_mm.y()));
    double      min_d = std::numeric_limits<double>::max();
    auto        scan  = [&](const Polygon &poly) {
        for (const Line &l : poly.lines())
            min_d = std::min(min_d, l.distance_to(pt));
    };
    scan(expoly.contour);
    for (const Polygon &hole : expoly.holes)
        scan(hole);
    return min_d == std::numeric_limits<double>::max() ? 0.0 : min_d * SCALING_FACTOR;
}

// Splits a point list so no segment exceeds max_segment_scaled; original points are kept.
Points split_for_relief(const Points &src, double max_segment_scaled)
{
    if (src.size() < 2 || max_segment_scaled <= 1.0)
        return src;
    Points out;
    out.reserve(src.size());
    out.push_back(src.front());
    for (size_t i = 1; i < src.size(); ++i) {
        const Point &a       = src[i - 1];
        const Point &b       = src[i];
        const double seg_len = (b - a).cast<double>().norm();
        const int    n_sub   = std::max(1, int(std::ceil(seg_len / max_segment_scaled)));
        for (int k = 1; k <= n_sub; ++k) {
            const double t = double(k) / double(n_sub);
            out.emplace_back(coord_t(std::round(double(a.x()) + t * double(b.x() - a.x()))),
                             coord_t(std::round(double(a.y()) + t * double(b.y() - a.y()))));
        }
    }
    return out;
}

} // namespace

void apply_zbump_to_range(const Layer &layer, const PrintRegionConfig &region_config, const ExPolygon &top_surface,
                          ExtrusionEntitiesPtr &entities, size_t first_entity)
{
    const PrintObject *object = layer.object();
    if (object == nullptr || !object->config().zbump_enabled.value || first_entity >= entities.size())
        return;
    if (layer.id() == 0 || region_config.zaa_enabled.value) // first layer stays flat; ZAA owns the Z of its paths
        return;
    const ZBumpConfig cfg = resolve_zbump_config(object->config());
    if (cfg.image_path.empty() || cfg.thickness_mm <= 0.0)
        return;

    BoundingBoxf3 bounds;
    bounds.min = Vec3d(-unscale_(object->size().x()) / 2.0, -unscale_(object->size().y()) / 2.0, 0.0);
    bounds.max = Vec3d(unscale_(object->size().x()) / 2.0, unscale_(object->size().y()) / 2.0, unscale_(object->size().z()));
    const ZBumpHeightMap &map = get_or_build_height_map(object, cfg, bounds);
    if (map.empty())
        return;

    const int    extruder  = std::max(0, region_config.top_surface_filament_id.value - 1);
    const double nozzle_mm = object->print()->config().nozzle_diameter.get_at(extruder);
    const double cap_mm    = std::min(cfg.thickness_mm, compute_pass_cap_mm(nozzle_mm, double(layer.height)));
    if (cap_mm <= 1e-6)
        return;
    const double max_segment_scaled = scale_(std::max(0.1, cfg.relief_segment_mm));

    auto bump_one_path = [&](ExtrusionPath &path) {
        if (path.polyline.points.size() < 2 || path.z_contoured || path.role() == erIroning)
            return;
        Points pts_2d;
        pts_2d.reserve(path.polyline.points.size());
        for (const Point3 &p : path.polyline.points)
            pts_2d.emplace_back(p.x(), p.y());
        Points              new_points = split_for_relief(pts_2d, max_segment_scaled);
        std::vector<double> h_mm(new_points.size(), 0.0);
        bool                any_bumped = false;
        for (size_t i = 0; i < new_points.size(); ++i) {
            const Vec2d  pt_mm = unscale(new_points[i]);
            const double raw_h = map.sample_xy(pt_mm.x(), pt_mm.y());
            if (raw_h <= 1e-6)
                continue;
            const double ramp = edge_ramp_factor(distance_to_boundary_mm(top_surface, pt_mm), cfg.edge_margin_mm);
            const double h    = std::clamp(raw_h * ramp, 0.0, cap_mm);
            if (h > 1e-6) {
                h_mm[i]    = h;
                any_bumped = true;
            }
        }
        if (!any_bumped)
            return;
        Polyline3 poly;
        poly.points.reserve(new_points.size());
        for (size_t i = 0; i < new_points.size(); ++i)
            poly.points.emplace_back(Point3(new_points[i].x(), new_points[i].y(), scaled<coord_t>(h_mm[i])));
        path.polyline    = std::move(poly);
        path.z_contoured = true;
    };

    std::function<void(ExtrusionEntity *)> walk = [&](ExtrusionEntity *e) {
        if (auto *p = dynamic_cast<ExtrusionPath *>(e)) {
            bump_one_path(*p);
        } else if (auto *mp = dynamic_cast<ExtrusionMultiPath *>(e)) {
            for (ExtrusionPath &sp : mp->paths)
                bump_one_path(sp);
        } else if (auto *lp = dynamic_cast<ExtrusionLoop *>(e)) {
            for (ExtrusionPath &sp : lp->paths)
                bump_one_path(sp);
        } else if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(e)) {
            for (ExtrusionEntity *c : coll->entities)
                walk(c);
        }
    };
    for (size_t i = first_entity; i < entities.size(); ++i)
        walk(entities[i]);
}

} // namespace Slic3r::Feature::Bump
