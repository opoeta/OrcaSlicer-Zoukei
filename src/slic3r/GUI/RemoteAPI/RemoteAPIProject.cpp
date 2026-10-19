// src/slic3r/GUI/RemoteAPI/RemoteAPIProject.cpp
//
// Orca Zoukei Remote API, group G2: project and model operations. Every handler runs on the UI thread
// (run_on_ui), validates its input and takes an undo snapshot where the equivalent UI action does.
// Destructive project operations refuse (409 unsaved_changes) unless "discard_changes": true; file
// writes only go to the path the caller passes and never overwrite without "overwrite": true.
//
//   GET    /api/v1/project                       name/path, dirty flag, object and plate counts
//   POST   /api/v1/project/new                   {discard_changes?}
//   POST   /api/v1/project/open                  {path: x.3mf, discard_changes?}
//   POST   /api/v1/project/save                  {path?: x.3mf, overwrite?}  (no path = the project's own file)
//   POST   /api/v1/project/import                {paths: [stl|obj|3mf|step...]}  add models to the plate
//   POST   /api/v1/project/export                {format: 3mf|stl|obj, path, overwrite?, objects?: [ids], plate?}
//   POST   /api/v1/export/glb                    {path: x.glb, embed_html?, html?: none|linked|embedded, travel?, detail?: preview|standard|full, overwrite?}
//                                                the sliced toolpaths shown in the Preview as GLB (+ a self-contained three.js viewer .html next to it)
//   POST   /api/v1/project/undo | redo           {steps?}
//   PUT    /api/v1/selection                     {objects: [ids], volume?}  (empty list = deselect)
//   PUT    /api/v1/objects/{id}                  {name}  rename
//   GET    /api/v1/objects/{id}/volumes          parts / modifiers / negative volumes / support volumes
//   POST   /api/v1/objects/{id}/volumes          add: {type: part|modifier|negative, shape+size | file, position?, name?}
//   PUT    /api/v1/objects/{id}/volumes/{i}      {name?, type?, config?: {key: value|null}}
//   DELETE /api/v1/objects/{id}/volumes/{i}
//   POST   /api/v1/objects/{id}/split            {mode: objects|parts, volume?}
//   POST   /api/v1/objects/{id}/copy             {copies?}  independent copies (new objects)
//   POST   /api/v1/objects/{id}/mirror           {axis: x|y|z}
//   POST   /api/v1/objects/{id}/scale_to_fit     {margin?, allow_upscale?, center?}
//   POST   /api/v1/objects/{id}/drop             drop to the bed
//   POST   /api/v1/objects/{id}/cut              {z, keep?: both|upper|lower, place_on_cut_upper?}
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Zoukei/ToolpathGlbExport.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <cmath>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

namespace {

bool project_dirty(Plater *p)
{
    return !p->up_to_date(false, false) || p->is_project_dirty() || wxGetApp().has_unsaved_preset_changes();
}

json project_info()
{
    Plater *p = wxGetApp().plater();
    PartPlateList &plates = p->get_partplate_list();
    const wxString fn = p->get_project_filename(".3mf");
    return {{"path", std::string(fn.ToUTF8().data())},
            {"dirty", project_dirty(p)},
            {"unsaved_preset_changes", wxGetApp().has_unsaved_preset_changes()},
            {"objects", p->model().objects.size()},
            {"plates", plates.get_plate_count()},
            {"current_plate", plates.get_curr_plate_index()}};
}

boost::filesystem::path to_fs(const std::string &utf8)
{
    return boost::filesystem::path(boost::nowide::widen(utf8));
}

// Checks a caller-supplied output path: right extension, existing directory, no silent overwrite.
void check_output_path(const std::string &path, const char *ext, bool overwrite)
{
    if (path.empty())
        zk::fail("missing_path");
    if (!zk::has_ext(path, ext))
        zk::fail("bad_extension", std::string("path must end with ") + ext);
    const boost::filesystem::path p = to_fs(path);
    boost::system::error_code ec;
    if (!p.has_parent_path() || !boost::filesystem::is_directory(p.parent_path(), ec))
        zk::fail("no_directory", "the folder of path must exist");
    if (boost::filesystem::exists(p, ec) && !overwrite)
        zk::fail("exists", "the file exists; pass overwrite:true to replace it");
}

json objects_brief(const Model &model, size_t from)
{
    json arr = json::array();
    for (size_t i = from; i < model.objects.size(); ++i)
        arr.push_back({{"index", i}, {"id", (uint64_t) model.objects[i]->id().id}, {"name", model.objects[i]->name}});
    return arr;
}

// Merged positive geometry of an object in world coordinates (all instances, or only those on `plate`).
void merge_object_mesh(const ModelObject &mo, int obj_idx, int plate, TriangleMesh &out)
{
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    for (size_t ii = 0; ii < mo.instances.size(); ++ii) {
        if (plate >= 0) {
            PartPlate *pp = plates.get_plate(plate);
            if (pp == nullptr || !pp->contain_instance(obj_idx, int(ii)))
                continue;
        }
        for (const ModelVolume *v : mo.volumes) {
            if (!v->is_model_part())
                continue;
            TriangleMesh m(v->mesh());
            m.transform(mo.instances[ii]->get_matrix() * v->get_matrix(), true);
            out.merge(m);
        }
    }
}

size_t count_model_parts(const ModelObject &mo)
{
    size_t n = 0;
    for (const ModelVolume *v : mo.volumes)
        if (v->is_model_part())
            ++n;
    return n;
}

// ---- individual operations (GUI thread) ------------------------------------------------------

json op_new(const json &body)
{
    Plater *p = wxGetApp().plater();
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for or cancel the running slice first");
    if (project_dirty(p) && !zk::opt_bool(body, "discard_changes", false))
        zk::fail("unsaved_changes", "the project has unsaved changes; save it first or pass discard_changes:true");
    p->new_project(true, false);
    zk::notify("New project");
    json r = project_info();
    r["created"] = true;
    return r;
}

json op_open(const json &body)
{
    Plater *p = wxGetApp().plater();
    const std::string path = zk::need_str(body, "path");
    if (!zk::has_ext(path, ".3mf"))
        zk::fail("unsupported_format", "open takes a .3mf project (use import for stl/obj/step)");
    boost::system::error_code ec;
    if (!boost::filesystem::exists(to_fs(path), ec))
        zk::fail("not_found", path);
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for or cancel the running slice first");
    const bool dirty = project_dirty(p);
    if (dirty && !zk::opt_bool(body, "discard_changes", false))
        zk::fail("unsaved_changes", "the project has unsaved changes; save it first or pass discard_changes:true");
    // A dirty project (or modified presets) would make Plater::load_project ask a question that nothing
    // can answer from here: reset to a clean project first, which is what discard_changes means.
    if (dirty)
        p->new_project(true, true);
    p->load_project(from_u8(path), "<loadall>");
    zk::notify("Opened " + to_fs(path).filename().string());
    json r = project_info();
    r["opened"] = true;
    return r;
}

json op_save(const json &body)
{
    Plater *p = wxGetApp().plater();
    const std::string given = zk::opt_str(body, "path");
    wxString          target;
    if (!given.empty())
        target = from_u8(given);
    else
        target = p->get_project_filename(".3mf");
    if (target.IsEmpty())
        zk::fail("missing_path", "the project has no file yet: pass path (.3mf)");
    const std::string target_u8 = std::string(target.ToUTF8().data());
    check_output_path(target_u8, ".3mf", zk::opt_bool(body, "overwrite", false));
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for or cancel the running slice first");

    // Same layout as Plater::save_project (without its file dialog).
    SaveStrategy strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    if (wxGetApp().app_config->get_bool("export_sources_full_pathnames"))
        strategy = strategy | SaveStrategy::FullPathSources;
    if (p->export_3mf(into_path(target), strategy) < 0)
        zk::fail("save_failed", "could not write the file (is it open in another program?)");
    Slic3r::remove_backup(p->model(), false);
    p->set_project_filename(target);
    p->up_to_date(true, false);
    p->up_to_date(true, true);
    wxGetApp().update_saved_preset_from_current_preset();
    p->reset_project_dirty_after_save();
    zk::notify("Saved " + to_fs(target_u8).filename().string());
    json r = project_info();
    r["saved"] = true;
    r["file"]  = target_u8;
    return r;
}

json op_import(const json &body)
{
    Plater *p = wxGetApp().plater();
    if (!body.contains("paths") || !body["paths"].is_array() || body["paths"].empty())
        zk::fail("missing_paths", "paths: [\"C:/a.stl\", ...]");
    json results = json::array();
    AppConfig *cfg = wxGetApp().app_config;
    for (const json &pj : body["paths"]) {
        if (!pj.is_string())
            zk::fail("bad_value", "paths must be strings");
        const std::string path = pj.get<std::string>();
        const std::string ext  = zk::ext_of(path);
        const bool is_step     = ext == ".step" || ext == ".stp";
        if (!(ext == ".stl" || ext == ".obj" || ext == ".3mf" || is_step)) {
            results.push_back({{"path", path}, {"error", "unsupported_format"}});
            continue;
        }
        boost::system::error_code ec;
        if (!boost::filesystem::exists(to_fs(path), ec)) {
            results.push_back({{"path", path}, {"error", "not_found"}});
            continue;
        }
        // Use the stored STEP tessellation defaults instead of the dialog; restore the flags after.
        const bool prev_mesh_dlg  = cfg->get_bool("enable_step_mesh_setting");
        const bool prev_utf8_warn = cfg->get_bool("step_not_utf8_no_warn");
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", false);
            cfg->set_bool("step_not_utf8_no_warn", true);
        }
        std::vector<size_t> idxs = p->load_files(std::vector<boost::filesystem::path>{to_fs(path)}, LoadStrategy::LoadModel);
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", prev_mesh_dlg);
            cfg->set_bool("step_not_utf8_no_warn", prev_utf8_warn);
        }
        if (idxs.empty()) {
            results.push_back({{"path", path}, {"error", "load_failed"}});
            continue;
        }
        json objs = json::array();
        for (size_t i : idxs) {
            const ModelObject *mo = p->model().objects[i];
            const Vec3d sz = mo->bounding_box_exact().size();
            objs.push_back({{"index", i}, {"id", (uint64_t) mo->id().id}, {"name", mo->name}, {"size_mm", {sz.x(), sz.y(), sz.z()}}});
        }
        results.push_back({{"path", path}, {"objects", objs}});
    }
    zk::notify("Imported " + std::to_string(results.size()) + " file(s)");
    return {{"imported", results}, {"object_count", p->model().objects.size()}};
}

json op_export(const json &body)
{
    Plater *p = wxGetApp().plater();
    const std::string format = zk::lower(zk::need_str(body, "format"));
    const std::string path   = zk::need_str(body, "path");
    if (format != "3mf" && format != "stl" && format != "obj")
        zk::fail("bad_format", "3mf | stl | obj");
    const std::string ext = "." + format;
    check_output_path(path, ext.c_str(), zk::opt_bool(body, "overwrite", false));
    if (p->model().objects.empty())
        zk::fail("empty", "nothing to export: the plate has no objects");
    if (format == "3mf") {
        if (!p->export_3mf_copy(to_fs(path)))
            zk::fail("export_failed", "could not write the 3mf");
        return {{"exported", true}, {"format", "3mf"}, {"file", path}};
    }
    // stl / obj: the positive geometry of the chosen objects, in world coordinates, as one mesh.
    int plate = -1;
    if (body.contains("plate") && !body["plate"].is_null()) {
        plate = int(zk::need_num(body, "plate"));
        if (plate < 0 || plate >= p->get_partplate_list().get_plate_count())
            zk::fail("unknown_plate");
    }
    std::vector<uint64_t> wanted;
    if (body.contains("objects") && body["objects"].is_array())
        for (const json &o : body["objects"]) {
            if (!o.is_number_integer())
                zk::fail("bad_value", "objects must be object ids");
            wanted.push_back((uint64_t) o.get<long long>());
        }
    TriangleMesh mesh;
    size_t       used = 0;
    for (size_t i = 0; i < p->model().objects.size(); ++i) {
        const ModelObject *mo = p->model().objects[i];
        if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), (uint64_t) mo->id().id) == wanted.end())
            continue;
        const size_t before = mesh.its.indices.size();
        merge_object_mesh(*mo, int(i), plate, mesh);
        if (mesh.its.indices.size() > before)
            ++used;
    }
    for (uint64_t w : wanted)
        if (zk::find_object(p->model(), w) < 0)
            zk::fail("unknown_object", std::to_string(w));
    if (mesh.empty())
        zk::fail("empty", "the selection has no printable geometry");
    const bool ok = format == "stl" ? store_stl(path.c_str(), &mesh, true) : store_obj(path.c_str(), &mesh);
    if (!ok)
        zk::fail("export_failed", "could not write the file");
    return {{"exported", true}, {"format", format}, {"file", path}, {"objects", used}, {"triangles", mesh.its.indices.size()}};
}

// Sliced toolpaths -> GLB (+ viewer html). Writes only `path` and its .html twin; no overwrite unless overwrite:true.
json op_export_glb(const json &body)
{
    Plater *p = wxGetApp().plater();
    const std::string path = zk::need_str(body, "path");
    Zoukei::GlbExportOptions opt;
    opt.overwrite = zk::opt_bool(body, "overwrite", false);
    opt.travel    = zk::opt_bool(body, "travel", false);
    if (!Zoukei::glb_detail_from_string(zk::opt_str(body, "detail", "standard"), opt.detail))
        zk::fail("bad_value", "detail: preview | standard | full");
    if (zk::opt_bool(body, "embed_html", false))
        opt.html = Zoukei::GlbHtml::Embedded;
    if (body.contains("html") && body["html"].is_string() && !Zoukei::glb_html_from_string(body["html"].get<std::string>(), opt.html))
        zk::fail("bad_value", "html: none | linked | embedded");
    if (p->is_background_process_slicing())
        zk::fail("slicing", "wait for or cancel the running slice first");
    const Zoukei::GlbExportResult r = Zoukei::export_toolpaths_glb(path, opt);
    if (!r.ok)
        zk::fail(r.error_code.c_str(), r.error);
    json out = {{"exported", true}, {"format", "glb"}, {"file", r.glb_path}, {"bytes", r.glb_bytes}, {"triangles", r.triangles},
                {"roles", r.roles}, {"materials", r.materials}, {"detail", Zoukei::glb_detail_name(opt.detail)}};
    if (!r.html_path.empty()) {
        out["html"]       = r.html_path;
        out["html_bytes"] = r.html_bytes;
    }
    return out;
}

json op_undo_redo(const json &body, bool undo)
{
    Plater *p = wxGetApp().plater();
    const int steps = int(zk::opt_num(body, "steps", 1));
    if (steps < 1 || steps > 50)
        zk::fail("bad_steps", "1..50");
    int done = 0;
    for (int i = 0; i < steps; ++i) {
        if (undo ? !p->can_undo() : !p->can_redo())
            break;
        if (undo) p->undo(); else p->redo();
        ++done;
    }
    if (done == 0)
        zk::fail("nothing_to_do", undo ? "no undo step available (the 3D editor tab must be shown)" : "no redo step available (the 3D editor tab must be shown)");
    return {{undo ? "undone" : "redone", done}, {"can_undo", p->can_undo()}, {"can_redo", p->can_redo()},
            {"objects", p->model().objects.size()}};
}

json op_select(const json &body)
{
    Plater *p = wxGetApp().plater();
    ObjectList *ol = wxGetApp().obj_list();
    if (ol == nullptr)
        zk::fail("no_object_list");
    if (!body.contains("objects") || !body["objects"].is_array())
        zk::fail("missing_objects", "objects: [ids] (empty = deselect)");
    if (body["objects"].empty()) {
        p->deselect_all();
        return {{"selected", json::array()}};
    }
    std::vector<ObjectVolumeID> ids;
    json                         sel = json::array();
    for (const json &o : body["objects"]) {
        if (!o.is_number_integer())
            zk::fail("bad_value", "objects must be object ids");
        int idx = -1;
        ModelObject *mo = zk::need_object((uint64_t) o.get<long long>(), idx);
        ids.push_back(ObjectVolumeID{mo, nullptr});
        sel.push_back(o);
    }
    if (body.contains("volume") && !body["volume"].is_null()) {
        if (ids.size() != 1 || !body["volume"].is_number_integer())
            zk::fail("bad_value", "volume needs exactly one object and an integer index");
        ids[0].volume = zk::need_volume(*ids[0].object, (long) body["volume"].get<long long>());
    }
    ol->select_items(ids);
    return {{"selected", sel}};
}

json op_rename(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    const std::string name = zk::need_str(body, "name");
    if (name.empty() || name.size() > 200)
        zk::fail("bad_name", "1..200 characters");
    p->take_snapshot("Rename object (remote)");
    mo->name = name;
    if (ObjectList *ol = wxGetApp().obj_list())
        if (ObjectDataViewModel *om = ol->GetModel()) {
            wxDataViewItem item = om->GetItemById(idx);
            if (item.IsOk())
                om->SetName(from_u8(name), item);
        }
    p->set_plater_dirty(true);
    return {{"id", id}, {"name", name}};
}

json op_volume_set(uint64_t id, long vol, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    ModelVolume *mv = zk::need_volume(*mo, vol);
    if (!body.is_object())
        zk::fail("body_must_be_object");
    ModelVolumeType nt = mv->type();
    const bool has_type = body.contains("type") && !body["type"].is_null();
    if (has_type) {
        if (!zk::volume_type_from_name(zk::need_str(body, "type"), nt))
            zk::fail("bad_type", "part | modifier | negative | support_enforcer | support_blocker");
        if (mv->is_model_part() && nt != ModelVolumeType::MODEL_PART && count_model_parts(*mo) < 2)
            zk::fail("last_solid_part", "an object needs at least one model part");
    }
    json cfg_result = json::object();
    if (body.contains("config")) {
        // Validated and applied atomically before anything else changes.
        p->take_snapshot("Edit volume (remote)");
        cfg_result = zk::apply_model_config(mv->config, body["config"]);
        if (!cfg_result["errors"].empty())
            return json{{"error", "config_rejected"}, {"errors", cfg_result["errors"]}};
    } else {
        p->take_snapshot("Edit volume (remote)");
    }
    if (body.contains("name")) {
        const std::string name = zk::need_str(body, "name");
        if (name.empty() || name.size() > 200)
            zk::fail("bad_name", "1..200 characters");
        mv->name = name;
    }
    ModelVolume *keep = mv;
    if (has_type && nt != mv->type()) {
        mv->set_type(nt);
        mo->sort_volumes(true);
    }
    zk::refresh_object(idx, nt == ModelVolumeType::MODEL_PART || has_type);
    int new_index = -1;
    for (size_t i = 0; i < mo->volumes.size(); ++i)
        if (mo->volumes[i] == keep)
            new_index = int(i);
    return {{"id", id}, {"volume", new_index}, {"config", cfg_result}, {"volumes", zk::volumes_json(*mo)}};
}

json op_volume_delete(uint64_t id, long vol)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    ModelVolume *mv = zk::need_volume(*mo, vol);
    if (mv->is_model_part() && count_model_parts(*mo) < 2)
        zk::fail("last_solid_part", "deleting the last solid part is not allowed");
    if (mo->is_cut())
        zk::fail("cut_object", "parts of a cut object cannot be deleted here");
    p->take_snapshot("Delete part (remote)");
    const bool was_part = mv->is_model_part();
    mo->delete_volume(size_t(vol));
    zk::refresh_object(idx, was_part);
    zk::notify("Deleted a volume of '" + mo->name + "'");
    return {{"id", id}, {"deleted", true}, {"volumes", zk::volumes_json(*mo)}};
}

json op_split(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    const std::string mode = zk::lower(zk::opt_str(body, "mode", "objects"));
    if (mode == "objects") {
        size_t parts = 0;
        bool   splittable = false;
        for (const ModelVolume *v : mo->volumes)
            if (v->is_model_part()) {
                ++parts;
                if (v->is_splittable())
                    splittable = true;
            }
        if (parts < 2 && !splittable)
            zk::fail("not_splittable", "the object is a single connected part");
        const size_t before = p->model().objects.size();
        // Plater::split_object asks "disable auto-drop?" when parts float; with auto_drop already off it
        // keeps the Z positions without asking (the dialog's Yes answer).
        const bool prev_auto_drop = mo->auto_drop;
        mo->auto_drop = false;
        // Plater::split_object(int, bool) is declared but not defined in this tree: select the object and use
        // the selection-based overload.
        if (ObjectList *ol = wxGetApp().obj_list())
            ol->select_item(ObjectVolumeID{mo, nullptr});
        p->split_object(false);
        const size_t after = p->model().objects.size();
        if (after == before) { // the engine refused (the object is still there): put the flag back
            mo->auto_drop = prev_auto_drop;
            zk::fail("split_failed", "the object could not be split");
        }
        // The old object was removed and the pieces appended at the end.
        for (size_t i = before > 0 ? before - 1 : 0; i < after; ++i)
            p->model().objects[i]->auto_drop = prev_auto_drop;
        zk::notify("Split object into " + std::to_string(after - before + 1) + " objects");
        return {{"split", "objects"}, {"objects", objects_brief(p->model(), 0)}, {"object_count", after}};
    }
    if (mode == "parts") {
        long vi = -1;
        if (body.contains("volume") && !body["volume"].is_null()) {
            if (!body["volume"].is_number_integer())
                zk::fail("bad_value", "volume must be an integer index");
            vi = (long) body["volume"].get<long long>();
        } else {
            for (size_t i = 0; i < mo->volumes.size(); ++i)
                if (mo->volumes[i]->is_model_part()) {
                    if (vi >= 0)
                        zk::fail("volume_required", "the object has several parts: pass volume");
                    vi = long(i);
                }
        }
        ModelVolume *mv = zk::need_volume(*mo, vi);
        if (!mv->is_splittable())
            zk::fail("not_splittable", "the part is a single connected shell");
        p->take_snapshot("Split to parts (remote)");
        const size_t cnt = std::max<size_t>(1, wxGetApp().preset_bundle->filament_presets.size());
        mv->split((unsigned int) cnt, wxGetApp().app_config->get_bool("keep_painting"));
        mo->input_file.clear();
        zk::refresh_object(idx, true);
        zk::notify("Split a part of '" + mo->name + "'");
        return {{"split", "parts"}, {"id", id}, {"volumes", zk::volumes_json(*mo)}};
    }
    zk::fail("bad_mode", "objects | parts");
}

json op_copy(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    const int copies = int(zk::opt_num(body, "copies", 1));
    if (copies < 1 || copies > 20)
        zk::fail("bad_copies", "1..20");
    ObjectList *ol = wxGetApp().obj_list();
    if (ol == nullptr)
        zk::fail("no_object_list");
    ol->select_item(ObjectVolumeID{mo, nullptr});
    if (!p->can_copy_to_clipboard())
        zk::fail("copy_failed", "copy needs the 3D editor tab; for another instance of the same object use POST /objects/{id}/duplicate");
    p->copy_selection_to_clipboard();
    if (!p->can_paste_from_clipboard())
        zk::fail("copy_failed", "nothing was copied");
    const size_t before = p->model().objects.size();
    for (int i = 0; i < copies; ++i)
        p->paste_from_clipboard();
    if (p->model().objects.size() == before)
        zk::fail("copy_failed", "the 3D editor tab must be shown for copy/paste; for another instance of the same object use POST /objects/{id}/duplicate");
    zk::notify("Copied '" + mo->name + "'");
    return {{"copied", true}, {"new_objects", objects_brief(p->model(), before)}, {"object_count", p->model().objects.size()}};
}

json op_mirror(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    const std::string axis = zk::lower(zk::need_str(body, "axis"));
    if (axis != "x" && axis != "y" && axis != "z")
        zk::fail("bad_axis", "x | y | z");
    const int ax = axis == "x" ? 0 : (axis == "y" ? 1 : 2);
    p->take_snapshot("Mirror (remote)");
    for (ModelInstance *inst : mo->instances) {
        Vec3d m = inst->get_mirror();
        m[ax] = -m[ax];
        inst->set_mirror(m);
    }
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    p->get_partplate_list().notify_instance_update(idx, 0, true);
    p->changed_object(idx);
    if (ObjectList *ol = wxGetApp().obj_list())
        ol->update_plate_values_for_items();
    zk::notify("Mirrored '" + mo->name + "' along " + axis);
    return {{"id", id}, {"mirrored", axis}};
}

json op_scale_to_fit(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    if (mo->instances.empty())
        zk::fail("no_instance");
    const double margin = zk::opt_num(body, "margin", 0.0);
    if (margin < 0.0 || margin > 100.0)
        zk::fail("bad_margin", "0..100 mm");
    const bool allow_up = zk::opt_bool(body, "allow_upscale", false);
    const bool center   = zk::opt_bool(body, "center", false);
    PartPlateList &plates = p->get_partplate_list();
    int pi = plates.find_instance(idx, 0);
    if (pi < 0)
        pi = plates.get_curr_plate_index();
    PartPlate *plate = plates.get_plate(pi);
    if (plate == nullptr)
        zk::fail("unknown_plate");
    const BoundingBoxf3 bv    = plate->get_build_volume();
    const Vec3d         avail = bv.size() - Vec3d(2 * margin, 2 * margin, margin);
    const BoundingBoxf3 ob    = mo->instance_bounding_box(0);
    const Vec3d         size  = ob.size();
    if (size.minCoeff() <= 1e-6 || avail.minCoeff() <= 0.0)
        zk::fail("degenerate", "object or plate has no usable size");
    double f = std::min({avail.x() / size.x(), avail.y() / size.y(), avail.z() / size.z()});
    if (!allow_up && f >= 1.0)
        return {{"id", id}, {"changed", false}, {"detail", "already fits"}, {"size_mm", {size.x(), size.y(), size.z()}}};
    p->take_snapshot("Scale to fit (remote)");
    const Vec3d old_center = ob.center();
    ModelInstance *inst = mo->instances.front();
    inst->set_scaling_factor(inst->get_scaling_factor() * f);
    mo->invalidate_bounding_box();
    const BoundingBoxf3 nb = mo->instance_bounding_box(0);
    Vec3d delta = (center ? Vec3d(bv.center().x(), bv.center().y(), 0.0) : Vec3d(old_center.x(), old_center.y(), 0.0)) -
                  Vec3d(nb.center().x(), nb.center().y(), 0.0);
    inst->set_offset(inst->get_offset() + delta);
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    plates.notify_instance_update(idx, 0, true);
    p->changed_object(idx);
    if (ObjectList *ol = wxGetApp().obj_list())
        ol->update_plate_values_for_items();
    const Vec3d ns = mo->instance_bounding_box(0).size();
    zk::notify("Scaled '" + mo->name + "' to fit");
    return {{"id", id}, {"changed", true}, {"factor", f}, {"size_mm", {ns.x(), ns.y(), ns.z()}}};
}

json op_drop(uint64_t id)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    p->take_snapshot("Drop to bed (remote)");
    mo->ensure_on_bed();
    p->get_partplate_list().notify_instance_update(idx, 0, true);
    p->changed_object(idx);
    if (ObjectList *ol = wxGetApp().obj_list())
        ol->update_plate_values_for_items();
    zk::notify("Dropped '" + mo->name + "' to the bed");
    return {{"id", id}, {"min_z", mo->instance_bounding_box(0).min.z()}};
}

json op_cut(uint64_t id, const json &body)
{
    Plater *p = wxGetApp().plater();
    int idx = -1;
    ModelObject *mo = zk::need_object(id, idx);
    if (mo->instances.empty())
        zk::fail("no_instance");
    if (mo->is_cut())
        zk::fail("cut_object", "this object was already cut (cut connectors); use the cut tool in the window");
    const double z = zk::need_num(body, "z");
    const std::string keep = zk::lower(zk::opt_str(body, "keep", "both"));
    if (keep != "both" && keep != "upper" && keep != "lower")
        zk::fail("bad_keep", "both | upper | lower");
    const BoundingBoxf3 bb = mo->instance_bounding_box(0);
    if (!(z > bb.min.z() + 1e-3 && z < bb.max.z() - 1e-3))
        zk::fail("z_out_of_range", "z must be inside the object: " + std::to_string(bb.min.z()) + " .. " + std::to_string(bb.max.z()));
    // enum_bitmask::operator|= is broken in libslic3r (never instantiated elsewhere): build with operator|.
    ModelObjectCutAttributes attrs;
    if (keep != "lower")
        attrs = attrs | ModelObjectCutAttribute::KeepUpper;
    if (keep != "upper")
        attrs = attrs | ModelObjectCutAttribute::KeepLower;
    if (keep != "lower" && zk::opt_bool(body, "place_on_cut_upper", true))
        attrs = attrs | ModelObjectCutAttribute::PlaceOnCutUpper;
    p->take_snapshot("Cut (remote)");
    const Vec3d offset = mo->instances.front()->get_offset();
    Cut cut(mo, 0, Geometry::translation_transform(z * Vec3d::UnitZ() - offset), attrs);
    const ModelObjectPtrs &new_objects = cut.perform_with_plane();
    if (new_objects.empty())
        zk::fail("cut_failed", "the cut produced no object");
    p->apply_cut_object_to_model(size_t(idx), new_objects);
    zk::notify("Cut at z=" + std::to_string(z));
    return {{"cut", true}, {"z", z}, {"objects", objects_brief(p->model(), 0)}, {"object_count", p->model().objects.size()}};
}

} // namespace

bool Controller::dispatch_project(const Request &req, Response &out)
{
    static const std::string base = "/api/v1/";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::vector<std::string> parts = zk::split_path(path.substr(base.size()));
    if (parts.empty())
        return false;
    const std::string &m = req.method;

    // ---- toolpath export (GLB) ---------------------------------------------------------------
    if (parts[0] == "export" && parts.size() == 2 && parts[1] == "glb" && m == "POST") {
        json body = json::object();
        if (!zk::parse_body(req, body)) {
            out = {400, {{"error", "invalid_json"}}};
            return true;
        }
        out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_export_glb(body); }); }, 600));
        return true;
    }

    // ---- project / selection ----------------------------------------------------------------
    if (parts[0] == "project" || parts[0] == "selection") {
        json body = json::object();
        const bool wants_body = m != "GET";
        if (wants_body && !zk::parse_body(req, body)) {
            out = {400, {{"error", "invalid_json"}}};
            return true;
        }
        if (parts[0] == "selection" && parts.size() == 1 && m == "PUT") {
            out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_select(body); }); }, 30));
            return true;
        }
        if (parts[0] != "project")
            return false;
        if (parts.size() == 1 && m == "GET") {
            out = zk::reply(run_on_ui([]() -> json { return project_info(); }, 30));
            return true;
        }
        if (parts.size() == 2 && m == "POST") {
            const std::string &act = parts[1];
            if (act == "new") {
                out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_new(body); }); }, 120));
                return true;
            }
            if (act == "open") {
                out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_open(body); }); }, 300));
                return true;
            }
            if (act == "save") {
                out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_save(body); }); }, 300));
                return true;
            }
            if (act == "import") {
                out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_import(body); }); }, 300));
                return true;
            }
            if (act == "export") {
                out = zk::reply(run_on_ui([body]() -> json { return zk::guard([&]() -> json { return op_export(body); }); }, 300));
                return true;
            }
            if (act == "undo" || act == "redo") {
                const bool undo = act == "undo";
                out = zk::reply(run_on_ui([body, undo]() -> json { return zk::guard([&]() -> json { return op_undo_redo(body, undo); }); }, 60));
                return true;
            }
        }
        return false;
    }

    // ---- objects ------------------------------------------------------------------------------
    if (parts[0] != "objects" || parts.size() < 2)
        return false;
    uint64_t id = 0;
    if (!zk::parse_uint(parts[1], id))
        return false; // the controller's own routes report bad_object_id
    const std::string act = parts.size() > 2 ? parts[2] : std::string();

    enum class Op { None, Rename, VolList, VolAdd, VolSet, VolDel, Split, Copy, Mirror, Scale, Drop, Cut };
    Op   op   = Op::None;
    long vol  = -1;
    if (parts.size() == 2 && m == "PUT") op = Op::Rename;
    else if (parts.size() == 3 && act == "volumes" && m == "GET") op = Op::VolList;
    else if (parts.size() == 3 && act == "volumes" && m == "POST") op = Op::VolAdd;
    else if (parts.size() == 4 && act == "volumes" && (m == "PUT" || m == "DELETE")) {
        uint64_t vi = 0;
        if (!zk::parse_uint(parts[3], vi)) {
            out = {400, {{"error", "bad_volume_index"}}};
            return true;
        }
        vol = (long) vi;
        op  = m == "PUT" ? Op::VolSet : Op::VolDel;
    } else if (parts.size() == 3 && m == "POST") {
        if (act == "split") op = Op::Split;
        else if (act == "copy") op = Op::Copy;
        else if (act == "mirror") op = Op::Mirror;
        else if (act == "scale_to_fit") op = Op::Scale;
        else if (act == "drop") op = Op::Drop;
        else if (act == "cut") op = Op::Cut;
    }
    if (op == Op::None)
        return false;

    json body = json::object();
    if (m != "GET" && m != "DELETE" && !zk::parse_body(req, body)) {
        out = {400, {{"error", "invalid_json"}}};
        return true;
    }
    out = zk::reply(run_on_ui([op, id, vol, body]() -> json {
        return zk::guard([&]() -> json {
            switch (op) {
            case Op::Rename:  return op_rename(id, body);
            case Op::VolList: {
                int idx = -1;
                ModelObject *mo = zk::need_object(id, idx);
                return {{"id", id}, {"name", mo->name}, {"volumes", zk::volumes_json(*mo)}};
            }
            case Op::VolAdd:  return zk::add_volume(id, body, std::string());
            case Op::VolSet:  return op_volume_set(id, vol, body);
            case Op::VolDel:  return op_volume_delete(id, vol);
            case Op::Split:   return op_split(id, body);
            case Op::Copy:    return op_copy(id, body);
            case Op::Mirror:  return op_mirror(id, body);
            case Op::Scale:   return op_scale_to_fit(id, body);
            case Op::Drop:    return op_drop(id);
            case Op::Cut:     return op_cut(id, body);
            default:          return {{"error", "not_found"}};
            }
        });
    }, 120));
    if (op == Op::VolAdd && out.status == 200)
        out.status = 201;
    return true;
}

}}} // namespace
