// Orca Zoukei: per plate type 3D bed textures. See BedTextures.hpp.
#include "BedTextures.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>

namespace Slic3r {
namespace GUI {
namespace Zoukei {
namespace BedTextures {

namespace {

// -1 = "any plate type" (fallback), otherwise a BedType value; -2 = unknown name.
int bed_type_from_name(std::string name)
{
    boost::trim(name);
    boost::to_lower(name);
    if (name == "*" || name == "default" || name == "default plate" || name == "all")
        return -1;
    if (name == "cool plate" || name == "smooth cool plate")
        return int(btPC);
    if (name == "engineering plate")
        return int(btEP);
    if (name == "high temp plate" || name == "smooth high temp plate" || name == "smooth pei plate")
        return int(btPEI);
    if (name == "textured pei plate")
        return int(btPTE);
    if (name == "textured cool plate")
        return int(btPCT);
    if (name == "supertack plate" || name == "cool plate (supertack)")
        return int(btSuperTack);
    return -2;
}

struct Cache
{
    std::string          key;      // edited printer preset name + its own spec option
    bool                 valid = false;
    std::map<int, Entry> by_type;  // key -1 is the fallback

    std::map<std::string, std::unique_ptr<GLTexture>> textures;
    std::set<std::string>                              failed;
};

Cache &cache()
{
    // leaked on purpose: the GL context may be gone by the time static destructors run
    static Cache *c = new Cache();
    return *c;
}

std::string find_file(const std::string &vendor, const std::string &file)
{
    boost::system::error_code ec;
    boost::filesystem::path   p(file);
    if (p.is_absolute())
        return boost::filesystem::exists(p, ec) ? p.string() : std::string();

    std::vector<std::string> candidates;
    if (!vendor.empty()) {
        candidates.push_back(Slic3r::data_dir() + "/vendor/" + vendor + "/" + file);
        candidates.push_back(Slic3r::resources_dir() + "/profiles/" + vendor + "/" + file);
    }
    candidates.push_back(Slic3r::resources_dir() + "/images/" + file);
    for (const std::string &c : candidates)
        if (boost::filesystem::exists(boost::filesystem::path(c), ec))
            return c;
    return std::string();
}

unsigned int texture_size()
{
    return unsigned(std::min<int>(OpenGLManager::get_gl_info().get_max_tex_size(), 2048));
}

} // namespace

bool is_holographic_file(const std::string &file)
{
    std::string name = boost::filesystem::path(file).filename().string();
    boost::to_lower(name);
    return name.find("holo") != std::string::npos;
}

bool parse_spec(const std::string &spec, std::vector<std::pair<std::string, std::string>> &out)
{
    out.clear();
    std::vector<std::string> pieces;
    boost::split(pieces, spec, boost::is_any_of(";"));
    for (std::string &piece : pieces) {
        const size_t eq = piece.find('=');
        if (eq == std::string::npos)
            continue;
        std::string type = piece.substr(0, eq);
        std::string file = piece.substr(eq + 1);
        boost::trim(type);
        boost::trim(file);
        if (!type.empty() && !file.empty())
            out.emplace_back(std::move(type), std::move(file));
    }
    return !out.empty();
}

bool holographic_effect_enabled()
{
    AppConfig *cfg = wxGetApp().app_config;
    return cfg == nullptr || cfg->get("bed_holographic_effect") != "0";
}

bool resolve(BedType type, Entry &out)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;

    const Preset &preset = bundle->printers.get_edited_preset();

    // Everything below only depends on the edited printer preset and its own spec option, so it is recomputed only
    // when one of those changes (this runs every frame for every plate).
    std::string opt_spec;
    if (const auto *opt = preset.config.opt<ConfigOptionString>("bed_texture_per_type"))
        opt_spec = opt->value;

    Cache            &c   = cache();
    const std::string key = preset.name + '\n' + opt_spec;
    if (!c.valid || c.key != key) {
        c.key   = key;
        c.valid = true;
        c.by_type.clear();

        // model level spec (and the vendor folder the files live in)
        std::string vendor_id;
        std::string model_spec;
        {
            const auto       *printer_model = preset.config.opt<ConfigOptionString>("printer_model");
            const std::string model_name    = printer_model != nullptr ? printer_model->value : std::string();
            if (preset.vendor != nullptr) {
                vendor_id = preset.vendor->id;
                for (const auto &pm : preset.vendor->models)
                    if (!model_name.empty() && (pm.id == model_name || pm.name == model_name)) {
                        model_spec = pm.bed_texture_per_type;
                        break;
                    }
            }
            if (model_spec.empty() && !model_name.empty()) {
                for (const auto &vp : bundle->vendors) {
                    bool found = false;
                    for (const auto &pm : vp.second.models)
                        if (pm.id == model_name || pm.name == model_name) {
                            if (vendor_id.empty())
                                vendor_id = vp.first;
                            model_spec = pm.bed_texture_per_type;
                            found      = true;
                            break;
                        }
                    if (found)
                        break;
                }
            }
        }

        const std::string &spec = opt_spec.empty() ? model_spec : opt_spec;
        std::vector<std::pair<std::string, std::string>> pairs;
        if (!spec.empty() && parse_spec(spec, pairs)) {
            for (const auto &kv : pairs) {
                const int t = bed_type_from_name(kv.first);
                if (t == -2) {
                    BOOST_LOG_TRIVIAL(warning) << "bed_texture_per_type: unknown plate type '" << kv.first << "'";
                    continue;
                }
                const std::string path = find_file(vendor_id, kv.second);
                if (path.empty()) {
                    BOOST_LOG_TRIVIAL(warning) << "bed_texture_per_type: texture not found: " << kv.second;
                    continue;
                }
                Entry e;
                e.path        = path;
                e.holographic = is_holographic_file(kv.second);
                c.by_type[t]  = std::move(e);
            }
        }
    }

    if (c.by_type.empty())
        return false;
    auto it = c.by_type.find(int(type));
    if (it == c.by_type.end())
        it = c.by_type.find(-1);
    if (it == c.by_type.end())
        return false;
    out = it->second;
    return true;
}

GLTexture *get_texture(const std::string &path)
{
    Cache &c = cache();
    if (c.failed.count(path))
        return nullptr;

    auto it = c.textures.find(path);
    if (it == c.textures.end()) {
        auto tex = std::make_unique<GLTexture>();
        bool ok  = false;
        if (boost::algorithm::iends_with(path, ".svg"))
            ok = tex->load_from_svg_file(path, true, true, true, texture_size());
        else if (boost::algorithm::iends_with(path, ".png"))
            ok = tex->load_from_file(path, true, GLTexture::MultiThreaded, true);
        if (!ok) {
            BOOST_LOG_TRIVIAL(warning) << "bed_texture_per_type: unable to load " << path;
            c.failed.insert(path);
            return nullptr;
        }
        it = c.textures.emplace(path, std::move(tex)).first;
    }

    GLTexture *tex = it->second.get();
    if (tex->unsent_compressed_data_available())
        tex->send_compressed_data_to_gpu();
    if (tex->get_id() == 0) {
        // compression still running: keep the default texture for now and ask for another frame
        if (Plater *plater = wxGetApp().plater())
            if (GLCanvas3D *canvas = plater->get_view3D_canvas3D())
                canvas->request_extra_frame();
        return nullptr;
    }
    return tex;
}

void reset()
{
    Cache &c = cache();
    c.valid  = false;
    c.by_type.clear();
    c.textures.clear();
    c.failed.clear();
}

} // namespace BedTextures
} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
