// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0. See SurfaceEffectProfile.hpp.
#include "SurfaceEffectProfile.hpp"

#include "ColorStitch.hpp"
#include "Config.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <exception>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

SurfaceEffectProfileManager& SurfaceEffectProfileManager::get()
{
    static SurfaceEffectProfileManager s_instance;
    return s_instance;
}

int SurfaceEffectProfileManager::add(SurfaceEffectProfile profile)
{
    profile.id = m_next_id++;
    if (profile.name.empty())
        profile.name = "Profile " + std::to_string(profile.id);
    m_profiles.push_back(std::move(profile));
    return m_profiles.back().id;
}

bool SurfaceEffectProfileManager::adopt(SurfaceEffectProfile profile)
{
    if (profile.id <= 0 || find(profile.id) != nullptr)
        return false;
    if (profile.name.empty())
        profile.name = "Profile " + std::to_string(profile.id);
    m_next_id = std::max(m_next_id, profile.id + 1);
    m_profiles.push_back(std::move(profile));
    return true;
}

bool SurfaceEffectProfileManager::remove(int id)
{
    for (auto it = m_profiles.begin(); it != m_profiles.end(); ++it)
        if (it->id == id) {
            m_profiles.erase(it);
            return true;
        }
    return false;
}

bool SurfaceEffectProfileManager::rename(int id, const std::string& new_name)
{
    for (auto& p : m_profiles)
        if (p.id == id) {
            p.name = new_name;
            return true;
        }
    return false;
}

const SurfaceEffectProfile* SurfaceEffectProfileManager::find(int id) const
{
    for (const auto& p : m_profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}

SurfaceEffectProfile* SurfaceEffectProfileManager::find_mut(int id)
{
    for (auto& p : m_profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}

void SurfaceEffectProfileManager::clear()
{
    m_profiles.clear();
    m_next_id = 1;
}

void SurfaceEffectProfileManager::restore(std::vector<SurfaceEffectProfile> profiles, int next_id)
{
    m_profiles = std::move(profiles);
    m_next_id  = std::max(1, next_id);
    for (const auto& p : m_profiles)
        m_next_id = std::max(m_next_id, p.id + 1);
}

SurfaceEffectPayload SurfaceEffectProfileManager::snapshot_keys(const DynamicPrintConfig& cfg, const std::vector<std::string>& keys)
{
    SurfaceEffectPayload out;
    for (const auto& k : keys)
        if (cfg.has(k))
            out.kv[k] = cfg.opt_serialize(k);
    out.present = !out.kv.empty();
    return out;
}

void SurfaceEffectProfileManager::payload_from_stacks(const SurfacePassStack& top, const SurfacePassStack& penu, SurfaceEffectProfile& p)
{
    bool cm_top = false, cm_penu = false;
    bool pb_top = false, pb_penu = false;

    auto lift = [](const SurfacePassStack& st, SurfaceEffectProfile& prof, bool& cm_hit, bool& pb_hit) {
        for (const SurfacePass& pass : st.passes) {
            if (pass.kind == SurfacePassKind::ColorStitch && pass.colorstitch.present) {
                for (const auto& kv : pass.colorstitch.kv)
                    prof.colorstitch.kv[kv.first] = kv.second;
                cm_hit = true;
            } else if (pass.kind == SurfacePassKind::PathBlend && pass.pathblend.present) {
                for (const auto& kv : pass.pathblend.kv)
                    prof.pathblend.kv[kv.first] = kv.second;
                pb_hit = true;
            }
        }
    };
    lift(top, p, cm_top, pb_top);
    lift(penu, p, cm_penu, pb_penu);

    if (cm_top || cm_penu) {
        p.colorstitch.present = true;
        p.colorstitch.kv["interlayer_colormix_enabled"] = "1";
    }
    if (pb_top || pb_penu) {
        p.pathblend.present = true;
        p.pathblend.kv["multipass_path_gradient"] = "1";
    }
}

void SurfaceEffectProfileManager::restore_keys(DynamicPrintConfig& cfg, const SurfaceEffectPayload& payload)
{
    if (!payload.present)
        return;
    for (const auto& kv : payload.kv) {
        if (!cfg.has(kv.first))
            continue;
        try {
            cfg.set_deserialize_strict(kv.first, kv.second);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "SurfaceEffectProfile: cannot restore " << kv.first << " = " << kv.second << ": " << e.what();
        }
    }
}

const std::vector<std::string>& SurfaceEffectProfileManager::colorstitch_keys()
{
    static const std::vector<std::string> k = {
        "interlayer_colormix_enabled",
        "interlayer_colormix_mode",
        "interlayer_colormix_pattern_top",
        "interlayer_colormix_weave",
        "interlayer_colormix_filament_a",
        "interlayer_colormix_filament_b",
        "interlayer_colormix_filament_c",
        "interlayer_colormix_filament_d",
        "interlayer_colormix_pct_a",
        "interlayer_colormix_pct_b",
        "interlayer_colormix_easing",
        "interlayer_colormix_gamma",
        "interlayer_colormix_overlap",
        "interlayer_colormix_invert",
        "interlayer_colormix_band_mm_a",
        "interlayer_colormix_band_mm_b",
        "interlayer_colormix_band_mm_c",
        "interlayer_colormix_band_mm_d",
        "interlayer_colormix_gradient_span_mm",
        "interlayer_colormix_repetitions",
        "interlayer_colormix_angle",
        "interlayer_colormix_min_length",
        "interlayer_colormix_min_surface_lines",
    };
    return k;
}

const std::vector<std::string>& SurfaceEffectProfileManager::multipass_keys()
{
    // multipass_prime_volume is print-wide (wipe tower), not part of a recipe.
    static const std::vector<std::string> k = {
        "multipass_enabled",
        "multipass_num_passes",
        "multipass_kind_1",
        "multipass_kind_2",
        "multipass_kind_3",
        "multipass_filament_1",
        "multipass_filament_2",
        "multipass_filament_3",
        "multipass_height_1",
        "multipass_height_2",
        "multipass_height_3",
        "multipass_angle_1",
        "multipass_angle_2",
        "multipass_angle_3",
    };
    return k;
}

const std::vector<std::string>& SurfaceEffectProfileManager::pathblend_keys()
{
    static const std::vector<std::string> k = {
        "multipass_path_gradient",
        "pathblend_mode",
        "pathblend_filament_ramp",
        "pathblend_filament_cap",
        "pathblend_floor_mm",
        "pathblend_ramp_end_mm",
        "pathblend_fill_angle",
        "pathblend_top",
    };
    return k;
}

static nlohmann::json payload_to_json(const SurfaceEffectPayload& p)
{
    nlohmann::json j;
    j["present"] = p.present;
    j["kv"]      = p.kv;
    return j;
}

static SurfaceEffectPayload payload_from_json(const nlohmann::json& j)
{
    SurfaceEffectPayload out;
    if (j.is_object()) {
        if (j.contains("present") && j["present"].is_boolean())
            out.present = j["present"].get<bool>();
        if (j.contains("kv") && j["kv"].is_object())
            for (auto it = j["kv"].begin(); it != j["kv"].end(); ++it)
                if (it.value().is_string())
                    out.kv[it.key()] = it.value().get<std::string>();
    }
    return out;
}

// Schema (same as NeotkoCM, "v": 2):
//   { "v": 2, "next_id": 7, "profiles": [ { "id", "name", "preview_argb", "colormix", "pathblend",
//     "multipass", "stack_top_json", "stack_penu_json", "stack_bottom_json", "auto" }, ... ] }
std::string SurfaceEffectProfileManager::to_json() const
{
    nlohmann::json root;
    root["v"]       = 2;
    root["next_id"] = m_next_id;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : m_profiles) {
        nlohmann::json e;
        e["id"]                = p.id;
        e["name"]              = p.name;
        e["preview_argb"]      = p.preview_argb;
        e["colormix"]          = payload_to_json(p.colorstitch);
        e["pathblend"]         = payload_to_json(p.pathblend);
        e["multipass"]         = payload_to_json(p.multipass);
        e["stack_top_json"]    = p.stack_top_json;
        e["stack_penu_json"]   = p.stack_penu_json;
        e["stack_bottom_json"] = p.stack_bottom_json;
        e["auto"]              = p.auto_generated;
        arr.push_back(std::move(e));
    }
    root["profiles"] = std::move(arr);
    return root.dump();
}

bool SurfaceEffectProfileManager::from_json(const std::string& text)
{
    if (text.empty())
        return false;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "SurfaceEffectProfileManager::from_json parse error: " << e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("profiles") || !root["profiles"].is_array())
        return false;

    m_profiles.clear();
    m_next_id = 1;

    for (const auto& e : root["profiles"]) {
        if (!e.is_object())
            continue;
        SurfaceEffectProfile p;
        if (e.contains("id") && e["id"].is_number_integer())
            p.id = e["id"].get<int>();
        if (e.contains("name") && e["name"].is_string())
            p.name = e["name"].get<std::string>();
        if (e.contains("preview_argb") && e["preview_argb"].is_number())
            p.preview_argb = e["preview_argb"].get<uint32_t>();
        if (e.contains("colormix"))
            p.colorstitch = payload_from_json(e["colormix"]);
        if (e.contains("pathblend"))
            p.pathblend = payload_from_json(e["pathblend"]);
        if (e.contains("multipass"))
            p.multipass = payload_from_json(e["multipass"]);
        if (e.contains("stack_top_json") && e["stack_top_json"].is_string())
            p.stack_top_json = e["stack_top_json"].get<std::string>();
        if (e.contains("stack_penu_json") && e["stack_penu_json"].is_string())
            p.stack_penu_json = e["stack_penu_json"].get<std::string>();
        if (e.contains("stack_bottom_json") && e["stack_bottom_json"].is_string())
            p.stack_bottom_json = e["stack_bottom_json"].get<std::string>();
        if (e.contains("auto") && e["auto"].is_boolean())
            p.auto_generated = e["auto"].get<bool>();
        // Profiles saved with only the visual stacks: derive the payloads from them.
        if (!p.colorstitch.present && !p.pathblend.present && (!p.stack_top_json.empty() || !p.stack_penu_json.empty())) {
            const SurfacePassStack st_top  = SurfacePassStack::from_json(p.stack_top_json);
            const SurfacePassStack st_penu = SurfacePassStack::from_json(p.stack_penu_json);
            payload_from_stacks(st_top, st_penu, p);
        }
        if (p.id <= 0)
            p.id = m_next_id;
        m_next_id = std::max(m_next_id, p.id + 1);
        m_profiles.push_back(std::move(p));
    }
    if (root.contains("next_id") && root["next_id"].is_number_integer())
        m_next_id = std::max(m_next_id, root["next_id"].get<int>());
    return true;
}

} // namespace Slic3r
