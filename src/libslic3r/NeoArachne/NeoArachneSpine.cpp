// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoArachne v3 "spine" (single-line fill). See NeoArachneSpine.hpp.
#include "NeoArachneSpine.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Geometry/MedialAxis.hpp"
#include "../Line.hpp"
#include "../PerimeterGenerator.hpp"
#include "../PrintConfig.hpp"
#include "../VariableWidth.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

namespace Slic3r { namespace NeoArachne {

bool fits_in_one_line(const ExPolygon &ex, double ceiling_scaled)
{
    return offset_ex(ExPolygons{ ex }, -float(ceiling_scaled / 2.)).empty();
}

// Skeleton cleanup. MedialAxis cuts the lines at every junction and leaves short twigs towards the corners at
// every sharp tip; uncleaned, those twigs reach the border and ride over the main line, and every branch is a
// separate path. Learned upstream:
//   - The branches of one junction do NOT share the exact point (Voronoi splits it into vertices microns apart),
//     so nodes are grouped by PROXIMITY.
//   - A gap pinched down to a point becomes TWO zones touching there, so the graph is cleaned with ALL zones at
//     once, not zone by zone.
//   - A twig is always narrow (it runs into a corner), so it is compared with the gap thickness at the node where
//     it starts, not with its own width.

// Skeleton line + the zone of each end (to extend the tip to ITS own border).
struct SPl {
    ThickPolyline pl;
    size_t        zf = 0;
    size_t        zb = 0;
};

struct EndRef { size_t pl; bool back; };

static bool alive(const SPl &s) { return s.pl.points.size() >= 2; }
static bool is_closed_pl(const ThickPolyline &pl) { return pl.points.front() == pl.points.back(); }

static const Point &end_point(const ThickPolyline &pl, bool back) { return back ? pl.points.back() : pl.points.front(); }
static double end_width(const ThickPolyline &pl, bool back) { return back ? pl.width.back() : pl.width.front(); }

static double max_width(const ThickPolyline &pl)
{
    return pl.width.empty() ? 0. : *std::max_element(pl.width.begin(), pl.width.end());
}

static void reverse_spl(SPl &s)
{
    s.pl.reverse();
    std::swap(s.zf, s.zb);
}

// Direction in which the line LEAVES the node through the given end.
static Vec2d leave_dir(const ThickPolyline &pl, bool back)
{
    const Point &n = back ? pl.points.back() : pl.points.front();
    const Point &q = back ? pl.points[pl.points.size() - 2] : pl.points[1];
    Vec2d d = (q - n).cast<double>();
    const double l = d.norm();
    return l > 0. ? Vec2d(d / l) : d;
}

// Appends `b` after `a` through their common node and empties `b`. If the ends do not coincide exactly (split
// junction) the gap is covered with a bridging segment.
static void join_at(SPl &a, bool a_back, SPl &b, bool b_back)
{
    if (!a_back) reverse_spl(a); // `a` ends at the node
    if (b_back)  reverse_spl(b); // `b` starts at the node
    ThickPolyline &A = a.pl;
    ThickPolyline &B = b.pl;
    if (A.points.back() == B.points.front()) {
        A.points.insert(A.points.end(), B.points.begin() + 1, B.points.end());
        A.width.insert(A.width.end(), B.width.begin(), B.width.end());
    } else {
        const coordf_t wa = A.width.back();
        const coordf_t wb = B.width.front();
        A.points.insert(A.points.end(), B.points.begin(), B.points.end());
        A.width.push_back(wa);
        A.width.push_back(wb);
        A.width.insert(A.width.end(), B.width.begin(), B.width.end());
    }
    A.endpoints.second = B.endpoints.second;
    a.zb = b.zb;
    B.points.clear();
    B.width.clear();
}

// Nodes = groups of nearby ends. `node_of[(pl, back)]` gives the node of each end.
struct Graph {
    std::vector<std::vector<EndRef>>          nodes;
    std::map<std::pair<size_t, bool>, size_t> node_of;
    size_t degree_of(size_t pl, bool back) const { return nodes[node_of.at({ pl, back })].size(); }
    // Gap thickness at the node: the largest width of the ends reaching it.
    double node_width(const std::vector<SPl> &pls, size_t node) const
    {
        double w = 0.;
        for (const EndRef &e : nodes[node])
            w = std::max(w, end_width(pls[e.pl].pl, e.back));
        return w;
    }
};

static Graph build_graph(const std::vector<SPl> &pls, double max_tol)
{
    std::vector<EndRef> ends;
    for (size_t i = 0; i < pls.size(); ++i) {
        if (!alive(pls[i]) || is_closed_pl(pls[i].pl))
            continue;
        ends.push_back({ i, false });
        ends.push_back({ i, true });
    }
    std::vector<size_t> parent(ends.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto root = [&](size_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    for (size_t a = 0; a < ends.size(); ++a)
        for (size_t b = a + 1; b < ends.size(); ++b) {
            const ThickPolyline &pa = pls[ends[a].pl].pl;
            const ThickPolyline &pb = pls[ends[b].pl].pl;
            // Half a line width at that point (plus a margin), at most half the ceiling.
            const double tol = std::min(0.6 * std::max(end_width(pa, ends[a].back), end_width(pb, ends[b].back))
                                            + 2. * SCALED_EPSILON, max_tol);
            if ((end_point(pa, ends[a].back) - end_point(pb, ends[b].back)).cast<double>().squaredNorm() <= tol * tol)
                parent[root(a)] = root(b);
        }
    Graph g;
    std::map<size_t, size_t> root_to_node;
    for (size_t i = 0; i < ends.size(); ++i) {
        const size_t r = root(i);
        auto it = root_to_node.find(r);
        if (it == root_to_node.end()) {
            it = root_to_node.emplace(r, g.nodes.size()).first;
            g.nodes.emplace_back();
        }
        g.nodes[it->second].push_back(ends[i]);
        g.node_of[{ ends[i].pl, ends[i].back }] = it->second;
    }
    return g;
}

static void kill(SPl &s) { s.pl.points.clear(); s.pl.width.clear(); }

static void clean_graph(std::vector<SPl> &pls, double ceiling_w, SpineStats &s)
{
    const double max_tol = 0.5 * ceiling_w;
    for (int guard = 0; guard < 100000; ++guard) {
        const Graph gr = build_graph(pls, max_tol);

        // (0) Connector inside a junction: a short line with both ends in the same node.
        bool changed = false;
        for (size_t i = 0; i < pls.size() && !changed; ++i) {
            if (!alive(pls[i]) || is_closed_pl(pls[i].pl))
                continue;
            if (gr.node_of.at({ i, false }) == gr.node_of.at({ i, true }) && pls[i].pl.length() <= 2. * max_tol) {
                kill(pls[i]);
                changed = true;
            }
        }
        if (changed)
            continue;

        // (a) Pruning: the shortest twig hanging from a junction with a free other end, shorter than 1.5x the gap
        //     thickness at that junction. One at a time, so all branches of a small Y are not pruned together.
        size_t best_pl    = std::numeric_limits<size_t>::max();
        double best_ratio = 1.5;
        for (size_t n = 0; n < gr.nodes.size(); ++n) {
            const auto &ends = gr.nodes[n];
            if (ends.size() < 3)
                continue;
            const double w = gr.node_width(pls, n);
            if (w <= 0.)
                continue;
            for (const EndRef &e : ends) {
                if (gr.degree_of(e.pl, !e.back) != 1)
                    continue;
                const double ratio = pls[e.pl].pl.length() / w;
                if (ratio < best_ratio) { best_ratio = ratio; best_pl = e.pl; }
            }
        }
        if (best_pl != std::numeric_limits<size_t>::max()) {
            kill(pls[best_pl]);
            ++s.spurs_pruned;
            continue;
        }

        // (b) Pass-through node (degree 2): both halves are the same line.
        for (const auto &ends : gr.nodes) {
            if (ends.size() == 2 && ends[0].pl != ends[1].pl) {
                join_at(pls[ends[0].pl], ends[0].back, pls[ends[1].pl], ends[1].back);
                changed = true;
                break;
            }
        }
        if (changed)
            continue;

        // (c) Junction: chain the two most aligned branches (the ones going straight on). A Y of 3 branches ends
        //     up as 2 lines, like in S3D.
        for (const auto &ends : gr.nodes) {
            if (ends.size() < 3)
                continue;
            double best = 2.;
            size_t bi = 0, bj = 0;
            for (size_t i = 0; i < ends.size(); ++i)
                for (size_t j = i + 1; j < ends.size(); ++j) {
                    if (ends[i].pl == ends[j].pl)
                        continue;
                    const double d = leave_dir(pls[ends[i].pl].pl, ends[i].back).dot(leave_dir(pls[ends[j].pl].pl, ends[j].back));
                    if (d < best) { best = d; bi = i; bj = j; }
                }
            if (best < 2.) {
                join_at(pls[ends[bi].pl], ends[bi].back, pls[ends[bj].pl], ends[bj].back);
                ++s.junction_joins;
                changed = true;
                break;
            }
        }
        if (!changed)
            break;
    }
    pls.erase(std::remove_if(pls.begin(), pls.end(), [](const SPl &x) { return !alive(x); }), pls.end());

    // Closed loops smaller than their own width are a blob, not a line.
    pls.erase(std::remove_if(pls.begin(), pls.end(), [&s](const SPl &x) {
                  if (!is_closed_pl(x.pl) || x.pl.length() >= PI * max_width(x.pl))
                      return false;
                  ++s.tiny_loops_dropped;
                  return true;
              }), pls.end());

    // Free ends according to the final graph (not to MedialAxis): a tip left alone after pruning its twigs must also
    // reach the border; one touching another line must not.
    const Graph gr = build_graph(pls, max_tol);
    for (size_t i = 0; i < pls.size(); ++i) {
        ThickPolyline &pl = pls[i].pl;
        if (is_closed_pl(pl)) {
            pl.endpoints = { false, false };
            continue;
        }
        pl.endpoints.first  = gr.degree_of(i, false) == 1;
        pl.endpoints.second = gr.degree_of(i, true) == 1;
    }
}

// Point of the border of `ex` (contour or hole) reached by the tip `tip` following its direction (`inner` being the
// previous point of the line). Same gesture as ExPolygon::medial_axis, which only tests the contour; here the gap
// may be a ring.
static bool boundary_hit(const ExPolygon &ex, const Point &inner, const Point &tip, double reach, Point &hit)
{
    const Vec2d  a   = inner.cast<double>();
    const Vec2d  b   = tip.cast<double>();
    Vec2d        dir = b - a;
    const double n   = dir.norm();
    if (n < SCALED_EPSILON)
        return false;
    dir /= n;
    const Line ray(tip, Point((b + dir * reach).cast<coord_t>()));

    double best  = std::numeric_limits<double>::max();
    bool   found = false;
    auto test = [&](const Polygon &poly) {
        for (const Line &l : poly.lines()) {
            Point ip;
            if (line_alg::intersection(ray, l, &ip)) {
                const double d = (ip - tip).cast<double>().squaredNorm();
                if (d < best) { best = d; hit = ip; found = true; }
            }
        }
    };
    test(ex.contour);
    for (const Polygon &h : ex.holes)
        test(h);
    return found && hit != tip;
}

ExPolygons run_spine(PerimeterGenerator &g, const ExPolygons &regions, const SpineParams &p, SpineStats *stats,
                     ExPolygons *rejected, const ExPolygons *forced, ExtrusionEntitiesPtr *out)
{
    SpineStats  local;
    SpineStats &s = stats ? *stats : local;
    ExPolygons  consumed;

    const double floor_w   = scaled<double>(std::max(p.floor_mm, 0.001));
    const double ceiling_w = scaled<double>(std::max(p.ceiling_mm, p.floor_mm));
    const double min_len   = scaled<double>(std::max(p.min_length_mm, 0.));
    const double sliver_w  = scaled<double>(std::max(p.sliver_mm, 0.));
    // The skeleton is searched down to very small widths so the line reaches the tip; the floor is applied later
    // to the line width, never as a cut (rule 3).
    const double detect_min = std::max(double(SCALED_EPSILON) * 4., 0.25 * floor_w);
    const double detect_max = ceiling_w * 1.05;
    const double simplify   = scaled<double>(std::max(g.print_config->resolution.value, 0.005));
    const double mm2        = SCALING_FACTOR * SCALING_FACTOR;

    // 1. Skeleton of every zone (simplified; kept to extend the tips to its border).
    ExPolygons       zones;
    std::vector<SPl> pls;
    std::vector<std::pair<const ExPolygon *, bool>> input; // (zone, forced)
    for (const ExPolygon &c : regions)
        input.push_back({ &c, false });
    if (forced)
        for (const ExPolygon &c : *forced)
            input.push_back({ &c, true });
    for (const auto &[comp_ptr, is_forced] : input) {
        const ExPolygon &comp = *comp_ptr;
        ++s.components;
        const double a = comp.area();
        if (a <= 0.)
            continue;
        if (!is_forced && !fits_in_one_line(comp, ceiling_w)) {
            ++s.wide_components;
            s.wide_area_mm2 += a * mm2;
            if (rejected) rejected->push_back(comp);
            continue;
        }
        ExPolygon ex = comp;
        ex.douglas_peucker(simplify);
        ThickPolylines tp;
        // A forced zone may exceed the ceiling: search the whole skeleton (otherwise MedialAxis drops the edges wider
        // than its maximum and the line comes out cut), clip the width to the ceiling afterwards.
        Geometry::MedialAxis ma(detect_min, is_forced ? ceiling_w * 4. : detect_max, ex);
        ma.build(&tp);
        tp.erase(std::remove_if(tp.begin(), tp.end(), [](const ThickPolyline &pl) { return pl.points.size() < 2; }), tp.end());
        if (tp.empty()) {
            // No useful spine (e.g. a tiny square: its skeleton is corners that validate_edge drops). To infill
            // instead of staying empty.
            if (rejected) rejected->push_back(comp);
            continue;
        }
        if (sliver_w > 0.) {
            double wmax = 0.;
            for (const ThickPolyline &pl : tp)
                wmax = std::max(wmax, max_width(pl));
            if (wmax < sliver_w) { // crack: the gap never reaches the threshold
                ++s.slivers_skipped;
                continue;
            }
        }
        const size_t z = zones.size();
        zones.push_back(std::move(ex));
        consumed.push_back(comp);
        ++s.spine_components;
        s.spine_area_mm2 += a * mm2;
        for (ThickPolyline &pl : tp)
            pls.push_back({ std::move(pl), z, z });
    }

    // 2. Cleanup with all zones at once (a pinched gap is still ONE line).
    clean_graph(pls, ceiling_w, s);

    // 3. Tips to the border, floor / ceiling and minimum length. Unlike ExPolygon::medial_axis branches are NOT
    //    pruned by the maximum width found (that would kill the small gaps S3D does print); only the length counts.
    ThickPolylines all;
    for (SPl &sp : pls) {
        ThickPolyline &pl = sp.pl;
        // A crack INSIDE a good zone: the zone rule does not see it because the zone does reach the floor thanks
        // to the thick line. Checked line by line too.
        if (sliver_w > 0. && max_width(pl) < sliver_w) {
            ++s.slivers_skipped;
            continue;
        }
        // Tips to the border with a NEW segment tapering to the floor. Stretching the last segment left the
        // extension at the width of the line end, running where the gap already closes = material over the wall.
        if (pl.endpoints.first) {
            Point tip;
            if (boundary_hit(zones[sp.zf], pl.points[1], pl.points.front(), ceiling_w, tip)) {
                const coordf_t w0 = pl.width.front();
                pl.points.insert(pl.points.begin(), tip);
                pl.width.insert(pl.width.begin(), { coordf_t(floor_w), w0 });
            }
        }
        if (pl.endpoints.second) {
            Point tip;
            if (boundary_hit(zones[sp.zb], pl.points[pl.points.size() - 2], pl.points.back(), ceiling_w, tip)) {
                const coordf_t w1 = pl.width.back();
                pl.points.push_back(tip);
                pl.width.push_back(w1);
                pl.width.push_back(coordf_t(floor_w));
            }
        }
        for (coordf_t &w : pl.width)
            w = std::clamp<coordf_t>(w, floor_w, ceiling_w); // rules 3 and 4
        const double len = pl.length();
        if (len < min_len) {
            ++s.dropped_short;
            continue;
        }
        s.spine_len_mm += unscale<double>(len);
        all.emplace_back(std::move(pl));
    }
    s.polylines += all.size();

    if (!all.empty()) {
        // Same converter as the classic gap fill: the ThickPolyline width is the gap, the volume is gap x height.
        if (out != nullptr) {
            variable_width(all, erGapFill, g.solid_infill_flow, *out);
        } else if (g.gap_fill != nullptr) {
            ExtrusionEntityCollection coll;
            variable_width(all, erGapFill, g.solid_infill_flow, coll.entities);
            g.gap_fill->append(std::move(coll.entities));
        }
    }
    return consumed;
}

}} // namespace Slic3r::NeoArachne
