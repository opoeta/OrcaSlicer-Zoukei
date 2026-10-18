// src/slic3r/GUI/RemoteAPI/RemoteAPIPaint.cpp
//
// Orca Zoukei Remote API, group G3: facet painting by rules (colour / MMU, seam, fuzzy skin and, for
// completeness, support). Every handler runs on the UI thread (run_on_ui) and takes an undo snapshot.
//
//   POST /api/v1/objects/{id}/paint        {kind: color|seam|fuzzy_skin|support,
//                                           state: <filament slot> | enforcer | blocker | "on",
//                                           volume?: i | volumes?: [i...],       (default: every model part)
//                                           rule: {...}}
//   POST /api/v1/objects/{id}/paint/clear  {kind, volume?, volumes?, rule?}       (no rule = clear everything)
//   GET  /api/v1/objects/{id}/paint        ?kind=color|seam|fuzzy_skin|support    summary of the painted state
//
// Rule (criteria combine with AND; world coordinates, Z = 0 at the bed):
//   {fill: true}                                  every facet
//   {z_min, z_max}                                height band
//   {normal: [x,y,z], normal_angle: deg}          facets facing a direction (e.g. up = [0,0,1])
//   {overhang_angle: deg}                         facets leaning down by more than deg from vertical
//   {box_min: [x,y,z], box_max: [x,y,z]}          region
//   coverage: "centroid" (default) | "any" | "all"   how a triangle is tested against z / box ranges
// Painting works on the mesh's original triangles (no re-triangulation at the band edges).
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

bool Controller::dispatch_paint(const Request &req, Response &out)
{
    static const std::string base = "/api/v1/objects/";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::vector<std::string> parts = zk::split_path(path.substr(base.size()));
    if (parts.size() < 2 || parts[1] != "paint" || parts.size() > 3)
        return false;
    if (parts.size() == 3 && parts[2] != "clear")
        return false;
    uint64_t id = 0;
    if (!zk::parse_uint(parts[0], id)) {
        out = {400, {{"error", "bad_object_id"}}};
        return true;
    }

    if (req.method == "GET" && parts.size() == 2) {
        const std::string kind_s = zk::query_value(req, "kind");
        out = zk::reply(run_on_ui([id, kind_s]() -> json {
            return zk::guard([&]() -> json {
                std::vector<zk::PaintKind> kinds;
                if (!kind_s.empty()) {
                    zk::PaintKind k;
                    if (!zk::paint_kind_from_name(kind_s, k))
                        zk::fail("bad_kind", "color | seam | fuzzy_skin | support");
                    kinds.push_back(k);
                }
                return zk::paint_summary(id, kinds);
            });
        }, 60));
        return true;
    }
    if (req.method == "POST") {
        json body;
        if (!zk::parse_body(req, body)) {
            out = {400, {{"error", "invalid_json"}}};
            return true;
        }
        const bool clear_route = parts.size() == 3;
        out = zk::reply(run_on_ui([id, body, clear_route]() -> json {
            return zk::guard([&]() -> json {
                zk::PaintKind kind;
                if (!zk::paint_kind_from_name(zk::need_str(body, "kind"), kind))
                    zk::fail("bad_kind", "color | seam | fuzzy_skin | support");
                return zk::paint_apply(kind, id, body, clear_route);
            });
        }, 120));
        return true;
    }
    return false;
}

}}} // namespace
