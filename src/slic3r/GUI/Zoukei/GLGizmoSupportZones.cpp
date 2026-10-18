// Orca Zoukei: Support Zones / Stumps gizmo, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// See GLGizmoSupportZones.hpp for what is and is not ported.
#include "GLGizmoSupportZones.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Triangulation.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Feature/SupportZones/SupportZoneProbe.hpp"

#include <nlohmann/json.hpp>
#include <imgui/imgui.h>
#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace GUI {

namespace {

const ColorRGBA kHoverCol     (0.10f, 0.95f, 0.85f, 0.88f); // about to take it, past the threshold
const ColorRGBA kHoverBelowCol(0.72f, 0.48f, 1.00f, 0.88f); // about to take it, under the threshold
const ColorRGBA kTargetCol    (0.16f, 0.90f, 0.32f, 0.82f); // taken
const ColorRGBA kPreviewCol   (0.30f, 0.55f, 0.95f, 0.45f); // the solid that will be created
const ColorRGBA kMarkCol      (0.95f, 0.60f, 0.15f, 0.90f); // landing / stumps

// Only surfaces looking DOWNWARD can be picked (about 6 degrees off vertical). This picks a destination; whether
// support is needed there is still decided by the slicer.
constexpr float  DOWN_FACING_MAX_NORMAL_Z = -0.10f;
constexpr double PI_D                     = 3.14159265358979323846;
constexpr double ROOF_LIFT_MM             = 0.5;

Polygon zone_disc(const Vec2d &c, double r)
{
    Points pts;
    const int seg = 32;
    pts.reserve(seg);
    for (int i = 0; i < seg; ++ i) {
        const double a = 2.0 * PI_D * double(i) / double(seg);
        pts.emplace_back(scaled<coord_t>(c.x() + r * std::cos(a)), scaled<coord_t>(c.y() + r * std::sin(a)));
    }
    return Polygon(std::move(pts));
}

// Extrudes an ExPolygons into a straight prism between z_bot and z_top (world coordinates). One triangulation serves
// both caps; walls are wound so the solid is closed and outward facing (holes included).
void zone_extrude_prism(const ExPolygons &area, double z_bot, double z_top, indexed_triangle_set &its)
{
    if (area.empty() || z_top <= z_bot)
        return;
    const Points pts = to_points(area);
    if (pts.size() < 3)
        return;
    const std::vector<Vec3i32> cap = Triangulation::triangulate(area);
    if (cap.empty())
        return;
    const int n  = int(pts.size());
    const int up = int(its.vertices.size());
    const int dn = up + n;
    its.vertices.reserve(its.vertices.size() + size_t(2 * n));
    for (const Point &p : pts)
        its.vertices.emplace_back(float(unscaled<double>(p.x())), float(unscaled<double>(p.y())), float(z_top));
    for (const Point &p : pts)
        its.vertices.emplace_back(float(unscaled<double>(p.x())), float(unscaled<double>(p.y())), float(z_bot));
    for (const Vec3i32 &t : cap) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= n || t[1] >= n || t[2] >= n)
            continue;
        its.indices.emplace_back(up + t[0], up + t[1], up + t[2]);
        its.indices.emplace_back(dn + t[0], dn + t[2], dn + t[1]);
    }
    int off = 0;
    auto wall = [&](const Polygon &ring) {
        const int m = int(ring.points.size());
        for (int i = 0; i < m; ++ i) {
            const int a = off + i, b = off + (i + 1) % m;
            its.indices.emplace_back(up + a, dn + b, up + b);
            its.indices.emplace_back(up + a, dn + a, dn + b);
        }
        off += m;
    };
    for (const ExPolygon &ep : area) {
        wall(ep.contour);
        for (const Polygon &h : ep.holes)
            wall(h);
    }
}

Polygons world_triangles_xy(const indexed_triangle_set &its, const std::vector<int> &faces, const Transform3d &trafo)
{
    Polygons tris;
    tris.reserve(faces.size());
    for (int f : faces) {
        if (f < 0 || f >= int(its.indices.size()))
            continue;
        const Vec3i32 t = its.indices[f];
        Points p;
        p.reserve(3);
        for (int k = 0; k < 3; ++ k) {
            const Vec3d w = trafo * its.vertices[t[k]].cast<double>();
            p.emplace_back(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
        }
        Polygon q(std::move(p));
        if (q.area() == 0.)
            continue; // edge-on: a line in XY
        if (! q.is_counter_clockwise())
            q.reverse();
        tris.emplace_back(std::move(q));
    }
    return tris;
}

void render_world_model(GLModel &model, const Transform3d &model_matrix, bool xray)
{
    if (! model.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    if (xray) {
        // The marked surface is an underside, usually behind a wall of the part: a faint pass without depth test
        // says "it is there", the depth tested pass shows where it is really visible.
        glsafe(::glDisable(GL_DEPTH_TEST));
        model.render();
        glsafe(::glEnable(GL_DEPTH_TEST));
    } else
        glsafe(::glEnable(GL_DEPTH_TEST));
    model.render();
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

} // namespace

GLGizmoSupportZones::GLGizmoSupportZones(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoSupportZones::on_init()
{
    return true;
}

std::string GLGizmoSupportZones::on_get_name() const
{
    return _u8L("Support Zones");
}

bool GLGizmoSupportZones::on_is_activable() const
{
    // Every zone is a volume of one object.
    return m_parent.get_selection().get_object_idx() >= 0;
}

CommonGizmosDataID GLGizmoSupportZones::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo));
}

void GLGizmoSupportZones::on_set_state()
{
    if (get_state() == On) {
        m_zones_dirty = true;
        if (! m_threshold_seeded) {
            const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            if (cfg.has("support_threshold_angle") && cfg.opt_int("support_threshold_angle") > 0)
                m_overhang_threshold_deg = float(cfg.opt_int("support_threshold_angle"));
            m_threshold_seeded = true;
        }
        // The corridor needs Snug (with Grid / Default the column steps a grid cell at a time). Written to the object
        // config with a snapshot, outside the state change.
        wxGetApp().CallAfter([this]() {
            if (get_state() == On)
                ensure_snug_style();
        });
    } else if (get_state() == Off) {
        m_parent.enable_moving(true);
        if (m_parent.is_using_slope())
            m_parent.use_slope(false);
        m_threshold_seeded = false;
        m_cut_mode = false;
        m_cut_points.clear();
        clear_pick();
        m_forced_snug = false;
        m_zones.clear();
    }
}

void GLGizmoSupportZones::data_changed(bool /*is_serializing*/)
{
    // Selection changes and volume rebuilds: the raycaster is rebuilt lazily, and ensure_raycaster() drops the picks
    // only if the mesh really changed (they are facet indices).
    m_zones_dirty       = true;
    m_raycaster.reset();
    m_raycaster_obj_idx = -1;
    m_hover_model_facet = m_target_model_facet = -1;
    m_candidates.clear();
    m_cand_mouse = Vec2d(-1e9, -1e9);
    invalidate_patch();
    m_preview_dirty = true;
}

// -----------------------------------------------------------------------------------------------------------------
// Object and raycaster
// -----------------------------------------------------------------------------------------------------------------

int GLGizmoSupportZones::current_object_idx() const
{
    return m_parent.get_selection().get_object_idx();
}

const ModelObject *GLGizmoSupportZones::current_object() const
{
    const Model *model = m_parent.get_selection().get_model();
    const int    idx   = current_object_idx();
    if (model == nullptr || idx < 0 || idx >= int(model->objects.size()))
        return nullptr;
    return model->objects[idx];
}

void GLGizmoSupportZones::ensure_raycaster()
{
    const ModelObject *mo  = current_object();
    const int          idx = current_object_idx();
    if (mo == nullptr || mo->instances.empty()) {
        m_raycaster.reset();
        m_raycaster_obj_idx = -1;
        return;
    }
    m_world_trafo = mo->instances.front()->get_matrix();
    if (m_raycaster && m_raycaster_obj_idx == idx)
        return;
    TriangleMesh new_mesh = mo->raw_mesh();
    // The picks and the paint are facet indices: they survive a scene refresh only if the mesh is the same one.
    const bool same_mesh = idx == m_mesh_obj_idx && new_mesh.its.indices.size() == m_mesh.its.indices.size() &&
                           new_mesh.its.vertices.size() == m_mesh.its.vertices.size() &&
                           (new_mesh.its.vertices.empty() || new_mesh.its.vertices.front() == m_mesh.its.vertices.front());
    if (! same_mesh) {
        clear_pick();
        m_painted.clear();
        m_painted_list.clear();
        m_visit_stamp.clear();
    }
    m_mesh = std::move(new_mesh);
    m_mesh_obj_idx = idx;
    m_raycaster.reset(new MeshRaycaster(m_mesh));
    m_raycaster_obj_idx = idx;
    m_face_normals      = its_face_normals(m_mesh.its);
    m_face_neighbors    = its_face_neighbors(m_mesh.its);
    m_region_cache_facet = -1;
    m_region_cache.clear();
    m_region_area_cache.clear();
    ++ m_stamp_stamp;
    m_hover_model_facet = m_target_model_facet = -1;
    m_candidates.clear();
}

void GLGizmoSupportZones::clear_pick()
{
    m_target_pick_mode  = false;
    m_landing_pick_mode = false;
    m_stump_pick_mode   = false;
    m_has_target        = false;
    m_has_landing       = false;
    m_landing_locked    = false;
    m_mouse_down        = false;
    m_painting          = false;
    m_extra_stumps.clear();
    m_stamps.clear();
    clear_painted();
    invalidate_patch();
    m_preview_dirty = true;
    m_preview_model.reset();
    m_marks_model.reset();
    m_target_facet_idx   = -1;
    m_target_model_facet = -1;
    m_hover_model_facet  = -1;
    m_have_hover_pos     = false;
    m_candidate_idx      = 0;
    m_candidates.clear();
    m_hover_model.reset();
    m_target_model.reset();
}

float GLGizmoSupportZones::overhang_normal_z_cut() const
{
    return float(- std::cos(Geometry::deg2rad(double(m_overhang_threshold_deg))));
}

// -----------------------------------------------------------------------------------------------------------------
// Picking
// -----------------------------------------------------------------------------------------------------------------

void GLGizmoSupportZones::update_candidates(const Vec2d &mouse_pos)
{
    ensure_raycaster();
    // A still mouse costs nothing.
    if ((mouse_pos - m_cand_mouse).squaredNorm() < 0.25 && m_cand_trafo.isApprox(m_world_trafo) && ! m_candidates.empty())
        return;
    m_cand_mouse = mouse_pos;
    m_cand_trafo = m_world_trafo;

    const int previous_facet = (m_candidate_idx >= 0 && m_candidate_idx < int(m_candidates.size())) ?
                                   m_candidates[m_candidate_idx].facet_idx : -1;
    m_candidates.clear();
    if (! m_raycaster)
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3d world_src, world_dir;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, world_src, world_dir);
    const Transform3d inv       = m_world_trafo.inverse();
    const Vec3d       local_src = inv * world_src;
    const Vec3d       local_dir = inv.linear() * world_dir;
    const std::vector<AABBMesh::hit_result> hits = m_raycaster->get_aabb_mesh().query_ray_hits(local_src, local_dir);

    // Paint what is SEEN: only the surfaces at the depth of the first hit, never the inside face of a wall behind it.
    double depth_limit = std::numeric_limits<double>::max();
    for (const AABBMesh::hit_result &h : hits)
        if (h.is_hit()) {
            depth_limit = h.distance() + 1e-4;
            break;
        }

    const int      n_facets = int(m_mesh.its.indices.size());
    const Matrix3d nrm      = m_world_trafo.linear().inverse().transpose();
    for (const AABBMesh::hit_result &h : hits) {
        if (! h.is_hit())
            continue;
        if (h.distance() > depth_limit)
            break;
        const int f = h.face();
        if (f < 0 || f >= n_facets || f >= int(m_face_normals.size()))
            continue;
        Vec3d world_n = nrm * m_face_normals[f].cast<double>();
        const double nl = world_n.norm();
        if (nl < 1e-9)
            continue;
        world_n /= nl;
        if (world_n.z() > double(DOWN_FACING_MAX_NORMAL_Z))
            continue;
        Candidate c;
        c.facet_idx      = f;
        c.distance       = h.distance();
        c.world_pos      = m_world_trafo * h.position();
        c.world_normal   = world_n;
        c.past_threshold = world_n.z() <= double(overhang_normal_z_cut());
        m_candidates.push_back(c);
    }
    std::sort(m_candidates.begin(), m_candidates.end(), [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });
    m_candidate_idx = 0;
    if (previous_facet >= 0)
        for (int i = 0; i < int(m_candidates.size()); ++ i)
            if (m_candidates[i].facet_idx == previous_facet) {
                m_candidate_idx = i;
                break;
            }
}

const GLGizmoSupportZones::Candidate *GLGizmoSupportZones::live_candidate() const
{
    if (m_candidate_idx < 0 || m_candidate_idx >= int(m_candidates.size()))
        return nullptr;
    return &m_candidates[m_candidate_idx];
}

// Connected region of facets within ~20 degrees of the seed normal (absolute value: raw meshes may have mixed winding).
std::vector<int> GLGizmoSupportZones::collect_region(int facet_idx) const
{
    std::vector<int> region;
    const int n_facets = int(m_mesh.its.indices.size());
    if (facet_idx < 0 || facet_idx >= n_facets || int(m_face_normals.size()) != n_facets || int(m_face_neighbors.size()) != n_facets)
        return region;
    const float  cos_thresh = 0.94f;
    const size_t max_facets = 40000;
    const Vec3f  seed_n     = m_face_normals[facet_idx].normalized();
    std::vector<char> visited(n_facets, 0);
    std::vector<int>  stack { facet_idx };
    region.reserve(256);
    visited[facet_idx] = 1;
    while (! stack.empty() && region.size() < max_facets) {
        const int f = stack.back();
        stack.pop_back();
        region.push_back(f);
        for (int e = 0; e < 3; ++ e) {
            const int nb = m_face_neighbors[f][e];
            if (nb < 0 || nb >= n_facets || visited[nb])
                continue;
            if (std::abs(m_face_normals[nb].normalized().dot(seed_n)) >= cos_thresh) {
                visited[nb] = 1;
                stack.push_back(nb);
            }
        }
    }
    return region;
}

const std::vector<int> &GLGizmoSupportZones::region_for(int seed_facet) const
{
    if (m_region_cache_facet == seed_facet && m_region_cache_trafo.isApprox(m_world_trafo))
        return m_region_cache;
    m_region_cache       = collect_region(seed_facet);
    m_region_cache_facet = seed_facet;
    m_region_cache_trafo = m_world_trafo;
    m_region_area_cache.clear();
    if (! m_region_cache.empty()) {
        const Polygons tris = world_triangles_xy(m_mesh.its, m_region_cache, m_world_trafo);
        // The micro offset closes the integer seams that rounding leaves between neighbouring triangles.
        if (! tris.empty())
            m_region_area_cache = union_ex(offset(tris, scaled<float>(0.002f)));
    }
    return m_region_cache;
}

// Where a pillar lands for this cursor position: an upward shelf of the part below the target, else the bed.
bool GLGizmoSupportZones::resolve_landing(const Vec2d &mouse_pos, Vec3d &out_pos, bool &out_on_bed)
{
    ensure_raycaster();
    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3d world_src, world_dir;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, world_src, world_dir);
    if (m_raycaster) {
        const Transform3d inv  = m_world_trafo.inverse();
        const Matrix3d    nrm  = m_world_trafo.linear().inverse().transpose();
        const std::vector<AABBMesh::hit_result> hits = m_raycaster->get_aabb_mesh().query_ray_hits(inv * world_src, inv.linear() * world_dir);
        const int n_facets  = int(m_mesh.its.indices.size());
        double    best_dist = std::numeric_limits<double>::max();
        bool      found     = false;
        Vec3d     best_pos { Vec3d::Zero() };
        for (const AABBMesh::hit_result &h : hits) {
            if (! h.is_hit())
                continue;
            const int f = h.face();
            if (f < 0 || f >= n_facets || f >= int(m_face_normals.size()))
                continue;
            Vec3d wn = nrm * m_face_normals[f].cast<double>();
            const double nl = wn.norm();
            if (nl < 1e-9)
                continue;
            wn /= nl;
            if (wn.z() < - double(DOWN_FACING_MAX_NORMAL_Z))
                continue; // not facing up
            const Vec3d wp = m_world_trafo * h.position();
            if (m_has_target && wp.z() >= m_target_world_pos.z() - 0.2)
                continue; // not below the surface held up
            if (h.distance() < best_dist) {
                best_dist = h.distance();
                best_pos  = wp;
                found     = true;
            }
        }
        if (found) {
            out_pos    = best_pos;
            out_on_bed = false;
            return true;
        }
    }
    if (std::abs(world_dir.z()) < 1e-9)
        return false;
    const double t = - world_src.z() / world_dir.z();
    if (t <= 0.)
        return false;
    out_pos     = world_src + t * world_dir;
    out_pos.z() = 0.;
    out_on_bed  = true;
    return true;
}

// The landing straight below the middle of the footprint: a shelf of the part if there is one, else the bed.
bool GLGizmoSupportZones::resolve_landing_plumb(Vec3d &out_pos, bool &out_on_bed)
{
    ensure_raycaster();
    if (! m_has_target)
        return false;
    const Vec2d c   = footprint_centroid();
    const Vec3d src(c.x(), c.y(), m_target_world_pos.z());
    const Vec3d dir(0., 0., -1.);
    if (m_raycaster) {
        const Transform3d inv = m_world_trafo.inverse();
        const Matrix3d    nrm = m_world_trafo.linear().inverse().transpose();
        const std::vector<AABBMesh::hit_result> hits = m_raycaster->get_aabb_mesh().query_ray_hits(inv * src, inv.linear() * dir);
        const int n_facets  = int(m_mesh.its.indices.size());
        double    best_dist = std::numeric_limits<double>::max();
        bool      found     = false;
        Vec3d     best_pos { Vec3d::Zero() };
        for (const AABBMesh::hit_result &h : hits) {
            if (! h.is_hit())
                continue;
            const int f = h.face();
            if (f < 0 || f >= n_facets || f >= int(m_face_normals.size()))
                continue;
            Vec3d wn = nrm * m_face_normals[f].cast<double>();
            const double nl = wn.norm();
            if (nl < 1e-9)
                continue;
            wn /= nl;
            if (wn.z() < - double(DOWN_FACING_MAX_NORMAL_Z))
                continue;
            const Vec3d wp = m_world_trafo * h.position();
            if (wp.z() >= m_target_world_pos.z() - 0.2)
                continue;
            if (h.distance() < best_dist) {
                best_dist = h.distance();
                best_pos  = wp;
                found     = true;
            }
        }
        if (found) {
            out_pos    = best_pos;
            out_on_bed = false;
            return true;
        }
    }
    out_pos    = Vec3d(c.x(), c.y(), 0.);
    out_on_bed = true;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------
// The footprint mask and its roof
// -----------------------------------------------------------------------------------------------------------------

void GLGizmoSupportZones::ensure_heightmap(ZoneMask &mk, const BoundingBox &area_bb, double z_ref, int max_cells) const
{
    mk.hm_nx = mk.hm_ny = 0;
    mk.hm_z.clear();
    mk.hm_n.clear();
    if (! m_raycaster)
        return;
    constexpr double MARGIN = 1.0;
    const double x0 = unscaled<double>(double(area_bb.min.x())) - MARGIN;
    const double y0 = unscaled<double>(double(area_bb.min.y())) - MARGIN;
    const double x1 = unscaled<double>(double(area_bb.max.x())) + MARGIN;
    const double y1 = unscaled<double>(double(area_bb.max.y())) + MARGIN;
    double step = 0.5;
    int nx = std::max(1, int(std::ceil((x1 - x0) / step)));
    int ny = std::max(1, int(std::ceil((y1 - y0) / step)));
    while (nx * ny > max_cells) {
        step *= 1.5;
        nx = std::max(1, int(std::ceil((x1 - x0) / step)));
        ny = std::max(1, int(std::ceil((y1 - y0) / step)));
    }
    mk.hm_origin = Vec2d(x0, y0);
    mk.hm_step   = step;
    mk.hm_nx     = nx;
    mk.hm_ny     = ny;
    const int w = nx + 1;
    mk.hm_z.assign(size_t(w) * size_t(ny + 1), std::numeric_limits<float>::quiet_NaN());
    mk.hm_n.assign(size_t(w) * size_t(ny + 1), Vec3f::Zero());

    const Transform3d inv  = m_world_trafo.inverse();
    const AABBMesh   &tree = m_raycaster->get_aabb_mesh();
    const Matrix3d    nrm  = m_world_trafo.linear().inverse().transpose();
    const Vec3d       ldir = inv.linear() * Vec3d(0., 0., -1.);
    for (int j = 0; j <= ny; ++ j)
        for (int i = 0; i <= nx; ++ i) {
            const Vec3d src_w { x0 + step * i, y0 + step * j, z_ref + 1000. };
            const std::vector<AABBMesh::hit_result> hits = tree.query_ray_hits(inv * src_w, ldir);
            double best  = 0.;
            Vec3f  best_n { Vec3f::Zero() };
            bool   found = false;
            for (const AABBMesh::hit_result &h : hits) {
                if (! h.is_hit())
                    continue;
                const int f = h.face();
                if (f < 0 || f >= int(m_face_normals.size()))
                    continue;
                Vec3d wn = nrm * m_face_normals[f].cast<double>();
                if (wn.norm() < 1e-12)
                    continue;
                wn.normalize();
                // The height of the part over this point, whatever its normal: the roof has to touch it. The one
                // closest in z to the picked face wins (a part with several floors has several).
                const double zw = (m_world_trafo * h.position()).z();
                if (! found || std::abs(zw - z_ref) < std::abs(best - z_ref)) {
                    best   = zw;
                    best_n = wn.cast<float>();
                    found  = true;
                }
            }
            if (found) {
                const size_t k = size_t(j) * size_t(w) + size_t(i);
                mk.hm_z[k] = float(best);
                mk.hm_n[k] = best_n;
            }
        }
}

double GLGizmoSupportZones::z_at(const ZoneMask &mk, const Vec2d &p)
{
    auto on_plane = [&mk, &p]() {
        if (std::abs(mk.seed_n.z()) < 1e-6)
            return mk.seed_p.z();
        return (mk.seed_n.dot(mk.seed_p) - mk.seed_n.x() * p.x() - mk.seed_n.y() * p.y()) / mk.seed_n.z();
    };
    if (mk.hm_nx < 1 || mk.hm_ny < 1 || mk.hm_step <= 0.)
        return on_plane();
    const double fx = (p.x() - mk.hm_origin.x()) / mk.hm_step;
    const double fy = (p.y() - mk.hm_origin.y()) / mk.hm_step;
    const int    i0 = int(std::floor(fx));
    const int    j0 = int(std::floor(fy));
    if (i0 < 0 || j0 < 0 || i0 + 1 > mk.hm_nx || j0 + 1 > mk.hm_ny)
        return on_plane();
    const double tx = fx - double(i0);
    const double ty = fy - double(j0);
    const int    w  = mk.hm_nx + 1;
    const float  z00 = mk.hm_z[size_t(j0) * size_t(w) + size_t(i0)];
    const float  z10 = mk.hm_z[size_t(j0) * size_t(w) + size_t(i0 + 1)];
    const float  z01 = mk.hm_z[size_t(j0 + 1) * size_t(w) + size_t(i0)];
    const float  z11 = mk.hm_z[size_t(j0 + 1) * size_t(w) + size_t(i0 + 1)];
    // One node without surface invalidates the cell: the plane of the seed face is the honest answer there.
    if (std::isnan(z00) || std::isnan(z10) || std::isnan(z01) || std::isnan(z11))
        return on_plane();
    const double a = double(z00) + (double(z10) - double(z00)) * tx;
    const double b = double(z01) + (double(z11) - double(z01)) * tx;
    return a + (b - a) * ty;
}

Vec3d GLGizmoSupportZones::n_at(const ZoneMask &mk, const Vec2d &p)
{
    if (mk.hm_nx < 1 || mk.hm_ny < 1 || mk.hm_step <= 0.)
        return mk.seed_n;
    const int i = int(std::lround((p.x() - mk.hm_origin.x()) / mk.hm_step));
    const int j = int(std::lround((p.y() - mk.hm_origin.y()) / mk.hm_step));
    if (i < 0 || j < 0 || i > mk.hm_nx || j > mk.hm_ny)
        return mk.seed_n;
    const Vec3f &n = mk.hm_n[size_t(j) * size_t(mk.hm_nx + 1) + size_t(i)];
    if (n.squaredNorm() < 1e-12f)
        return mk.seed_n;
    return n.cast<double>();
}

bool GLGizmoSupportZones::build_mask(int seed_facet, const Vec2d &centre_xy, ZoneMask &out, int max_cells) const
{
    out = ZoneMask();
    const int n_facets = int(m_mesh.its.indices.size());
    if (seed_facet < 0 || seed_facet >= n_facets || int(m_face_normals.size()) != n_facets)
        return false;
    {
        const Vec3i32 t0 = m_mesh.its.indices[seed_facet];
        out.seed_p = (m_world_trafo * m_mesh.its.vertices[t0[0]].cast<double>() +
                      m_world_trafo * m_mesh.its.vertices[t0[1]].cast<double>() +
                      m_world_trafo * m_mesh.its.vertices[t0[2]].cast<double>()) / 3.;
        Vec3d n = m_world_trafo.linear().inverse().transpose() * m_face_normals[seed_facet].cast<double>();
        if (n.norm() > 1e-12) {
            n.normalize();
            if (n.z() > 0.)
                n = - n;
            out.seed_n = n;
        }
    }

    if (m_foot_shape == FootShape::Brush) {
        // What is painted IS the footprint. Its height range comes from the painted triangles themselves.
        out.area = painted_area_world();
        if (out.area.empty())
            return false;
        out.z_low  = std::numeric_limits<double>::max();
        out.z_high = std::numeric_limits<double>::lowest();
        for (int f : m_painted_list) {
            if (f < 0 || f >= n_facets)
                continue;
            const Vec3i32 t = m_mesh.its.indices[f];
            for (int k = 0; k < 3; ++ k) {
                const double z = (m_world_trafo * m_mesh.its.vertices[t[k]].cast<double>()).z();
                out.z_low  = std::min(out.z_low, z);
                out.z_high = std::max(out.z_high, z);
            }
        }
        if (out.z_low > out.z_high)
            out.z_low = out.z_high = out.seed_p.z();
        out.ok = true;
        return true;
    }

    ExPolygons shape;
    bool       needs_region = true;
    if (m_foot_shape == FootShape::Round && m_foot_size_mm > 0.f) {
        shape.emplace_back(zone_disc(centre_xy, 0.5 * double(m_foot_size_mm)));
        needs_region = ! m_shape_covers;
    } else if (m_foot_shape == FootShape::Square && m_foot_size_mm > 0.f) {
        const double h = 0.5 * double(m_foot_size_mm);
        Points pts { { scaled<coord_t>(centre_xy.x() - h), scaled<coord_t>(centre_xy.y() - h) },
                     { scaled<coord_t>(centre_xy.x() + h), scaled<coord_t>(centre_xy.y() - h) },
                     { scaled<coord_t>(centre_xy.x() + h), scaled<coord_t>(centre_xy.y() + h) },
                     { scaled<coord_t>(centre_xy.x() - h), scaled<coord_t>(centre_xy.y() + h) } };
        shape.emplace_back(Polygon(std::move(pts)));
        needs_region = ! m_shape_covers;
    }
    if (needs_region) {
        // Cut: the shape is trimmed to the picked patch, it can never be bigger than the surface.
        const std::vector<int> &region = region_for(seed_facet);
        if (region.empty() || m_region_area_cache.empty())
            return false;
        out.area = shape.empty() ? m_region_area_cache : intersection_ex(m_region_area_cache, shape);
    } else {
        // Cover: the shape IS the section; the roof still follows the surface where there is one.
        if (shape.empty())
            return false;
        out.area = shape;
    }
    if (out.area.empty())
        return false;

    const BoundingBox abb = get_extents(out.area);
    ensure_heightmap(out, abb, out.seed_p.z(), max_cells);

    out.z_low  = std::numeric_limits<double>::max();
    out.z_high = std::numeric_limits<double>::lowest();
    if (out.hm_nx > 0 && out.hm_ny > 0)
        for (int j = 0; j <= out.hm_ny; ++ j)
            for (int i = 0; i <= out.hm_nx; ++ i) {
                const float z = out.hm_z[size_t(j) * size_t(out.hm_nx + 1) + size_t(i)];
                if (std::isnan(z))
                    continue;
                const Point q(scaled<coord_t>(out.hm_origin.x() + out.hm_step * i), scaled<coord_t>(out.hm_origin.y() + out.hm_step * j));
                bool inside = false;
                for (const ExPolygon &ep : out.area)
                    if (ep.contains(q)) {
                        inside = true;
                        break;
                    }
                if (! inside)
                    continue;
                out.z_low  = std::min(out.z_low, double(z));
                out.z_high = std::max(out.z_high, double(z));
            }
    for (const ExPolygon &ep : out.area)
        for (const Point &q : ep.contour.points) {
            const double z = z_at(out, Vec2d(unscaled<double>(q.x()), unscaled<double>(q.y())));
            out.z_low  = std::min(out.z_low, z);
            out.z_high = std::max(out.z_high, z);
        }
    if (out.z_low > out.z_high)
        out.z_low = out.z_high = out.seed_p.z();
    out.ok = true;
    return true;
}

void GLGizmoSupportZones::invalidate_patch()
{
    m_mask_cache_facet   = -1;
    m_foot_cache_version = size_t(-1);
}

const GLGizmoSupportZones::ZoneMask *GLGizmoSupportZones::mask() const
{
    if (! m_has_target)
        return nullptr;
    int   facet_idx = m_target_facet_idx;
    Vec2d centre_xy(m_target_world_pos.x(), m_target_world_pos.y());
    if (block_tree_mode() && ! m_stamps.empty())
        centre_xy = Vec2d(m_stamps.front().p.x(), m_stamps.front().p.y());
    const int shape_key = int(m_foot_shape) + (m_shape_covers ? 100 : 0);
    if (m_mask_cache_facet == facet_idx && m_mask_cache_shape == shape_key && m_mask_cache_size == m_foot_size_mm &&
        m_mask_cache_centre == centre_xy && m_mask_cache_stamp == m_stamp_stamp && m_mask_cache_trafo.isApprox(m_world_trafo))
        return m_mask_cache.ok ? &m_mask_cache : nullptr;
    build_mask(facet_idx, centre_xy, m_mask_cache, 90000);
    m_mask_cache_facet  = facet_idx;
    m_mask_cache_shape  = shape_key;
    m_mask_cache_size   = m_foot_size_mm;
    m_mask_cache_centre = centre_xy;
    m_mask_cache_stamp  = m_stamp_stamp;
    m_mask_cache_trafo  = m_world_trafo;
    ++ m_mask_version;
    return m_mask_cache.ok ? &m_mask_cache : nullptr;
}

// Resamples the largest island of an area to n points by arc length, starting at the +X direction from the centroid,
// so rings computed by separate offsets can be stitched. Returns empty instead of a ring with repeated points.
std::vector<Vec2d> GLGizmoSupportZones::resample_ring(const ExPolygons &area, int n_pts)
{
    std::vector<Vec2d> out;
    if (area.empty() || n_pts < 3)
        return out;
    size_t best = 0;
    for (size_t i = 1; i < area.size(); ++ i)
        if (std::abs(area[i].contour.area()) > std::abs(area[best].contour.area()))
            best = i;
    Polygon c = area[best].contour;
    if (! c.is_counter_clockwise())
        c.reverse();
    c.douglas_peucker(scaled<double>(0.02));
    if (c.points.size() < 3)
        return out;
    std::vector<Vec2d> pts;
    pts.reserve(c.points.size());
    for (const Point &p : c.points)
        pts.emplace_back(unscaled<double>(p.x()), unscaled<double>(p.y()));
    Vec2d ctr { 0., 0. };
    for (const Vec2d &p : pts)
        ctr += p;
    ctr /= double(pts.size());
    size_t start  = 0;
    double best_a = 1e9;
    for (size_t i = 0; i < pts.size(); ++ i) {
        const Vec2d  d = pts[i] - ctr;
        const double a = std::abs(std::atan2(d.y(), d.x()));
        if (a < best_a) {
            best_a = a;
            start  = i;
        }
    }
    std::vector<double> seg(pts.size(), 0.);
    double total = 0.;
    for (size_t i = 0; i < pts.size(); ++ i) {
        const Vec2d &a = pts[(start + i) % pts.size()];
        const Vec2d &b = pts[(start + i + 1) % pts.size()];
        seg[i] = (b - a).norm();
        total += seg[i];
    }
    if (total < 1e-9)
        return out;
    out.reserve(n_pts);
    size_t si  = 0;
    double acc = 0.;
    for (int k = 0; k < n_pts; ++ k) {
        const double want = total * double(k) / double(n_pts);
        while (si + 1 < seg.size() && acc + seg[si] < want) {
            acc += seg[si];
            ++ si;
        }
        const Vec2d &a = pts[(start + si) % pts.size()];
        const Vec2d &b = pts[(start + si + 1) % pts.size()];
        const double t = (seg[si] > 1e-12) ? std::clamp((want - acc) / seg[si], 0., 1.) : 0.;
        out.push_back(a + (b - a) * t);
    }
    for (size_t i = 0; i < out.size(); ++ i)
        if ((out[(i + 1) % out.size()] - out[i]).squaredNorm() < 1e-12)
            return std::vector<Vec2d>();
    return out;
}

std::vector<Vec2d> GLGizmoSupportZones::ring_outline_world(double edge_mm) const
{
    std::vector<Vec2d> out;
    const ZoneMask *mk = mask();
    if (mk == nullptr)
        return out;
    const ExPolygons ring = (std::abs(edge_mm) < 1e-6) ? mk->area : offset_ex(mk->area, scaled<float>(- edge_mm), jtRound);
    if (ring.empty())
        return out;
    size_t best = 0;
    for (size_t i = 1; i < ring.size(); ++ i)
        if (std::abs(ring[i].contour.area()) > std::abs(ring[best].contour.area()))
            best = i;
    Polygon outline = ring[best].contour;
    outline.douglas_peucker(scaled<double>(0.05));
    if (outline.points.size() < 3)
        return out;
    if (! outline.is_counter_clockwise())
        outline.reverse();
    out.reserve(outline.points.size());
    for (const Point &p : outline.points)
        out.emplace_back(unscaled<double>(p.x()), unscaled<double>(p.y()));
    return out;
}

std::vector<Vec2d> GLGizmoSupportZones::target_footprint_world() const
{
    if (! m_has_target)
        return {};
    // Touch the mask first: it bumps m_mask_version when it is rebuilt.
    if (mask() == nullptr)
        return {};
    if (m_foot_cache_version == m_mask_version && m_foot_cache_shrink == m_footprint_shrink_mm)
        return m_foot_cache;
    m_foot_cache         = ring_outline_world(double(m_footprint_shrink_mm));
    m_foot_cache_version = m_mask_version;
    m_foot_cache_shrink  = m_footprint_shrink_mm;
    return m_foot_cache;
}

Vec2d GLGizmoSupportZones::footprint_centroid() const
{
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.size() < 3)
        return Vec2d(m_target_world_pos.x(), m_target_world_pos.y());
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    return c / double(foot.size());
}

double GLGizmoSupportZones::max_inset_mm() const
{
    const ZoneMask *mk = mask();
    if (mk == nullptr || mk->area.empty())
        return 0.;
    const BoundingBox bb = get_extents(mk->area);
    return std::max(0., 0.5 * unscaled<double>(double(std::min(bb.size().x(), bb.size().y()))));
}

double GLGizmoSupportZones::max_outset_mm() const
{
    const ZoneMask *mk = mask();
    if (mk == nullptr || mk->area.empty())
        return 0.;
    const BoundingBox bb = get_extents(mk->area);
    return std::max(50.0, unscaled<double>(double(std::max(bb.size().x(), bb.size().y()))));
}

// -----------------------------------------------------------------------------------------------------------------
// The brush: it marks TRIANGLES, walking only through neighbours, so it cannot jump across a hole
// -----------------------------------------------------------------------------------------------------------------

size_t GLGizmoSupportZones::paint_facets(int seed, const Vec3d &hit_world, double radius, bool erase)
{
    const int n_facets = int(m_mesh.its.indices.size());
    if (seed < 0 || seed >= n_facets || int(m_face_neighbors.size()) != n_facets)
        return 0;
    if (int(m_painted.size()) != n_facets) {
        m_painted.assign(n_facets, 0);
        m_painted_list.clear();
    }
    if (int(m_visit_stamp.size()) != n_facets) {
        m_visit_stamp.assign(n_facets, 0);
        m_visit_epoch = 0;
    }
    if (++ m_visit_epoch == 0) {
        std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
        m_visit_epoch = 1;
    }
    const char   want = erase ? 0 : 1;
    const double r2   = radius * radius;
    // The return type is written on purpose: with auto, Eigen would return an expression referencing temporaries.
    auto centroid = [this](int f) -> Vec3d {
        const Vec3i32 t = m_mesh.its.indices[f];
        return (m_world_trafo * m_mesh.its.vertices[t[0]].cast<double>() + m_world_trafo * m_mesh.its.vertices[t[1]].cast<double>() +
                m_world_trafo * m_mesh.its.vertices[t[2]].cast<double>()) / 3.;
    };
    // "Only overhanging faces": walls are still walked through (they link one underside to the next), just not marked.
    const bool     filter  = ! erase && m_paint_overhang_only;
    const double   cut     = - std::cos(Geometry::deg2rad(double(m_paint_cut_deg)));
    const Matrix3d nrm_mat = m_world_trafo.linear().inverse().transpose();
    auto leans_enough = [this, &nrm_mat, cut](int f) -> bool {
        if (f >= int(m_face_normals.size()))
            return false;
        const Vec3d  n = nrm_mat * m_face_normals[f].cast<double>();
        const double l = n.norm();
        return l > 1e-12 && n.z() / l <= cut;
    };
    size_t           changed = 0;
    std::vector<int> stack { seed };
    m_visit_stamp[seed] = m_visit_epoch;
    const size_t max_visit = 200000;
    size_t       visits    = 0;
    while (! stack.empty() && visits < max_visit) {
        const int f = stack.back();
        stack.pop_back();
        ++ visits;
        if ((centroid(f) - hit_world).squaredNorm() > r2)
            continue;
        if (m_painted[f] != want && (! filter || leans_enough(f))) {
            m_painted[f] = want;
            if (want)
                m_painted_list.push_back(f);
            ++ changed;
        }
        for (int e = 0; e < 3; ++ e) {
            const int nb = m_face_neighbors[f][e];
            if (nb >= 0 && nb < n_facets && m_visit_stamp[nb] != m_visit_epoch) {
                m_visit_stamp[nb] = m_visit_epoch;
                stack.push_back(nb);
            }
        }
    }
    if (changed > 0) {
        if (erase)
            m_painted_list.erase(std::remove_if(m_painted_list.begin(), m_painted_list.end(), [this](int f) { return ! m_painted[f]; }),
                                 m_painted_list.end());
        ++ m_stamp_stamp;
    }
    return changed;
}

void GLGizmoSupportZones::clear_painted()
{
    for (int f : m_painted_list)
        if (f >= 0 && f < int(m_painted.size()))
            m_painted[f] = 0;
    m_painted_list.clear();
    ++ m_stamp_stamp;
}

ExPolygons GLGizmoSupportZones::painted_area_world() const
{
    if (m_painted_area_stamp == m_stamp_stamp && m_painted_area_trafo.isApprox(m_world_trafo))
        return m_painted_area;
    ExPolygons out;
    if (! m_painted_list.empty()) {
        const Polygons tris = world_triangles_xy(m_mesh.its, m_painted_list, m_world_trafo);
        if (! tris.empty())
            out = union_ex(offset(tris, scaled<float>(0.002f)));
    }
    m_painted_area       = out;
    m_painted_area_stamp = m_stamp_stamp;
    m_painted_area_trafo = m_world_trafo;
    return out;
}

void GLGizmoSupportZones::repaint_from_stamps()
{
    clear_painted();
    if (! m_raycaster)
        return;
    for (const Stamp &st : m_stamps) {
        const Vec3d local = m_world_trafo.inverse() * st.p;
        const int   f     = m_raycaster->get_closest_facet(local.cast<float>());
        if (f >= 0)
            paint_facets(f, st.p, st.r, false);
    }
}

bool GLGizmoSupportZones::paint_at(const Vec2d &mouse_pos, bool erase)
{
    update_candidates(mouse_pos);
    const Candidate *live = live_candidate();
    if (live == nullptr)
        return false;
    const double r = std::max(0.2, 0.5 * double(m_foot_size_mm));
    if (erase) {
        const size_t before         = m_stamps.size();
        const size_t painted_before = m_painted_list.size();
        m_stamps.erase(std::remove_if(m_stamps.begin(), m_stamps.end(), [&](const Stamp &st) { return (st.p - live->world_pos).norm() <= r; }),
                       m_stamps.end());
        // Erasing is not painting backwards (it would take what other stamps also cover): repaint from what is left.
        repaint_from_stamps();
        if (m_stamps.size() == before && m_painted_list.size() == painted_before)
            return false;
        if (m_stamps.empty())
            m_has_target = false;
    } else {
        if (! m_stamps.empty() && (m_stamps.back().p - live->world_pos).norm() < 0.8 * r && std::abs(m_stamps.back().r - r) < 1e-6)
            return false;
        if (m_stamps.empty()) {
            m_paint_cut_deg       = m_overhang_threshold_deg;
            m_target_facet_idx    = live->facet_idx;
            m_target_world_pos    = live->world_pos;
            m_target_world_normal = live->world_normal;
            m_has_target          = true;
        }
        if (paint_facets(live->facet_idx, live->world_pos, r, false) == 0 && ! m_stamps.empty())
            return false;
        m_stamps.push_back({ live->world_pos, r });
    }
    invalidate_patch();
    m_preview_dirty      = true;
    m_hover_model_facet  = -1;
    m_target_model_facet = -1;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------
// Block tree: head over what is painted, stumps on the plate, nothing in between
// -----------------------------------------------------------------------------------------------------------------

std::vector<GLGizmoSupportZones::StumpSpot> GLGizmoSupportZones::all_stumps() const
{
    std::vector<StumpSpot> out;
    if (m_has_landing)
        out.push_back(StumpSpot{ m_landing_world_pos, m_landing_on_bed });
    for (const StumpSpot &s : m_extra_stumps)
        out.push_back(s);
    return out;
}

// Painted triangles entirely below the floor (top of the highest stump + 1 mm) become the foot (a prism from the plate
// that the engine takes as one more stump); the rest is the head.
const GLGizmoSupportZones::HeadFoot &GLGizmoSupportZones::head_foot() const
{
    const std::vector<StumpSpot> stumps = all_stumps();
    double floor_z  = - std::numeric_limits<double>::max();
    double foot_bot = std::numeric_limits<double>::max();
    for (const StumpSpot &st : stumps) {
        floor_z  = std::max(floor_z, st.p.z() + STUMP_HEIGHT_MM + 1.0);
        foot_bot = std::min(foot_bot, st.on_bed ? -0.5 : st.p.z());
    }
    if (m_hf_stamp == m_stamp_stamp && m_hf_trafo.isApprox(m_world_trafo) && m_hf_floor == floor_z && m_hf_fbot == foot_bot)
        return m_hf_cache;

    HeadFoot hf;
    hf.floor_z  = floor_z;
    hf.foot_bot = foot_bot;
    std::vector<int> head_faces, foot_faces;
    double hz0 = std::numeric_limits<double>::max(), hz1 = std::numeric_limits<double>::lowest();
    for (int f : m_painted_list) {
        if (f < 0 || f >= int(m_mesh.its.indices.size()))
            continue;
        const Vec3i32 t = m_mesh.its.indices[f];
        double z0 = std::numeric_limits<double>::max(), z1 = std::numeric_limits<double>::lowest();
        for (int k = 0; k < 3; ++ k) {
            const double z = (m_world_trafo * m_mesh.its.vertices[t[k]].cast<double>()).z();
            z0 = std::min(z0, z);
            z1 = std::max(z1, z);
        }
        if (z1 < floor_z)
            foot_faces.push_back(f);
        else {
            hz0 = std::min(hz0, z0);
            hz1 = std::max(hz1, z1);
            head_faces.push_back(f);
        }
    }
    auto to_area = [this](const std::vector<int> &faces) {
        ExPolygons out;
        const Polygons tris = world_triangles_xy(m_mesh.its, faces, m_world_trafo);
        if (tris.empty())
            return out;
        out = union_ex(offset(tris, scaled<float>(0.002f)));
        // Crumbs of one or two loose triangles hold nothing up.
        out.erase(std::remove_if(out.begin(), out.end(), [](const ExPolygon &e) { return e.area() < scaled<double>(1.0) * scaled<double>(1.0); }),
                  out.end());
        return out;
    };
    hf.head = to_area(head_faces);
    hf.foot = to_area(foot_faces);
    if (! hf.head.empty()) {
        hf.head_bot = std::max(hz0, floor_z);
        hf.head_top = std::max(hz1 + ROOF_LIFT_MM, hf.head_bot + 0.4);
    }
    if (! hf.foot.empty())
        hf.foot_top = floor_z - 0.4;
    hf.ok = ! hf.head.empty() || ! hf.foot.empty();

    m_hf_cache = std::move(hf);
    m_hf_stamp = m_stamp_stamp;
    m_hf_trafo = m_world_trafo;
    m_hf_floor = floor_z;
    m_hf_fbot  = foot_bot;
    return m_hf_cache;
}

// The gap between the top of the highest stump and the bottom of the head. If it is not positive the engine does not
// see a block tree and the column comes down like a plain pillar.
double GLGizmoSupportZones::block_tree_gap_mm() const
{
    const HeadFoot &hf = head_foot();
    if (! hf.ok || hf.head.empty())
        return 0.;
    const std::vector<StumpSpot> stumps = all_stumps();
    if (stumps.empty())
        return 0.;
    double stump_top = - std::numeric_limits<double>::max();
    for (const StumpSpot &st : stumps)
        stump_top = std::max(stump_top, st.p.z() + STUMP_HEIGHT_MM);
    return hf.head_bot - stump_top;
}

bool GLGizmoSupportZones::build_block_tree_mesh(TriangleMesh &out_world) const
{
    if (! m_has_target)
        return false;
    const std::vector<StumpSpot> stumps = all_stumps();
    if (stumps.empty())
        return false;
    const HeadFoot &hf = head_foot();
    if (! hf.ok)
        return false;

    // The head edge control still means "pull the painted outline in or out".
    const double     e   = double(m_footprint_shrink_mm);
    const ExPolygons raw = (std::abs(e) < 1e-6) ? hf.head : offset_ex(hf.head, scaled<float>(- e), jtRound);
    if (! hf.head.empty() && raw.empty())
        return false;
    // Simplified (by Clipper, so it cannot self-intersect): the painted outline carries one edge per mesh triangle.
    const ExPolygons head = expolygons_simplify(raw, scaled<double>(0.05));
    if (! hf.head.empty() && head.empty())
        return false;

    indexed_triangle_set its;
    if (! head.empty())
        zone_extrude_prism(head, hf.head_bot, hf.head_top, its);
    if (! hf.head.empty() && its.indices.empty())
        return false;
    if (! hf.foot.empty() && hf.foot_top - hf.foot_bot >= 0.2)
        zone_extrude_prism(expolygons_simplify(hf.foot, scaled<double>(0.05)), hf.foot_bot, hf.foot_top, its);
    if (its.indices.empty())
        return false;

    size_t n_stumps = 0;
    for (const StumpSpot &st : stumps) {
        const double zb = st.on_bed ? -0.5 : st.p.z();
        const double zt = st.p.z() + STUMP_HEIGHT_MM;
        if (zt - zb < 0.2)
            continue;
        const ExPolygons one { ExPolygon(zone_disc(Vec2d(st.p.x(), st.p.y()), 0.5 * double(m_stump_size_mm))) };
        zone_extrude_prism(one, zb, zt, its);
        ++ n_stumps;
    }
    if (n_stumps == 0)
        return false;

    its_merge_vertices(its, true);
    its_remove_degenerate_faces(its, true);
    its_compactify_vertices(its, true);
    if (its.indices.empty())
        return false;
    if (its_volume(its) < 0.f)
        its_flip_triangles(its);
    out_world = TriangleMesh(std::move(its));
    return ! out_world.empty();
}

// -----------------------------------------------------------------------------------------------------------------
// Aimed pillar: plumb from the roof to its lowest point (M), leaning at the chosen angle to the knee (K), plumb to the
// foot (B). The angle is the input; a landing the angle cannot reach is refused.
// -----------------------------------------------------------------------------------------------------------------

bool GLGizmoSupportZones::pillar_axis(PillarAxis &out) const
{
    if (! m_has_target || ! m_has_landing)
        return false;
    const ZoneMask *mk = mask();
    if (mk == nullptr || mk->area.empty())
        return false;
    out.z_low  = mk->z_low;
    out.z_high = mk->z_high;
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.size() < 3)
        return false;
    out.c_top     = footprint_centroid();
    out.z_bot_ext = m_landing_on_bed ? -0.5 : m_landing_world_pos.z();
    if (out.z_high - out.z_bot_ext < 0.2)
        return false;
    out.shift = Vec2d(m_landing_world_pos.x(), m_landing_world_pos.y()) - out.c_top;
    out.wraps = out.z_low > out.z_bot_ext + 0.2;
    const double offset_d = out.shift.norm();
    const double tan_a    = std::tan(Geometry::deg2rad(std::max(0.05, double(m_lean_angle_deg))));
    const double lean_top = out.wraps ? out.z_low : (out.z_high + ROOF_LIFT_MM);
    out.z_knee = (offset_d < 1e-6) ? out.z_bot_ext : (lean_top - offset_d / tan_a);
    if (out.z_knee < out.z_bot_ext - 1e-6)
        return false; // out of reach: this angle does not cover this distance in the height available
    out.has_knee = (offset_d > 1e-6) && (out.z_knee > out.z_bot_ext + 0.2);
    out.rings.clear();
    out.rings.emplace_back(out.z_high + ROOF_LIFT_MM, Vec2d(0., 0.)); // T (real z per point)
    if (out.wraps)
        out.rings.emplace_back(out.z_low, Vec2d(0., 0.));             // M
    if (out.has_knee)
        out.rings.emplace_back(out.z_knee, out.shift);                // K
    out.rings.emplace_back(out.z_bot_ext, out.shift);                 // B
    return true;
}

bool GLGizmoSupportZones::build_pillar_mesh(TriangleMesh &out_world) const
{
    if (block_tree_mode())
        return build_block_tree_mesh(out_world);
    PillarAxis ax;
    if (! pillar_axis(ax))
        return false;
    const ZoneMask *mk = mask();
    if (mk == nullptr || mk->area.empty())
        return false;

    struct Ring { double z; Vec2d off; };
    std::vector<Ring> rings;
    rings.reserve(ax.rings.size());
    for (const auto &r : ax.rings)
        rings.push_back({ r.first, r.second });

    // The edge at the roof and at the foot, interpolated by height: wide where it holds, narrow where it stands.
    const double z_span     = std::max(1e-6, (ax.z_high + ROOF_LIFT_MM) - ax.z_bot_ext);
    const double inset_cap  = max_inset_mm();
    const double outset_cap = max_outset_mm();
    auto edge_at = [&](double z) {
        const double t = std::clamp((z - ax.z_bot_ext) / z_span, 0., 1.);
        const double v = double(m_footprint_base_mm) + (double(m_footprint_shrink_mm) - double(m_footprint_base_mm)) * t;
        return std::clamp(v, - outset_cap, inset_cap);
    };

    int n_ring = 128;
    {
        double perim = 0.;
        for (const ExPolygon &ep : mk->area) {
            const Points &p = ep.contour.points;
            for (size_t i = 0; i < p.size(); ++ i) {
                const Point &a = p[i], &b = p[(i + 1) % p.size()];
                perim += (Vec2d(unscaled<double>(b.x()), unscaled<double>(b.y())) - Vec2d(unscaled<double>(a.x()), unscaled<double>(a.y()))).norm();
            }
        }
        n_ring = std::clamp(int(std::lround(perim / 0.3)), 24, 512);
    }

    std::vector<std::vector<Vec2d>> ring_pts;
    ring_pts.reserve(rings.size());
    for (size_t ri = 0; ri < rings.size(); ++ ri) {
        const double     e         = edge_at((ri == 0) ? (ax.z_high + ROOF_LIFT_MM) : rings[ri].z);
        const ExPolygons ring_area = (std::abs(e) < 1e-6) ? mk->area : offset_ex(mk->area, scaled<float>(- e), jtRound);
        std::vector<Vec2d> pts = resample_ring(ring_area, n_ring);
        if (pts.size() != size_t(n_ring))
            return false; // pushed in too far: no shape, and that is an answer
        for (Vec2d &p : pts)
            p += rings[ri].off;
        ring_pts.push_back(std::move(pts));
    }

    const double z_under_roof = (rings.size() > 1) ? rings[1].z : ax.z_bot_ext;
    indexed_triangle_set its;
    const int nblocks = int(rings.size());
    its.vertices.reserve(size_t(nblocks) * size_t(n_ring));
    for (int r = 0; r < nblocks; ++ r)
        for (int i = 0; i < n_ring; ++ i) {
            const Vec2d &p = ring_pts[r][i];
            double       z = rings[r].z;
            if (r == 0) {
                // Only the roof follows the surface; the tangent plane corrects the z of an offset edge.
                const Vec2d p_src = p - rings[r].off;
                z = z_at(*mk, p_src) + ROOF_LIFT_MM;
                const double e = edge_at(ax.z_high + ROOF_LIFT_MM);
                if (std::abs(e) > 1e-9) {
                    const Vec3d n = n_at(*mk, p_src);
                    if (std::abs(n.z()) > 1e-3) {
                        const Vec2d  d  = p_src - ax.c_top;
                        const double dl = d.norm();
                        if (dl > 1e-9) {
                            const Vec2d u = (d / dl) * (- e);
                            z += - (n.x() * u.x() + n.y() * u.y()) / n.z();
                        }
                    }
                }
                z = std::max(z, z_under_roof + 0.05);
            }
            its.vertices.emplace_back(float(p.x()), float(p.y()), float(z));
        }
    const int bot_base = (nblocks - 1) * n_ring;
    {
        Points cap;
        cap.reserve(n_ring);
        for (int i = 0; i < n_ring; ++ i)
            cap.emplace_back(scaled<coord_t>(ring_pts[0][i].x()), scaled<coord_t>(ring_pts[0][i].y()));
        Polygon cap_poly(std::move(cap));
        if (! cap_poly.is_counter_clockwise())
            return false;
        const std::vector<Vec3i32> idx = Triangulation::triangulate(ExPolygon(cap_poly));
        if (idx.empty())
            return false;
        for (const Vec3i32 &t : idx) {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= n_ring || t[1] >= n_ring || t[2] >= n_ring)
                continue;
            its.indices.emplace_back(t[0], t[1], t[2]);                                 // roof
            its.indices.emplace_back(bot_base + t[0], bot_base + t[2], bot_base + t[1]); // floor
        }
    }
    for (int blk = 0; blk + 1 < nblocks; ++ blk) {
        if (blk > 0 && std::abs(rings[blk + 1].z - rings[blk].z) < 1e-6 && (rings[blk + 1].off - rings[blk].off).squaredNorm() < 1e-12)
            continue;
        const int lo = blk * n_ring, hi = (blk + 1) * n_ring;
        for (int i = 0; i < n_ring; ++ i) {
            const int a = i, b = (i + 1) % n_ring;
            its.indices.emplace_back(lo + a, hi + b, lo + b);
            its.indices.emplace_back(lo + a, hi + a, hi + b);
        }
    }
    // Degenerates are welded, never deleted (deleting leaves open edges).
    its_merge_vertices(its, true);
    its_remove_degenerate_faces(its, true);
    its_compactify_vertices(its, true);
    if (its.indices.empty())
        return false;
    if (its_volume(its) < 0.f)
        its_flip_triangles(its);
    out_world = TriangleMesh(std::move(its));
    return ! out_world.empty();
}

// -----------------------------------------------------------------------------------------------------------------
// Reach and settings
// -----------------------------------------------------------------------------------------------------------------

double GLGizmoSupportZones::support_line_width_mm() const
{
    const DynamicPrintConfig &cfg  = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    double nozzle = 0.4;
    if (const auto *nd = pcfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr && ! nd->values.empty())
        nozzle = nd->values.front();
    double line_w = cfg.has("support_line_width") ? cfg.get_abs_value("support_line_width", nozzle) : 0.;
    if (line_w <= 0.)
        line_w = nozzle * 1.05;
    return line_w;
}

double GLGizmoSupportZones::worst_layer_height_mm() const
{
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double layer_h = cfg.has("layer_height") ? cfg.opt_float("layer_height") : 0.2;
    // With variable layer height the thickest layer rules: the cap is per layer.
    const ModelObject *mo = current_object();
    if (mo != nullptr && mo->has_custom_layering()) {
        const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        if (const auto *mh = pcfg.option<ConfigOptionFloats>("max_layer_height"); mh != nullptr && ! mh->values.empty()) {
            double mx = mh->values.front();
            if (mx <= 0.) {
                double nozzle = 0.4;
                if (const auto *nd = pcfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr && ! nd->values.empty())
                    nozzle = nd->values.front();
                mx = 0.75 * nozzle;
            }
            layer_h = std::max(layer_h, mx);
        }
    }
    return layer_h;
}

double GLGizmoSupportZones::lean_top_z() const
{
    const ZoneMask *mk = mask();
    return mk != nullptr ? mk->z_low : m_target_world_pos.z();
}

double GLGizmoSupportZones::reach_radius_mm(double z_landing) const
{
    if (! m_has_target)
        return 0.;
    const double h = lean_top_z() - z_landing;
    return h <= 0. ? 0. : h * std::tan(Geometry::deg2rad(double(m_lean_angle_deg)));
}

bool GLGizmoSupportZones::landing_out_of_reach() const
{
    if (! m_has_target || ! m_has_landing)
        return false;
    const Vec2d c = footprint_centroid();
    for (const StumpSpot &st : all_stumps()) {
        if ((Vec2d(st.p.x(), st.p.y()) - c).norm() > reach_radius_mm(st.p.z()) + 1e-6)
            return true;
        if (! block_tree_mode())
            break; // a pillar has one landing
    }
    return false;
}

SupportType GLGizmoSupportZones::effective_support_type() const
{
    if (const ModelObject *mo = current_object(); mo != nullptr && mo->config.has("support_type"))
        return mo->config.get().opt_enum<SupportType>("support_type");
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    return cfg.has("support_type") ? cfg.opt_enum<SupportType>("support_type") : stNormalAuto;
}

bool GLGizmoSupportZones::support_style_not_snug() const
{
    if (const ModelObject *mo = current_object(); mo != nullptr && mo->config.has("support_style"))
        return mo->config.get().opt_enum<SupportMaterialStyle>("support_style") != smsSnug;
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    return cfg.has("support_style") && cfg.opt_enum<SupportMaterialStyle>("support_style") != smsSnug;
}

void GLGizmoSupportZones::ensure_snug_style()
{
    if (! support_style_not_snug())
        return;
    const int obj_idx = current_object_idx();
    Model    &model   = wxGetApp().plater()->model();
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()) || model.objects[obj_idx] == nullptr)
        return;
    // Object config, never the preset: visible and removable in the object list, and undoable.
    wxGetApp().plater()->take_snapshot(_u8L("Support zones: support style set to Snug"));
    model.objects[obj_idx]->config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsSnug));
    m_forced_snug = true;
    if (wxGetApp().obj_list() != nullptr)
        wxGetApp().obj_list()->update_and_show_object_settings_item();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

// The support settings the zones were printed with (NeotkoCM), written to the OBJECT and only where the object has
// nothing of its own. Tree support is switched to normal (keeping auto / manual): the tree generator knows no corridor.
bool GLGizmoSupportZones::seed_support_defaults(ModelObject &mo)
{
    bool touched = false;
    auto seed = [&mo, &touched](const char *key, ConfigOption *opt) {
        if (mo.config.has(key)) {
            delete opt;
            return;
        }
        mo.config.set_key_value(key, opt);
        touched = true;
    };
    seed("enable_support",               new ConfigOptionBool(true));
    seed("support_top_z_distance",       new ConfigOptionFloat(0.2));
    seed("tree_support_wall_count",      new ConfigOptionInt(1));
    seed("support_interface_top_layers", new ConfigOptionInt(3));
    seed("support_interface_spacing",    new ConfigOptionFloat(0.));
    seed("support_interface_pattern",    new ConfigOptionEnum<SupportMaterialInterfacePattern>(smipRectilinear));
    if (const SupportType st = effective_support_type(); is_tree(st)) {
        mo.config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(is_auto(st) ? stNormalAuto : stNormal));
        touched = true;
    }
    return touched;
}

void GLGizmoSupportZones::apply_only_my_zones(bool only_mine)
{
    const int obj_idx = current_object_idx();
    Model    &model   = wxGetApp().plater()->model();
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()) || model.objects[obj_idx] == nullptr)
        return;
    const bool tree = is_tree(effective_support_type());
    wxGetApp().plater()->take_snapshot(_u8L("Support zones: only my zones"));
    model.objects[obj_idx]->config.set_key_value("support_type",
        new ConfigOptionEnum<SupportType>(only_mine ? (tree ? stTree : stNormal) : (tree ? stTreeAuto : stNormalAuto)));
    if (wxGetApp().obj_list() != nullptr)
        wxGetApp().obj_list()->update_and_show_object_settings_item();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

// -----------------------------------------------------------------------------------------------------------------
// Zones
// -----------------------------------------------------------------------------------------------------------------

void GLGizmoSupportZones::rebuild_zone_rows()
{
    m_zones.clear();
    m_zones_dirty = false;
    const ModelObject *mo = current_object();
    if (mo == nullptr)
        return;
    // The probe grid is the support pillar resolution, the same number that governs the printed support.
    float step = 2.f;
    {
        const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (cfg.has("support_base_pattern_spacing"))
            step = float(cfg.opt_float("support_base_pattern_spacing"));
        if (mo->config.has("support_base_pattern_spacing"))
            step = float(mo->config.opt_float("support_base_pattern_spacing"));
    }
    Transform3d orient = mo->instances.empty() ? Transform3d::Identity() : mo->instances.front()->get_matrix();
    orient.translation() = Vec3d::Zero();
    for (int vi = 0; vi < int(mo->volumes.size()); ++ vi) {
        const ModelVolume *v = mo->volumes[vi];
        if (v == nullptr || ! v->is_support_enforcer())
            continue;
        ZoneRow row;
        row.volume_idx = vi;
        row.name       = v->name.empty() ? std::string("?") : v->name;
        row.is_zone    = v->config.has("neotko_zone_lean_deg");
        row.lean_deg   = row.is_zone ? v->config.opt_float("neotko_zone_lean_deg") : 0.;
        const SupportZones::ZoneProbe probe = SupportZones::probe_zone(*mo, *v, step, orient);
        row.lit     = probe.lit.size();
        row.sterile = probe.sterile();
        m_zones.push_back(std::move(row));
    }
}

std::string GLGizmoSupportZones::current_gesture_json(const Transform3d &inst, const Vec3d &lock_offset) const
{
    // The gesture in OBJECT space, in NeotkoCM's format (neotko_support_zone_gesture). Descriptive only: the engine never
    // reads it. Kept so projects stay compatible with NeotkoCM, where it makes the zone re-editable.
    const Transform3d inv = inst.inverse();
    const Vec3d tp = inv * m_target_world_pos;
    const Vec3d tn = (inst.linear().transpose() * m_target_world_normal).normalized();
    const Vec3d lp = inv * m_landing_world_pos;
    nlohmann::json r;
    int version = m_shape_covers ? 2 : 1;
    if (! m_stamps.empty())
        version = 3;
    if (block_tree_mode())
        version = 4;
    r["v"]   = version;
    r["tp"]  = { tp.x(), tp.y(), tp.z() };
    r["tn"]  = { tn.x(), tn.y(), tn.z() };
    r["lp"]  = { lp.x(), lp.y(), lp.z() };
    r["bed"] = m_landing_on_bed;
    r["fs"]  = int(m_foot_shape);
    r["fz"]  = m_foot_size_mm;
    if (m_shape_covers)
        r["cv"] = true;
    if (! m_stamps.empty()) {
        nlohmann::json st = nlohmann::json::array();
        for (const Stamp &s : m_stamps) {
            const Vec3d o = inv * s.p;
            st.push_back({ o.x(), o.y(), o.z(), s.r });
        }
        r["st"] = st;
    }
    if (! m_extra_stumps.empty()) {
        nlohmann::json tk = nlohmann::json::array();
        for (const StumpSpot &s : m_extra_stumps) {
            const Vec3d o = inv * s.p;
            tk.push_back({ o.x(), o.y(), o.z(), s.on_bed ? 1. : 0. });
        }
        r["tk"] = tk;
    }
    if (block_tree_mode())
        r["tz"] = m_stump_size_mm;
    r["ep"] = m_footprint_shrink_mm;
    r["ef"] = m_footprint_base_mm;
    r["la"] = m_lean_angle_deg;
    r["lo"] = { lock_offset.x(), lock_offset.y(), lock_offset.z() };
    if (block_tree_mode() && m_paint_overhang_only) {
        r["oo"] = true;
        r["oc"] = m_paint_cut_deg;
    }
    return r.dump();
}

void GLGizmoSupportZones::create_pillar()
{
    if (get_state() != On)
        return;
    TriangleMesh mesh;
    if (! build_pillar_mesh(mesh))
        return;
    Model    *model   = m_parent.get_selection().get_model();
    const int obj_idx = current_object_idx();
    if (model == nullptr || obj_idx < 0 || obj_idx >= int(model->objects.size()))
        return;
    ModelObject *mo = model->objects[obj_idx];
    if (mo == nullptr || mo->instances.empty())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add support zone"));
    const bool seeded = seed_support_defaults(*mo);

    // add_volume() centres the mesh it is given, so a placeholder goes in first and the real mesh is set afterwards.
    ModelVolume *nv = mo->add_volume(make_cube(1., 1., 1.), ModelVolumeType::SUPPORT_ENFORCER);
    if (nv == nullptr)
        return;
    const Transform3d inst = mo->instances.front()->get_matrix();
    // Mesh in OBJECT space, centred on its own origin, placed by the volume offset.
    mesh.transform(inst.inverse());
    const Vec3d local_centre = mesh.bounding_box().center();
    mesh.translate(float(- local_centre.x()), float(- local_centre.y()), float(- local_centre.z()));
    nv->set_mesh(std::move(mesh));
    nv->calculate_convex_hull();
    nv->invalidate_convex_hull_2d();
    nv->set_new_unique_id();
    nv->set_transformation(Geometry::Transformation());
    nv->set_offset(local_centre);
    nv->config.set_key_value("extruder", new ConfigOptionInt(0));
    nv->config.set_key_value("neotko_support_zone_gesture", new ConfigOptionString(current_gesture_json(inst, local_centre)));
    // The key that makes this enforcer a support zone for the engine (and its lean).
    nv->config.set_key_value("neotko_zone_lean_deg", new ConfigOptionFloat(double(m_lean_angle_deg)));
    nv->config.set_key_value("neotko_zone_roof_only", new ConfigOptionBool(m_opt_roof_only));
    nv->config.set_key_value("neotko_zone_solid", new ConfigOptionBool(m_opt_solid));
    nv->config.set_key_value("neotko_zone_land_only", new ConfigOptionBool(m_opt_land_only));
    nv->name = into_u8(block_tree_mode() ? _L("Support zone (stump)") : _L("Support zone"));
    nv->source.is_from_builtin_objects = true;
    Slic3r::save_object_mesh(*mo);

    clear_pick();
    m_zones_dirty = true;
    if (wxGetApp().obj_list() != nullptr) {
        wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(obj_idx));
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
        if (seeded)
            wxGetApp().obj_list()->update_and_show_object_settings_item();
    }
    wxGetApp().plater()->update();
    m_parent.set_as_dirty();
}

void GLGizmoSupportZones::delete_zone(int volume_idx)
{
    if (get_state() != On)
        return;
    const int obj_idx = current_object_idx();
    const ModelObject *mo = current_object();
    if (mo == nullptr || volume_idx < 0 || volume_idx >= int(mo->volumes.size()) || wxGetApp().obj_list() == nullptr)
        return;
    clear_pick();
    m_zones_dirty = true;
    // Through the object list (it takes its own snapshot and keeps the tree in sync with the model).
    wxGetApp().obj_list()->delete_from_model_and_list(itVolume, obj_idx, volume_idx);
    m_parent.set_as_dirty();
}

// -----------------------------------------------------------------------------------------------------------------
// Cut: a polygon drawn on screen, extruded along the view through the object, saved as a SUPPORT_BLOCKER with
// neotko_support_cut = true; the engine subtracts it from every support layer.
// -----------------------------------------------------------------------------------------------------------------

void GLGizmoSupportZones::render_cut_polygon()
{
    if (! m_cut_mode || m_cut_points.empty())
        return;
    const Size  cnv_size   = m_parent.get_canvas_size();
    const float cnv_width  = float(cnv_size.get_width());
    const float cnv_height = float(cnv_size.get_height());
    if (cnv_width == 0.f || cnv_height == 0.f)
        return;
    auto to_ndc = [cnv_width, cnv_height](const Vec2d &p) {
        return Vec2f(2.f * (float(p.x()) / cnv_width - 0.5f), -2.f * (float(p.y()) / cnv_height - 0.5f));
    };
    m_cut_overlay.reset();
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P2 };
    init_data.color  = { 1.0f, 0.35f, 0.25f, 1.0f };
    init_data.reserve_vertices(m_cut_points.size() + 1);
    init_data.reserve_indices(m_cut_points.size() + 1);
    for (size_t i = 0; i < m_cut_points.size(); ++ i) {
        init_data.add_vertex(to_ndc(m_cut_points[i]));
        init_data.add_index((unsigned int) i);
    }
    init_data.add_vertex(to_ndc(m_parent.get_local_mouse_position()));
    init_data.add_index((unsigned int) m_cut_points.size());
    m_cut_overlay.init_from(std::move(init_data));
    if (GLShaderProgram *shader = wxGetApp().get_shader("flat"); shader != nullptr) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_cut_overlay.render();
        shader->stop_using();
        glsafe(::glEnable(GL_DEPTH_TEST));
    }
}

bool GLGizmoSupportZones::build_cut_mesh(const std::vector<Vec2d> &screen_pts, TriangleMesh &out_world) const
{
    const ModelObject *mo = current_object();
    if (mo == nullptr || mo->instances.empty() || screen_pts.size() < 3)
        return false;
    Points ipts;
    ipts.reserve(screen_pts.size());
    for (const Vec2d &p : screen_pts)
        ipts.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    ExPolygons ex = union_ex(Polygons{ Polygon(ipts) });
    if (ex.empty())
        return false;
    std::sort(ex.begin(), ex.end(), [](const ExPolygon &a, const ExPolygon &b) { return a.area() > b.area(); });
    const Polygon contour = ex.front().contour;
    if (contour.points.size() < 3)
        return false;
    BoundingBoxf3 bb = mo->instance_bounding_box(0);
    bb.offset(5.);
    const Camera &camera = wxGetApp().plater()->get_camera();
    const size_t  n      = contour.points.size();
    std::vector<Vec3d> near_ring(n), far_ring(n);
    for (size_t i = 0; i < n; ++ i) {
        const Vec2d sp(unscaled<double>(contour.points[i].x()), unscaled<double>(contour.points[i].y()));
        Vec3d src, dir;
        CameraUtils::ray_from_screen_pos(camera, sp, src, dir);
        dir.normalize();
        double t0 = std::numeric_limits<double>::max(), t1 = std::numeric_limits<double>::lowest();
        for (int c = 0; c < 8; ++ c) {
            const Vec3d corner((c & 1) ? bb.max.x() : bb.min.x(), (c & 2) ? bb.max.y() : bb.min.y(), (c & 4) ? bb.max.z() : bb.min.z());
            const double t = (corner - src).dot(dir);
            t0 = std::min(t0, t);
            t1 = std::max(t1, t);
        }
        near_ring[i] = src + dir * t0;
        far_ring[i]  = src + dir * t1;
    }
    indexed_triangle_set its;
    its.vertices.reserve(2 * n);
    for (const Vec3d &p : near_ring)
        its.vertices.emplace_back(p.cast<float>());
    for (const Vec3d &p : far_ring)
        its.vertices.emplace_back(p.cast<float>());
    const std::vector<Vec3i32> cap = Triangulation::triangulate(contour);
    for (const Vec3i32 &t : cap) {
        its.indices.emplace_back(t[0], t[1], t[2]);
        its.indices.emplace_back(int(n) + t[2], int(n) + t[1], int(n) + t[0]);
    }
    for (size_t i = 0; i < n; ++ i) {
        const int a = int(i), b = int((i + 1) % n);
        its.indices.emplace_back(a, int(n) + a, int(n) + b);
        its.indices.emplace_back(a, int(n) + b, b);
    }
    if (its.indices.empty())
        return false;
    if (its_volume(its) < 0.f)
        its_flip_triangles(its);
    out_world = TriangleMesh(std::move(its));
    return ! out_world.empty();
}

void GLGizmoSupportZones::create_cut(std::vector<Vec2d> screen_pts)
{
    if (get_state() != On)
        return;
    TriangleMesh mesh;
    if (! build_cut_mesh(screen_pts, mesh))
        return;
    Model    *model   = m_parent.get_selection().get_model();
    const int obj_idx = current_object_idx();
    if (model == nullptr || obj_idx < 0 || obj_idx >= int(model->objects.size()))
        return;
    ModelObject *mo = model->objects[obj_idx];
    if (mo == nullptr || mo->instances.empty())
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add support cut"));
    ModelVolume *nv = mo->add_volume(make_cube(1., 1., 1.), ModelVolumeType::SUPPORT_BLOCKER);
    if (nv == nullptr)
        return;
    const Transform3d inst = mo->instances.front()->get_matrix();
    mesh.transform(inst.inverse());
    const Vec3d local_centre = mesh.bounding_box().center();
    mesh.translate(float(- local_centre.x()), float(- local_centre.y()), float(- local_centre.z()));
    nv->set_mesh(std::move(mesh));
    nv->calculate_convex_hull();
    nv->invalidate_convex_hull_2d();
    nv->set_new_unique_id();
    nv->set_transformation(Geometry::Transformation());
    nv->set_offset(local_centre);
    nv->config.set_key_value("neotko_support_cut", new ConfigOptionBool(true));
    nv->name = into_u8(_L("Support cut"));
    nv->source.is_from_builtin_objects = true;
    Slic3r::save_object_mesh(*mo);
    if (wxGetApp().obj_list() != nullptr) {
        wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(obj_idx));
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    }
    wxGetApp().plater()->update();
    m_parent.set_as_dirty();
}

// -----------------------------------------------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------------------------------------------

// The highlight shows exactly what will be taken: the painted triangles for the brush, else the mask triangulated on
// its roof. Built in OBJECT space (drawn with the instance matrix).
void GLGizmoSupportZones::build_face_model(GLModel &model, int facet_idx, const Vec2d &centre_xy, bool hover, const ColorRGBA &col)
{
    model.reset();
    if (block_tree_mode() && ! hover) {
        if (m_painted_list.empty())
            return;
        const float lift = 0.10f;
        indexed_triangle_set its;
        its.vertices.reserve(m_painted_list.size() * 3);
        its.indices.reserve(m_painted_list.size());
        int base = 0;
        for (int f : m_painted_list) {
            if (f < 0 || f >= int(m_mesh.its.indices.size()))
                continue;
            const Vec3i32 tri = m_mesh.its.indices[f];
            Vec3f n = m_face_normals[f];
            const float nl = n.norm();
            if (nl < 1e-6f)
                continue;
            n /= nl;
            its.vertices.push_back(m_mesh.its.vertices[tri[0]] + n * lift);
            its.vertices.push_back(m_mesh.its.vertices[tri[1]] + n * lift);
            its.vertices.push_back(m_mesh.its.vertices[tri[2]] + n * lift);
            its.indices.emplace_back(base, base + 1, base + 2);
            base += 3;
        }
        if (! its.indices.empty()) {
            model.init_from(its);
            model.set_color(col);
        }
        return;
    }
    if (block_tree_mode() && hover) {
        // The brush under the cursor: the triangles the stroke would mark around this point.
        return;
    }
    // Hover masks get their own (coarse) height map and never touch the target's cached mask.
    ZoneMask        local;
    const ZoneMask *mk = nullptr;
    if (hover) {
        if (! build_mask(facet_idx, centre_xy, local, 4000))
            return;
        mk = &local;
    } else
        mk = mask();
    if (mk == nullptr || mk->area.empty())
        return;
    const Transform3d inv  = m_world_trafo.inverse();
    const double      lift = 0.10;
    indexed_triangle_set its;
    for (const ExPolygon &ep : mk->area) {
        const Points               pts = to_points(ep);
        const std::vector<Vec3i32> idx = Triangulation::triangulate(ep);
        if (idx.empty() || pts.empty())
            continue;
        const int base = int(its.vertices.size());
        for (const Point &q : pts) {
            const Vec2d p2(unscaled<double>(q.x()), unscaled<double>(q.y()));
            const Vec3d n = n_at(*mk, p2);
            const Vec3d w = inv * (Vec3d(p2.x(), p2.y(), z_at(*mk, p2)) + n * lift);
            its.vertices.emplace_back(float(w.x()), float(w.y()), float(w.z()));
        }
        for (const Vec3i32 &t : idx) {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= int(pts.size()) || t[1] >= int(pts.size()) || t[2] >= int(pts.size()))
                continue;
            its.indices.emplace_back(base + t[0], base + t[1], base + t[2]);
        }
    }
    if (its.indices.empty())
        return;
    model.init_from(its);
    model.set_color(col);
}

void GLGizmoSupportZones::render_pick_overlays()
{
    if (! m_target_pick_mode && ! m_has_target)
        return;
    if (m_target_pick_mode && m_have_hover_pos && ! m_painting)
        update_candidates(m_hover_mouse_pos);

    const Candidate *live       = m_target_pick_mode ? live_candidate() : nullptr;
    const int        hover_face = (live != nullptr && ! block_tree_mode()) ? live->facet_idx : -1;
    const bool       hover_past = live != nullptr && live->past_threshold;
    const Vec2d      hover_c    = live != nullptr ? Vec2d(live->world_pos.x(), live->world_pos.y()) : Vec2d(0., 0.);
    const int        key        = int(m_foot_shape) * 1000 + (m_shape_covers ? 100 : 0) + int(std::lround(m_foot_size_mm * 4.f));
    const bool centre_moved = (m_foot_shape == FootShape::Round || m_foot_shape == FootShape::Square) && (hover_c - m_hover_model_centre).norm() > 0.5;
    if (hover_face != m_hover_model_facet || hover_past != m_hover_model_past || centre_moved || key != m_hover_model_key) {
        m_hover_model_centre = hover_c;
        m_hover_model_key    = key;
        if (hover_face >= 0)
            build_face_model(m_hover_model, hover_face, hover_c, true, hover_past ? kHoverCol : kHoverBelowCol);
        else
            m_hover_model.reset();
        m_hover_model_facet = hover_face;
        m_hover_model_past  = hover_past;
    }
    if (m_target_facet_idx != m_target_model_facet || key != m_target_model_key || m_stamps.size() != m_target_model_stamps ||
        (block_tree_mode() && m_painting)) {
        m_target_model_key    = key;
        m_target_model_stamps = m_stamps.size();
        if (m_has_target && m_target_facet_idx >= 0)
            build_face_model(m_target_model, m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()), false, kTargetCol);
        else
            m_target_model.reset();
        m_target_model_facet = m_target_facet_idx;
    }
    render_world_model(m_target_model, m_world_trafo, true);
    render_world_model(m_hover_model, m_world_trafo, true);
}

void GLGizmoSupportZones::update_preview()
{
    m_preview_dirty = false;
    m_preview_model.reset();
    TriangleMesh mesh;
    if (! build_pillar_mesh(mesh))
        return;
    m_preview_model.init_from(mesh.its);
    m_preview_model.set_color(kPreviewCol);
}

void GLGizmoSupportZones::render_preview()
{
    // While painting the solid is not rebuilt (it would cost one build per stroke mark); it appears on release.
    if (m_painting)
        return;
    if (m_preview_dirty)
        update_preview();
    render_world_model(m_preview_model, Transform3d::Identity(), false);
}

void GLGizmoSupportZones::render_landing_marks()
{
    if (! m_has_target || ! m_has_landing)
        return;
    indexed_triangle_set its;
    const double r = block_tree_mode() ? 0.5 * double(m_stump_size_mm) : 1.5;
    for (const StumpSpot &st : all_stumps()) {
        const ExPolygons disc { ExPolygon(zone_disc(Vec2d(st.p.x(), st.p.y()), std::max(0.5, r))) };
        zone_extrude_prism(disc, st.p.z() + 0.02, st.p.z() + 0.12, its);
        if (! block_tree_mode())
            break;
    }
    m_marks_model.reset();
    if (its.indices.empty())
        return;
    m_marks_model.init_from(its);
    m_marks_model.set_color(kMarkCol);
    render_world_model(m_marks_model, Transform3d::Identity(), true);
}

void GLGizmoSupportZones::on_render()
{
    // While a pick, the brush or the cut is armed, a drag that starts on the part must not move it.
    m_parent.enable_moving(! (m_target_pick_mode || m_landing_pick_mode || m_stump_pick_mode || m_painting || m_cut_mode));
    if (m_show_overhangs) {
        m_parent.set_slope_normal_angle(90.f - m_overhang_threshold_deg);
        if (! m_parent.is_using_slope())
            m_parent.use_slope(true);
    } else if (m_parent.is_using_slope())
        m_parent.use_slope(false);
    ensure_raycaster();
    render_pick_overlays();
    render_preview();
    render_landing_marks();
    render_cut_polygon();
}

// -----------------------------------------------------------------------------------------------------------------
// Mouse
// -----------------------------------------------------------------------------------------------------------------

bool GLGizmoSupportZones::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mpos(mouse_event.GetX(), mouse_event.GetY());

    if (m_cut_mode) {
        // Click adds a vertex, a click on the first one closes, right click cancels. The DOWN is only remembered so
        // a drag still orbits the camera (and drops the polygon: its points are screen points).
        if (mouse_event.LeftDown()) {
            m_mouse_down_pos = mpos;
            m_mouse_down     = true;
            return false;
        }
        if (mouse_event.Moving() || mouse_event.Dragging()) {
            m_parent.set_as_dirty();
            return false;
        }
        if (mouse_event.LeftUp()) {
            const bool was_down = m_mouse_down;
            m_mouse_down = false;
            if (was_down && (mpos - m_mouse_down_pos).norm() > CLICK_SLOP_PX) {
                m_cut_points.clear();
                m_parent.set_as_dirty();
                return false;
            }
            if (m_cut_points.size() >= 3 && (mpos - m_cut_points.front()).norm() <= CUT_CLOSE_RADIUS_PX) {
                std::vector<Vec2d> pts = std::move(m_cut_points);
                m_cut_points.clear();
                wxGetApp().CallAfter([this, pts]() { create_cut(pts); });
            } else
                m_cut_points.push_back(mpos);
            m_parent.set_as_dirty();
            return true;
        }
        if (mouse_event.RightDown())
            return ! m_cut_points.empty();
        if (mouse_event.RightUp() && ! m_cut_points.empty()) {
            m_cut_points.clear();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    const bool picking = m_target_pick_mode || (m_landing_pick_mode && m_has_target) || (m_stump_pick_mode && m_has_target);
    if (! picking) {
        m_mouse_down = false;
        return false;
    }

    // The DOWN is only remembered, never consumed, so a left drag still orbits the camera. Everything happens on the
    // UP, which is always consumed while picking (a click on empty space must not deselect the object).
    if (mouse_event.LeftDown()) {
        m_mouse_down_pos = mpos;
        m_mouse_down     = true;
        // The brush is the one exception, and only when the drag starts ON the surface.
        if (m_target_pick_mode && block_tree_mode()) {
            m_paint_erase = mouse_event.ShiftDown();
            if (paint_at(mpos, m_paint_erase)) {
                m_painting = true;
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
                return true;
            }
        }
        return false;
    }
    if (m_painting && (mouse_event.Dragging() || mouse_event.Moving())) {
        m_hover_mouse_pos = mpos;
        m_have_hover_pos  = true;
        paint_at(mpos, m_paint_erase);
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }
    if (m_painting && mouse_event.LeftUp()) {
        m_painting      = false;
        m_mouse_down    = false;
        m_preview_dirty = true;
        // The stump is planted plumb under the middle of what is painted, unless the user placed it.
        if (m_has_target && ! m_landing_locked) {
            Vec3d plumb_pos;
            bool  plumb_on_bed = true;
            if (resolve_landing_plumb(plumb_pos, plumb_on_bed)) {
                m_landing_world_pos = plumb_pos;
                m_landing_on_bed    = plumb_on_bed;
                m_has_landing       = true;
            }
        }
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }
    if (mouse_event.Moving() || mouse_event.Dragging()) {
        m_hover_mouse_pos = mpos;
        m_have_hover_pos  = true;
        if (m_landing_pick_mode && m_has_target && ! m_landing_locked && ! mouse_event.Dragging()) {
            Vec3d pos;
            bool  on_bed = true;
            if (resolve_landing(mpos, pos, on_bed)) {
                m_landing_world_pos = pos;
                m_landing_on_bed    = on_bed;
                m_has_landing       = true;
                m_preview_dirty     = true;
            }
        }
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return false;
    }
    if (mouse_event.LeftUp()) {
        const bool was_down = m_mouse_down;
        m_mouse_down = false;
        if (was_down && (mpos - m_mouse_down_pos).norm() > CLICK_SLOP_PX)
            return false; // a camera orbit
        if (m_target_pick_mode && ! block_tree_mode()) {
            update_candidates(mpos);
            if (const Candidate *live = live_candidate(); live != nullptr) {
                m_has_target          = true;
                m_target_facet_idx    = live->facet_idx;
                m_target_world_pos    = live->world_pos;
                m_target_world_normal = live->world_normal;
                m_preview_dirty       = true;
                m_target_pick_mode    = false;
                invalidate_patch();
                // The landing is placed plumb under the middle of what was taken; moving it is a deliberate act.
                Vec3d plumb_pos;
                bool  plumb_on_bed = true;
                m_has_landing = resolve_landing_plumb(plumb_pos, plumb_on_bed);
                if (m_has_landing) {
                    m_landing_world_pos = plumb_pos;
                    m_landing_on_bed    = plumb_on_bed;
                    m_landing_pick_mode = false;
                    m_landing_locked    = false;
                }
            }
        } else if (m_stump_pick_mode && m_has_target) {
            // Plant a stump, or pull one out by clicking inside it.
            Vec3d pos;
            bool  on_bed = true;
            if (resolve_landing(mpos, pos, on_bed)) {
                const double r       = std::max(0.5, 0.5 * double(m_stump_size_mm));
                bool         removed = false;
                for (size_t i = 0; i < m_extra_stumps.size(); ++ i)
                    if ((Vec2d(m_extra_stumps[i].p.x(), m_extra_stumps[i].p.y()) - Vec2d(pos.x(), pos.y())).norm() < r) {
                        m_extra_stumps.erase(m_extra_stumps.begin() + i);
                        removed = true;
                        break;
                    }
                if (! removed)
                    m_extra_stumps.push_back(StumpSpot{ pos, on_bed });
                m_preview_dirty = true;
            }
        } else if (m_landing_pick_mode && m_has_target) {
            Vec3d pos;
            bool  on_bed = true;
            if (resolve_landing(mpos, pos, on_bed)) {
                m_landing_world_pos = pos;
                m_landing_on_bed    = on_bed;
                m_has_landing       = true;
                m_landing_locked    = true;
                m_landing_pick_mode = false;
                m_preview_dirty     = true;
            }
        }
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }
    // The wheel cycles stacked downward surfaces under the cursor, only when there is something to cycle.
    if (m_target_pick_mode && ! block_tree_mode() && mouse_event.GetWheelRotation() != 0 && m_candidates.size() > 1) {
        const int n = int(m_candidates.size());
        m_candidate_idx += (mouse_event.GetWheelRotation() > 0) ? -1 : 1;
        m_candidate_idx = (m_candidate_idx % n + n) % n;
        m_hover_model_facet = -1;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------------------------------------------
// Panel
// -----------------------------------------------------------------------------------------------------------------

void GLGizmoSupportZones::render_zone_list()
{
    if (m_zones_dirty)
        rebuild_zone_rows();
    ImGui::Separator();
    ImGui::TextUnformatted(_u8L("Zones of this object").c_str());
    if (m_zones.empty()) {
        ImGui::TextDisabled("%s", _u8L("No support zone yet.").c_str());
    }
    int to_delete = -1;
    for (size_t i = 0; i < m_zones.size(); ++ i) {
        const ZoneRow &z = m_zones[i];
        std::string label = "#" + std::to_string(i + 1) + " " + z.name;
        if (z.is_zone)
            label += " (" + std::to_string(int(std::lround(z.lean_deg))) + " deg)";
        ImGui::AlignTextToFramePadding();
        if (z.sterile)
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f), "%s - %s", label.c_str(), _u8L("catches nothing").c_str());
        else
            ImGui::Text("%s - %s %d", label.c_str(), _u8L("catches").c_str(), int(z.lit));
        if (z.sterile && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("This zone does not reach any downward-facing surface: it will not produce any support.").c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton((_u8L("Delete") + "##zdel" + std::to_string(i)).c_str()))
            to_delete = z.volume_idx;
    }
    if (to_delete >= 0)
        wxGetApp().CallAfter([this, to_delete]() { delete_zone(to_delete); });

    // "Only my zones": the automatic support is switched off for this object (manual support type).
    const SupportType st      = effective_support_type();
    bool              only_my = ! is_auto(st);
    if (ImGui::Checkbox((_u8L("Only my zones (no automatic support)") + "##onlymine").c_str(), &only_my)) {
        const bool v = only_my;
        wxGetApp().CallAfter([this, v]() {
            if (get_state() == On)
                apply_only_my_zones(v);
        });
    }
}

void GLGizmoSupportZones::render_panel()
{
    const float wrap = ImGui::GetFontSize() * 24.f;
    ImGui::PushTextWrapPos(wrap);

    // --- step 1 --------------------------------------------------------------------------------------------------
    ImGui::TextUnformatted(_u8L("1. Footprint").c_str());
    int shape = int(m_foot_shape);
    bool shape_changed = false;
    shape_changed |= ImGui::RadioButton((_u8L("Whole patch") + "##fs0").c_str(), &shape, 0);
    ImGui::SameLine();
    shape_changed |= ImGui::RadioButton((_u8L("Round") + "##fs1").c_str(), &shape, 1);
    ImGui::SameLine();
    shape_changed |= ImGui::RadioButton((_u8L("Square") + "##fs2").c_str(), &shape, 2);
    ImGui::SameLine();
    shape_changed |= ImGui::RadioButton((_u8L("Paint (stumps)") + "##fs3").c_str(), &shape, 3);
    if (shape_changed && shape != int(m_foot_shape)) {
        const bool was_brush = block_tree_mode();
        m_foot_shape = FootShape(shape);
        if (was_brush != block_tree_mode())
            clear_pick(); // a pillar and a block tree are picked differently
        invalidate_patch();
        m_preview_dirty = true;
    }
    if (m_foot_shape != FootShape::Patch) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.f);
        const std::string size_label = block_tree_mode() ? _u8L("Brush size (mm)") : _u8L("Size (mm)");
        if (ImGui::SliderFloat((size_label + "##fsize").c_str(), &m_foot_size_mm, 1.f, 60.f, "%.1f")) {
            invalidate_patch();
            m_preview_dirty = true;
        }
        ImGui::PopItemWidth();
    }
    if (m_foot_shape == FootShape::Round || m_foot_shape == FootShape::Square) {
        if (ImGui::Checkbox((_u8L("Cover (the shape may grow past the surface)") + "##cover").c_str(), &m_shape_covers)) {
            invalidate_patch();
            m_preview_dirty = true;
        }
    }
    if (block_tree_mode()) {
        if (ImGui::Checkbox((_u8L("Only mark overhanging faces") + "##ovonly").c_str(), &m_paint_overhang_only)) {}
    }

    const bool picking_now = m_target_pick_mode;
    const std::string pick_label = block_tree_mode() ?
        (picking_now ? _u8L("Stop painting") : _u8L("Paint the area to hold up")) :
        (picking_now ? _u8L("Cancel pick") : (m_has_target ? _u8L("Pick another surface") : _u8L("Pick the surface to hold up")));
    if (ImGui::Button((pick_label + "##pick").c_str())) {
        m_target_pick_mode  = ! m_target_pick_mode;
        m_landing_pick_mode = false;
        m_stump_pick_mode   = false;
        m_cut_mode          = false;
    }
    if (m_target_pick_mode) {
        ImGui::TextDisabled("%s", (block_tree_mode() ? _u8L("Drag on the part to paint, Shift + drag to erase.") :
                                                       _u8L("Click a surface facing down. The wheel cycles stacked surfaces.")).c_str());
    }
    if (block_tree_mode() && ! m_stamps.empty()) {
        ImGui::SameLine();
        if (ImGui::Button((_u8L("Clear paint") + "##clrpaint").c_str()))
            clear_pick();
    }

    // --- step 2 --------------------------------------------------------------------------------------------------
    if (m_has_target) {
        ImGui::Separator();
        ImGui::TextUnformatted((block_tree_mode() ? _u8L("2. Stumps") : _u8L("2. Landing")).c_str());
        if (m_has_landing) {
            if (m_landing_on_bed)
                ImGui::Text("%s", _u8L("Lands on the plate.").c_str());
            else
                ImGui::Text("%s %.2f mm", _u8L("Lands on the part at Z").c_str(), m_landing_world_pos.z());
        }
        if (ImGui::Button(((m_landing_pick_mode ? _u8L("Cancel") : _u8L("Move it")) + "##moveland").c_str())) {
            m_landing_pick_mode = ! m_landing_pick_mode;
            m_landing_locked    = false;
            m_stump_pick_mode   = false;
            m_target_pick_mode  = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Click on the plate or on a shelf of the part to place the foot.").c_str());
        if (block_tree_mode()) {
            ImGui::SameLine();
            if (ImGui::Button(((m_stump_pick_mode ? _u8L("Done") : _u8L("Add / remove stumps")) + "##stumps").c_str())) {
                m_stump_pick_mode   = ! m_stump_pick_mode;
                m_landing_pick_mode = false;
                m_target_pick_mode  = false;
            }
            ImGui::PushItemWidth(ImGui::GetFontSize() * 10.f);
            if (ImGui::SliderFloat((_u8L("Stump size (mm)") + "##stsize").c_str(), &m_stump_size_mm, 2.f, 40.f, "%.1f"))
                m_preview_dirty = true;
            ImGui::PopItemWidth();
            ImGui::Text("%s %d", _u8L("Stumps:").c_str(), int(all_stumps().size()));
        }

        // --- shape --------------------------------------------------------------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted(_u8L("3. Shape").c_str());
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.f);
        if (ImGui::SliderFloat((_u8L("Lean angle") + "##lean").c_str(), &m_lean_angle_deg, 1.f,
                               float(SupportZones::SUPPORT_ZONE_MAX_LEAN_DEG), "%.0f deg"))
            m_preview_dirty = true;
        const double rec = SupportZones::corridor_max_angle_deg(worst_layer_height_mm(), support_line_width_mm());
        if (rec > 0.)
            ImGui::TextDisabled("%s %.0f deg", _u8L("Recommended maximum for this layer height:").c_str(), rec);
        const float inset  = std::max(0.01f, float(max_inset_mm()));
        const float outset = std::max(0.01f, float(max_outset_mm()));
        if (ImGui::SliderFloat((_u8L("Head edge (mm)") + "##ehead").c_str(), &m_footprint_shrink_mm, - outset, inset, "%.2f")) {
            m_foot_cache_version = size_t(-1);
            m_preview_dirty      = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Positive pulls the edge in, negative grows it.").c_str());
        if (! block_tree_mode()) {
            if (ImGui::SliderFloat((_u8L("Foot edge (mm)") + "##efoot").c_str(), &m_footprint_base_mm, - outset, inset, "%.2f"))
                m_preview_dirty = true;
        }
        ImGui::PopItemWidth();
        if (ImGui::CollapsingHeader((_u8L("Advanced") + "##adv").c_str())) {
            ImGui::Checkbox((_u8L("Grab only at its roof") + "##roofonly").c_str(), &m_opt_roof_only);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("The zone seeds contact only where its block ends at the top, not where it crosses the part.").c_str());
            ImGui::Checkbox((_u8L("Print the whole block") + "##solid").c_str(), &m_opt_solid);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("The section of the block is printed on every layer, touching an overhang or not.").c_str());
            ImGui::Checkbox((_u8L("Only land at the end") + "##landonly").c_str(), &m_opt_land_only);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("The column does not rest on the shelves it crosses, only on its landing.").c_str());
        }

        // --- warnings -----------------------------------------------------------------------------------------
        if (landing_out_of_reach())
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f), "%s",
                               _u8L("Out of reach: at this angle the column cannot get there. Raise the angle or move the foot.").c_str());
        if (block_tree_mode() && m_has_landing && block_tree_gap_mm() <= 0.)
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f), "%s",
                               _u8L("No gap between the head and the stumps: it will come down as a plain pillar.").c_str());
        if (support_style_not_snug())
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f), "%s",
                               _u8L("Support style is not Snug: a leaning column will come out in grid steps. Snug is recommended.").c_str());
        else if (m_forced_snug)
            ImGui::TextDisabled("%s", _u8L("Support style set to Snug for this object (object setting, undoable).").c_str());
        if (is_tree(effective_support_type()))
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f), "%s",
                               _u8L("Tree support ignores the corridor: creating a zone switches this object to normal support.").c_str());

        // --- create -------------------------------------------------------------------------------------------
        if (m_preview_dirty && ! m_painting)
            update_preview();
        const bool can_create = m_preview_model.is_initialized();
        m_imgui->disabled_begin(! can_create);
        if (ImGui::Button((_u8L("Create zone") + "##create").c_str()) && can_create)
            wxGetApp().CallAfter([this]() { create_pillar(); });
        m_imgui->disabled_end();
        ImGui::SameLine();
        if (ImGui::Button((_u8L("Cancel") + "##cancelall").c_str()))
            clear_pick();
    }

    // --- cut ---------------------------------------------------------------------------------------------------
    ImGui::Separator();
    if (ImGui::Button(((m_cut_mode ? _u8L("Stop cutting") : _u8L("Draw a support cut")) + "##cut").c_str())) {
        m_cut_mode = ! m_cut_mode;
        m_cut_points.clear();
        if (m_cut_mode) {
            m_target_pick_mode = m_landing_pick_mode = m_stump_pick_mode = false;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("A polygon drawn on screen, cut through the part along the view. It removes every support "
                                     "inside it, including columns coming down from above.").c_str());
    if (m_cut_mode)
        ImGui::TextDisabled("%s", _u8L("Click to add points, click the first point to close, right click to cancel.").c_str());

    // --- view ---------------------------------------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Checkbox((_u8L("Show overhangs") + "##showov").c_str(), &m_show_overhangs);
    if (m_show_overhangs) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.f);
        ImGui::SliderFloat((_u8L("Threshold") + "##ovthr").c_str(), &m_overhang_threshold_deg, 5.f, 85.f, "%.0f deg");
        ImGui::PopItemWidth();
    }

    render_zone_list();
    ImGui::PopTextWrapPos();
}

void GLGizmoSupportZones::on_render_input_window(float x, float y, float bottom_limit)
{
    const float f_scale = m_parent.get_scale();
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGuiWrapper::push_toolbar_style(f_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    render_panel();
    GizmoImguiEnd();
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r
