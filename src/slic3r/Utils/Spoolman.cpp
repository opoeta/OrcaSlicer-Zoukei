#include "Spoolman.hpp"

#include "Http.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace Slic3r { namespace Spoolman {

using json = nlohmann::json;

namespace {

std::mutex        s_cache_mutex;
std::map<int, Spool> s_cache;

std::string jstr(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

double jnum(const json& j, const char* key, double fallback = -1.)
{
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}

int jint(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<int>() : 0;
}

// GET returning the body on HTTP 200.
bool http_get(const std::string& url, std::string& body, std::string& error, int timeout = 10)
{
    bool ok = false;
    Http::get(url)
        .header("Accept", "application/json")
        .timeout_connect(4)
        .timeout_max(timeout)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            } else
                error = "HTTP " + std::to_string(status);
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            error = err.empty() ? "HTTP " + std::to_string(status) : err + (status != 0 ? " (HTTP " + std::to_string(status) + ")" : std::string());
        })
        .perform_sync();
    return ok;
}

// lower-case letters and digits only
std::string squash(const std::string& s)
{
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c))
            out += char(std::tolower(c));
    return out;
}

std::vector<std::string> tokens_upper(const std::string& s)
{
    std::vector<std::string> out;
    std::string              cur;
    for (unsigned char c : s) {
        if (std::isalnum(c))
            cur += char(std::toupper(c));
        else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string preset_type(const Preset& p)
{
    if (const auto* o = p.config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
        return o->values.front();
    return {};
}

std::string preset_vendor(const Preset& p)
{
    if (const auto* o = p.config.option<ConfigOptionStrings>("filament_vendor"); o != nullptr && !o->values.empty())
        return o->values.front();
    return {};
}

// "Voolt3D PLA SILK @Flashforge AD5X 0.4 nozzle" -> "Voolt3D PLA SILK"
std::string preset_base_name(const std::string& name)
{
    const size_t at = name.find(" @");
    return at == std::string::npos ? name : name.substr(0, at);
}

} // namespace

std::string Spool::title() const
{
    std::string t = "#" + std::to_string(id);
    if (!vendor.empty())
        t += " " + vendor;
    if (!name.empty())
        t += " " + name;
    if (!material.empty() && !boost::iequals(material, name))
        t += " (" + material + ")";
    return t;
}

std::string normalize_url(const std::string& raw)
{
    std::string u = raw;
    boost::trim(u);
    if (u.empty())
        return {};
    if (u.find("://") == std::string::npos)
        u = "http://" + u;
    while (!u.empty() && u.back() == '/')
        u.pop_back();
    for (const char* suffix : {"/api/v1", "/api"})
        if (boost::iends_with(u, suffix)) {
            u.resize(u.size() - std::strlen(suffix));
            break;
        }
    while (!u.empty() && u.back() == '/')
        u.pop_back();
    return u;
}

std::string configured_url()
{
    AppConfig* cfg = GUI::wxGetApp().app_config;
    return cfg == nullptr ? std::string() : normalize_url(cfg->get("spoolman_url"));
}

std::string normalize_colour(const std::string& raw)
{
    std::string c = raw;
    boost::trim(c);
    if (boost::istarts_with(c, "0x"))
        c = c.substr(2);
    else if (!c.empty() && c.front() == '#')
        c = c.substr(1);
    if (c.size() != 6 && c.size() != 8)
        return {};
    c = c.substr(0, 6);
    for (char ch : c)
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return {};
    boost::to_upper(c);
    return "#" + c;
}

Spool parse_spool(const std::string& text, bool* ok)
{
    Spool s;
    if (ok)
        *ok = false;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("id") || !j["id"].is_number())
        return s;
    // Spoolman (v0.22+) keeps colour data on the filament: color_hex, multi_color_hexes ("RRGGBB,RRGGBB") and
    // multi_color_direction ("coaxial" | "longitudinal"). Any field may be missing.
    s.id              = jint(j, "id");
    s.remaining_weight = jnum(j, "remaining_weight");
    s.initial_weight   = jnum(j, "initial_weight");
    s.spool_weight     = jnum(j, "spool_weight");
    s.location        = jstr(j, "location");
    s.comment         = jstr(j, "comment");
    s.archived        = j.contains("archived") && j["archived"].is_boolean() && j["archived"].get<bool>();
    if (j.contains("filament") && j["filament"].is_object()) {
        const json& f = j["filament"];
        s.filament_id = jint(f, "id");
        s.name        = jstr(f, "name");
        s.material    = jstr(f, "material");
        s.nozzle_temp = int(jnum(f, "settings_extruder_temp", 0.));
        s.bed_temp    = int(jnum(f, "settings_bed_temp", 0.));
        if (s.spool_weight < 0.)
            s.spool_weight = jnum(f, "spool_weight");
        if (s.initial_weight < 0.)
            s.initial_weight = jnum(f, "weight");
        if (f.contains("vendor") && f["vendor"].is_object()) {
            s.vendor    = jstr(f["vendor"], "name");
            s.vendor_id = jint(f["vendor"], "id");
        }
        s.color = normalize_colour(jstr(f, "color_hex"));
        std::vector<std::string> cols;
        {
            std::string       hexes = jstr(f, "multi_color_hexes");
            std::stringstream ss(hexes);
            for (std::string item; std::getline(ss, item, ',');) {
                const std::string c = normalize_colour(item);
                if (!c.empty())
                    cols.push_back(c);
            }
        }
        if (cols.size() >= 2) {
            if (cols.size() > 4)
                cols.resize(4); // Orca keeps up to four colours per filament
            if (s.color.empty() || s.color != cols.front())
                s.color = cols.front(); // the project colour is the first one
            s.colors   = std::move(cols);
            s.gradient = boost::iequals(jstr(f, "multi_color_direction"), "longitudinal");
        } else if (s.color.empty() && cols.size() == 1)
            s.color = cols.front();
    }
    if (ok)
        *ok = true;
    return s;
}

std::vector<Spool> parse_spools(const std::string& text, bool* ok)
{
    std::vector<Spool> out;
    if (ok)
        *ok = false;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array())
        return out;
    for (const json& e : j) {
        bool        one = false;
        Spool       s   = parse_spool(e.dump(), &one);
        if (one && !s.archived)
            out.push_back(std::move(s));
    }
    if (ok)
        *ok = true;
    return out;
}

void cache_spools(const std::vector<Spool>& spools)
{
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_cache.clear();
    for (const Spool& s : spools)
        s_cache[s.id] = s;
}

bool cached_spool(int id, Spool& out)
{
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    const auto                  it = s_cache.find(id);
    if (it == s_cache.end())
        return false;
    out = it->second;
    return true;
}

bool fetch_info(const std::string& url, std::string& version, std::string& error)
{
    std::string body;
    if (!http_get(url + "/api/v1/info", body, error, 6))
        return false;
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("version")) {
        error = "not a Spoolman server";
        return false;
    }
    version = jstr(j, "version");
    return true;
}

bool fetch_spools(const std::string& url, std::vector<Spool>& out, std::string& error)
{
    std::string body;
    if (!http_get(url + "/api/v1/spool?allow_archived=false", body, error, 20))
        return false;
    bool ok = false;
    out     = parse_spools(body, &ok);
    if (!ok) {
        error = "unexpected reply";
        return false;
    }
    cache_spools(out);
    return true;
}

bool fetch_spool(const std::string& url, int id, Spool& out, std::string& error, int timeout)
{
    std::string body;
    if (!http_get(url + "/api/v1/spool/" + std::to_string(id), body, error, timeout))
        return false;
    bool ok = false;
    out     = parse_spool(body, &ok);
    if (!ok) {
        error = "unexpected reply";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        s_cache[out.id] = out;
    }
    return true;
}

std::string discover_from_moonraker(const std::string& base_url, const std::string& api_key)
{
    if (base_url.empty())
        return {};
    std::string body, error;
    bool        ok = false;
    auto        http = Http::get(base_url + "/server/config");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(3)
        .timeout_max(6)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            }
        })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    if (!ok)
        return {};
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("result") || !j["result"].is_object() || !j["result"].contains("config") ||
        !j["result"]["config"].is_object() || !j["result"]["config"].contains("spoolman") || !j["result"]["config"]["spoolman"].is_object())
        return {};
    return normalize_url(jstr(j["result"]["config"]["spoolman"], "server"));
}

std::string orca_type_of(const Spool& spool, bool* silk)
{
    if (silk)
        *silk = false;
    const auto toks = tokens_upper(spool.material + " " + spool.name);
    auto has = [&](const char* t) { return std::find(toks.begin(), toks.end(), std::string(t)) != toks.end(); };
    auto starts = [&](const char* prefix) {
        return std::any_of(toks.begin(), toks.end(), [&](const std::string& t) { return t.rfind(prefix, 0) == 0; });
    };
    const bool carbon = has("CF") || has("CARBON");
    const bool is_silk = has("SILK");
    std::string type;
    if (has("PETG") || (has("PET") && has("G")))
        type = carbon ? "PETG-CF" : "PETG";
    else if (has("PLA"))
        type = carbon ? "PLA-CF" : "PLA";
    else if (has("ABS"))
        type = "ABS";
    else if (has("ASA"))
        type = "ASA";
    else if (has("TPU") || has("TPE") || has("FLEX"))
        type = "TPU";
    else if (has("PVA"))
        type = "PVA";
    else if (has("HIPS"))
        type = "HIPS";
    else if (has("PC"))
        type = "PC";
    else if (has("NYLON") || has("PA") || starts("PA1") || starts("PA6") || starts("PAHT"))
        type = carbon ? "PA-CF" : "PA";
    else if (has("PET"))
        type = "PET";
    else if (has("PP"))
        type = "PP";
    else if (is_silk)
        type = "PLA"; // "SILK" alone: the common silk filaments are PLA
    if (silk)
        *silk = is_silk && type == "PLA";
    return type;
}

std::string best_preset(const PresetBundle& bundle, const Spool& spool)
{
    bool              silk = false;
    const std::string type = orca_type_of(spool, &silk);
    if (type.empty())
        return {};
    const std::string vendor_key = squash(spool.vendor);
    // The filament name without the vendor and without the material word ("Voolt3D PLA SILK" -> "silk").
    auto strip_vendor = [&](const std::string& s) {
        std::string k = squash(s);
        if (!vendor_key.empty() && k.rfind(vendor_key, 0) == 0)
            k = k.substr(vendor_key.size());
        return k;
    };
    const std::string name_key = strip_vendor(spool.name);
    auto name_tokens = [&](const std::string& s) {
        std::set<std::string> out;
        for (const std::string& t : tokens_upper(s))
            if (t.size() >= 2 && t != "THE")
                out.insert(t);
        return out;
    };
    const std::set<std::string> spool_tokens = name_tokens(spool.name);

    const Preset* best       = nullptr;
    int           best_score = -1;
    for (const Preset& p : bundle.filaments) {
        if (!p.is_visible || !p.is_compatible || !boost::iequals(preset_type(p), type))
            continue;
        const std::string base = preset_base_name(p.name);
        const std::string pv   = squash(preset_vendor(p));
        int               score = 1;
        const bool        generic = boost::icontains(base, "generic");
        const bool        vendor_match = !vendor_key.empty() && ((!pv.empty() && pv == vendor_key) || squash(base).rfind(vendor_key, 0) == 0);
        if (vendor_match)
            score += 100;
        else if (generic)
            score += 10;
        const std::string pname = strip_vendor(base);
        if (!name_key.empty() && !pname.empty()) {
            if (pname == name_key)
                score += 60;
            else if (name_key.size() >= 3 && pname.size() >= 3 && (pname.find(name_key) != std::string::npos || name_key.find(pname) != std::string::npos))
                score += 30;
        }
        {   // shared words (other than the material itself)
            int shared = 0;
            for (const std::string& t : name_tokens(base))
                if (spool_tokens.count(t) && !boost::iequals(t, type))
                    ++shared;
            score += 8 * shared;
        }
        const bool p_silk = boost::icontains(base, "silk");
        if (silk && p_silk)
            score += 40;
        else if (silk != p_silk && type == "PLA")
            score -= 40;
        if (boost::icontains(base, "+") && !boost::icontains(spool.name, "+") && !boost::icontains(spool.material, "+"))
            score -= 2; // "PLA+" / "PLA Plus" presets only when asked for
        if (score > best_score) {
            best_score = score;
            best       = &p;
        }
    }
    return best != nullptr ? best->name : std::string();
}

}} // namespace Slic3r::Spoolman
