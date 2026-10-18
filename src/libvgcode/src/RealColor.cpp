///|/ Orca Zoukei: RealColor view, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "RealColor.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace libvgcode {

namespace {

// Light reflected by what is under the first bead (the plate and the part's own body), linear.
constexpr float kBacking = 0.6f;
// TD used for a filament without one: opaque.
constexpr float kOpaqueTd = 0.067f;
// How much of the cell's side mean one bead adds, relative to its height: 1.2 mm of stack is seen from the side.
constexpr float kSideWindowMm = 1.2f;
constexpr size_t kMaxCells = 2500000;
constexpr float kEmptyZ = -1.0e30f;

const std::array<float, 256>& srgb_lut()
{
    static const std::array<float, 256> lut = []() {
        std::array<float, 256> t{};
        for (size_t i = 0; i < t.size(); ++i) {
            const float c = float(i) / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return lut;
}

int linear_to_srgb8(float v)
{
    v = std::min(1.0f, std::max(0.0f, v));
    const float c = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::min(255.0f, std::max(0.0f, c * 255.0f + 0.5f)));
}

struct Grid
{
    float origin_x{ 0.0f };
    float origin_y{ 0.0f };
    float cell{ 0.4f };
    int w{ 0 };
    int h{ 0 };
    std::vector<float> top;      // 3 floats per cell: linear colour seen from above
    std::vector<float> side;     // 3 floats per cell: running mean of the beads, seen from the side
    std::vector<float> z;        // highest bead per cell, kEmptyZ = nothing printed
    std::vector<uint32_t> stamp; // last polyline that touched the cell

    int ix(float x) const { return static_cast<int>(std::floor((x - origin_x) / cell)); }
    int iy(float y) const { return static_cast<int>(std::floor((y - origin_y) / cell)); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    size_t idx(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x); }
};

} // namespace

void apply_realcolor(const std::vector<PathVertex>& vertices, std::vector<float>& colors, const RealColorParams& params)
{
    if (!params.enabled || vertices.size() < 2 || colors.size() != vertices.size())
        return;

    // Footprint of the extrusions
    float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
    bool any = false;
    for (size_t i = 1; i < vertices.size(); ++i) {
        if (!vertices[i].is_extrusion())
            continue;
        any = true;
        for (size_t n = i - 1; n <= i; ++n) {
            min_x = std::min(min_x, vertices[n].position[0]);
            max_x = std::max(max_x, vertices[n].position[0]);
            min_y = std::min(min_y, vertices[n].position[1]);
            max_y = std::max(max_y, vertices[n].position[1]);
        }
    }
    if (!any || !(max_x >= min_x) || !(max_y >= min_y))
        return;

    Grid g;
    g.cell = 0.4f;
    const float pad = 1.5f;
    g.origin_x = min_x - pad;
    g.origin_y = min_y - pad;
    const float span_x = (max_x - min_x) + 2.0f * pad;
    const float span_y = (max_y - min_y) + 2.0f * pad;
    for (;;) {
        g.w = static_cast<int>(std::ceil(span_x / g.cell)) + 1;
        g.h = static_cast<int>(std::ceil(span_y / g.cell)) + 1;
        if (static_cast<size_t>(g.w) * static_cast<size_t>(g.h) <= kMaxCells)
            break;
        g.cell *= 1.25f;
    }
    const size_t cells = static_cast<size_t>(g.w) * static_cast<size_t>(g.h);
    g.top.assign(cells * 3, kBacking);
    g.side.assign(cells * 3, kBacking);
    g.z.assign(cells, kEmptyZ);
    g.stamp.assign(cells, 0u);

    const std::array<float, 256>& lut = srgb_lut();
    const int blur_cells = std::min(12, std::max(0, static_cast<int>(std::lround(params.blur_mm / g.cell))));
    const float wall_blend = std::min(1.0f, std::max(0.0f, params.wall_blend));

    // Lays one bead over the cells under the segment a -> b.
    auto deposit = [&](const PathVertex& a, const PathVertex& b, uint32_t poly_id, const float* lin, float alpha, float beta) {
        const float dx = b.position[0] - a.position[0];
        const float dy = b.position[1] - a.position[1];
        const float len = std::sqrt(dx * dx + dy * dy);
        const float half = std::max(0.5f * b.width, 0.5f * g.cell);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / (0.5f * g.cell))));
        const int r = static_cast<int>(std::ceil(half / g.cell));
        const float reach2 = half * half + 0.25f * g.cell * g.cell;
        for (int s = 0; s <= steps; ++s) {
            const float t = float(s) / float(steps);
            const float px = a.position[0] + dx * t;
            const float py = a.position[1] + dy * t;
            const int cx = g.ix(px);
            const int cy = g.iy(py);
            for (int yy = cy - r; yy <= cy + r; ++yy) {
                for (int xx = cx - r; xx <= cx + r; ++xx) {
                    if (!g.inside(xx, yy))
                        continue;
                    const float ex = g.origin_x + (float(xx) + 0.5f) * g.cell - px;
                    const float ey = g.origin_y + (float(yy) + 0.5f) * g.cell - py;
                    if (ex * ex + ey * ey > reach2)
                        continue;
                    const size_t id = g.idx(xx, yy);
                    if (g.stamp[id] == poly_id)
                        continue;
                    g.stamp[id] = poly_id;
                    float* tp = &g.top[id * 3];
                    float* sd = &g.side[id * 3];
                    const bool first = g.z[id] <= kEmptyZ;
                    for (int k = 0; k < 3; ++k) {
                        tp[k] = alpha * lin[k] + (1.0f - alpha) * tp[k];
                        sd[k] = first ? lin[k] : sd[k] + (lin[k] - sd[k]) * beta;
                    }
                    g.z[id] = std::max(g.z[id], b.position[2]);
                }
            }
        }
    };

    // Colour a vertex shows once its layer is complete.
    auto sample = [&](const PathVertex& a, const PathVertex& b, float* out) {
        const int cx = g.ix(0.5f * (a.position[0] + b.position[0]));
        const int cy = g.iy(0.5f * (a.position[1] + b.position[1]));
        float top_sum[3] = { 0.0f, 0.0f, 0.0f };
        float side_sum[3] = { 0.0f, 0.0f, 0.0f };
        int n = 0;
        for (int yy = cy - blur_cells; yy <= cy + blur_cells; ++yy) {
            for (int xx = cx - blur_cells; xx <= cx + blur_cells; ++xx) {
                if (!g.inside(xx, yy))
                    continue;
                const size_t id = g.idx(xx, yy);
                if (g.z[id] <= kEmptyZ)
                    continue;
                for (int k = 0; k < 3; ++k) {
                    top_sum[k] += g.top[id * 3 + k];
                    side_sum[k] += g.side[id * 3 + k];
                }
                ++n;
            }
        }
        if (n == 0) {
            out[0] = kBacking;
            out[1] = kBacking;
            out[2] = kBacking;
            return;
        }
        // Edge or wall: a probe that finds nothing, or something clearly lower, is an open side
        const float zref = b.position[2];
        const float drop = 1.5f * std::max(b.height, 0.05f);
        const int probe = blur_cells + 2;
        const int probe_x[4] = { cx + probe, cx - probe, cx, cx };
        const int probe_y[4] = { cy, cy, cy + probe, cy - probe };
        int open = 0;
        for (int k = 0; k < 4; ++k) {
            if (!g.inside(probe_x[k], probe_y[k]) || g.z[g.idx(probe_x[k], probe_y[k])] < zref - drop)
                ++open;
        }
        const float wall = std::min(1.0f, 0.6f * float(open)) * wall_blend;
        const float inv = 1.0f / float(n);
        for (int k = 0; k < 3; ++k)
            out[k] = top_sum[k] * inv * (1.0f - wall) + side_sum[k] * inv * wall;
    };

    uint32_t poly_id = 0;
    size_t last_end = 0; // index of the previous extrusion vertex, 0 = none
    size_t i = 1;
    while (i < vertices.size()) {
        // One layer: consecutive vertices with the same layer id
        size_t j = i;
        while (j < vertices.size() && vertices[j].layer_id == vertices[i].layer_id)
            ++j;

        for (size_t k = i; k < j; ++k) {
            const PathVertex& v = vertices[k];
            if (!v.is_extrusion())
                continue;
            const PathVertex& p = vertices[k - 1];
            // A new polyline unless this segment continues the previous bead
            const bool continues = last_end != 0 && last_end == k - 1 &&
                vertices[last_end].extruder_id == v.extruder_id &&
                std::abs(vertices[last_end].position[2] - v.position[2]) < 0.001f;
            if (!continues)
                ++poly_id;
            last_end = k;

            const int c = static_cast<int>(colors[k]);
            const float lin[3] = { lut[(c >> 16) & 0xFF], lut[(c >> 8) & 0xFF], lut[c & 0xFF] };
            const float td = size_t(v.extruder_id) < params.td_mm.size() && params.td_mm[v.extruder_id] > 0.0f ?
                params.td_mm[v.extruder_id] : kOpaqueTd;
            const float height = v.height > 0.0f ? v.height : 0.2f;
            const float alpha = std::min(1.0f, std::max(0.02f, 1.0f - std::pow(0.1f, height / td)));
            const float beta = std::min(1.0f, height / kSideWindowMm);
            deposit(p, v, poly_id, lin, alpha, beta);
        }

        for (size_t k = i; k < j; ++k) {
            const PathVertex& v = vertices[k];
            if (!v.is_extrusion())
                continue;
            float out[3];
            sample(vertices[k - 1], v, out);
            const int cr = linear_to_srgb8(out[0]);
            const int cg = linear_to_srgb8(out[1]);
            const int cb = linear_to_srgb8(out[2]);
            colors[k] = static_cast<float>((cr << 16) | (cg << 8) | cb);
        }
        i = j;
    }
}

} // namespace libvgcode
