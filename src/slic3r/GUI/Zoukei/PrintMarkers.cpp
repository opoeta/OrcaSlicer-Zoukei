// Orca Zoukei: print markers (pause / colour change / tool change / custom G-code). See PrintMarkers.hpp.
#include "PrintMarkers.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>

namespace Slic3r {
namespace GUI {
namespace Zoukei {
namespace PrintMarkers {

namespace {

constexpr const char* KEY_ENABLED = "zoukei_print_markers";
constexpr size_t      MAX_JOBS    = 24;

using json = nlohmann::json;

struct Entry
{
    std::string key;
    std::string name;
    MarkerSet   set;
};

std::mutex         g_mutex;
std::vector<Entry> g_jobs; // most recent first
bool               g_loaded = false;

const char* kind_code(Kind k)
{
    switch (k) {
    case Kind::Pause: return "pause";
    case Kind::ColorChange: return "color";
    case Kind::ToolChange: return "tool";
    default: return "custom";
    }
}

Kind kind_from_code(const std::string& s)
{
    if (s == "pause") return Kind::Pause;
    if (s == "color") return Kind::ColorChange;
    if (s == "tool") return Kind::ToolChange;
    return Kind::Custom;
}

bool ends_with(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string normalize_key(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name.erase(0, slash + 1);
    for (char& c : name)
        if (static_cast<unsigned char>(c) < 128)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool again = true;
    while (again) {
        again = false;
        for (const char* suffix : {".3mf", ".gcode", ".gco", ".g"})
            if (ends_with(name, suffix)) {
                name.erase(name.size() - std::char_traits<char>::length(suffix));
                again = true;
            }
    }
    const size_t b = name.find_first_not_of(' ');
    const size_t e = name.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
}

std::string first_line(const std::string& s, size_t max_len)
{
    std::string out = s.substr(0, s.find_first_of("\r\n"));
    if (out.size() > max_len)
        out.resize(max_len);
    return out;
}

json marker_to_json(const Marker& m)
{
    return {{"k", kind_code(m.kind)}, {"z", m.print_z}, {"layer", m.layer}, {"t", m.time_s}, {"frac", m.frac},
            {"ext", m.extruder},      {"color", m.color}, {"text", m.text}};
}

MarkerSet set_from_json(const json& j)
{
    MarkerSet set;
    if (!j.is_object())
        return set;
    set.total_time   = j.value("total", 0.f);
    set.total_layers = j.value("layers", 0);
    set.has_time     = j.value("has_time", false);
    if (j.contains("markers") && j["markers"].is_array())
        for (const json& m : j["markers"]) {
            if (!m.is_object())
                continue;
            Marker mk;
            mk.kind     = kind_from_code(m.value("k", std::string()));
            mk.print_z  = m.value("z", 0.);
            mk.layer    = m.value("layer", 1);
            mk.time_s   = m.value("t", 0.f);
            mk.frac     = m.value("frac", 0.f);
            mk.extruder = m.value("ext", 0);
            mk.color    = m.value("color", std::string());
            mk.text     = m.value("text", std::string());
            set.markers.push_back(std::move(mk));
        }
    return set;
}

boost::filesystem::path store_path() { return boost::filesystem::path(data_dir()) / "zoukei_print_markers.json"; }

// Caller holds g_mutex.
void load_locked()
{
    if (g_loaded)
        return;
    g_loaded = true;
    try {
        boost::nowide::ifstream ifs(store_path().string());
        if (!ifs.good())
            return;
        json root = json::parse(ifs, nullptr, false);
        if (!root.is_array())
            return;
        for (const json& e : root) {
            if (!e.is_object() || g_jobs.size() >= MAX_JOBS)
                continue;
            Entry en;
            en.name = e.value("name", std::string());
            en.key  = normalize_key(en.name);
            en.set  = set_from_json(e.value("set", json()));
            if (!en.key.empty() && !en.set.empty())
                g_jobs.push_back(std::move(en));
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(warning) << "PrintMarkers: cannot read the job store: " << ex.what();
    }
}

// Caller holds g_mutex.
void save_locked()
{
    try {
        json root = json::array();
        for (const Entry& e : g_jobs)
            root.push_back({{"name", e.name}, {"set", to_json(e.set)}});
        boost::nowide::ofstream ofs(store_path().string(), std::ios::binary | std::ios::trunc);
        if (ofs.good())
            ofs << root.dump();
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(warning) << "PrintMarkers: cannot write the job store: " << ex.what();
    }
}

} // namespace

bool enabled()
{
    const AppConfig* ac = wxGetApp().app_config;
    if (ac == nullptr)
        return true;
    const std::string v = ac->get(KEY_ENABLED);
    return v.empty() ? true : ac->get_bool(KEY_ENABLED);
}

MarkerSet build(const GCodeProcessorResult& r)
{
    MarkerSet set;
    if (r.custom_gcode_per_print_z.empty())
        return set;

    constexpr size_t mode = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

    // Start time of each extruded layer, from the per-move durations.
    struct LayerStart
    {
        float z;
        float t;
    };
    std::vector<LayerStart> layers;
    float                   acc    = 0.f;
    float                   last_z = -1.f;
    for (const auto& mv : r.moves) {
        if (mv.type == EMoveType::Extrude && mv.position.z() > last_z + 1e-4f) {
            layers.push_back({mv.position.z(), acc});
            last_z = mv.position.z();
        }
        acc += mv.time[mode];
    }
    set.total_layers = static_cast<int>(layers.size());
    set.total_time   = r.print_statistics.modes[mode].time > 0.f ? r.print_statistics.modes[mode].time : acc;
    set.has_time     = set.total_time > 0.f && !layers.empty();

    for (const CustomGCode::Item& it : r.custom_gcode_per_print_z) {
        Marker m;
        switch (it.type) {
        case CustomGCode::PausePrint: m.kind = Kind::Pause; break;
        case CustomGCode::ColorChange: m.kind = Kind::ColorChange; break;
        case CustomGCode::ToolChange: m.kind = Kind::ToolChange; break;
        case CustomGCode::Template:
        case CustomGCode::Custom: m.kind = Kind::Custom; break;
        default: continue;
        }
        m.print_z  = it.print_z;
        m.extruder = it.extruder;
        if (m.kind == Kind::ColorChange)
            m.color = it.color;
        else if (m.kind == Kind::Pause)
            m.text = first_line(it.extra.empty() ? it.color : it.extra, 60);
        else if (m.kind == Kind::Custom)
            m.text = first_line(it.extra, 60);

        if (!layers.empty()) {
            auto lb = std::lower_bound(layers.begin(), layers.end(), static_cast<float>(it.print_z) - 1e-3f,
                                       [](const LayerStart& l, float z) { return l.z < z; });
            if (lb == layers.end()) {
                m.layer  = set.total_layers;
                m.time_s = set.total_time;
            } else {
                m.layer  = static_cast<int>(lb - layers.begin()) + 1;
                m.time_s = lb->t;
            }
        }
        set.markers.push_back(std::move(m));
    }
    if (set.markers.empty())
        return set;

    // The time estimator knows pauses and colour changes exactly: use it when it lines up with the items.
    {
        std::vector<Marker*> timed;
        for (Marker& m : set.markers)
            if (m.kind == Kind::Pause || m.kind == Kind::ColorChange)
                timed.push_back(&m);
        const auto& entries = r.print_statistics.modes[mode].custom_gcode_times;
        if (!timed.empty() && entries.size() >= timed.size()) {
            bool               ok  = true;
            float              cum = 0.f;
            std::vector<float> times;
            for (size_t i = 0; i < timed.size() && ok; ++i) {
                const CustomGCode::Type want = timed[i]->kind == Kind::Pause ? CustomGCode::PausePrint : CustomGCode::ColorChange;
                ok                           = entries[i].first == want;
                cum += entries[i].second.first;
                times.push_back(cum);
            }
            if (ok)
                for (size_t i = 0; i < timed.size(); ++i)
                    timed[i]->time_s = set.total_time > 0.f ? std::min(times[i], set.total_time) : times[i];
        }
    }

    for (Marker& m : set.markers) {
        if (set.has_time)
            m.frac = std::max(0.f, std::min(1.f, m.time_s / set.total_time));
        else if (set.total_layers > 0)
            m.frac = std::max(0.f, std::min(1.f, float(m.layer) / float(set.total_layers)));
    }
    std::stable_sort(set.markers.begin(), set.markers.end(), [](const Marker& a, const Marker& b) { return a.frac < b.frac; });
    return set;
}

void remember_job(const std::string& job_name, const MarkerSet& set)
{
    const std::string key = normalize_key(job_name);
    if (key.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [&](const Entry& e) { return e.key == key; }), g_jobs.end());
    if (!set.empty()) {
        g_jobs.insert(g_jobs.begin(), Entry{key, job_name, set});
        if (g_jobs.size() > MAX_JOBS)
            g_jobs.resize(MAX_JOBS);
    }
    save_locked();
}

void remember_job(const std::string& job_name, const GCodeProcessorResult* result)
{
    remember_job(job_name, result != nullptr ? build(*result) : MarkerSet());
}

bool find_job(const std::string& job_name, MarkerSet& out)
{
    const std::string key = normalize_key(job_name);
    if (key.empty())
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    for (const Entry& e : g_jobs)
        if (e.key == key) {
            out = e.set;
            return true;
        }
    // The printer may report the name with a plate suffix (or the project name with one less): prefix match.
    for (const Entry& e : g_jobs) {
        const std::string& a = e.key.size() < key.size() ? e.key : key;
        const std::string& b = e.key.size() < key.size() ? key : e.key;
        if (a.size() >= 4 && b.compare(0, a.size(), a) == 0) {
            out = e.set;
            return true;
        }
    }
    return false;
}

json to_json(const MarkerSet& set)
{
    json arr = json::array();
    for (const Marker& m : set.markers)
        arr.push_back(marker_to_json(m));
    return {{"total", set.total_time}, {"layers", set.total_layers}, {"has_time", set.has_time}, {"markers", std::move(arr)}};
}

json web_answer(const std::string& request_id, const std::string& job_name)
{
    json out = {{"requestId", request_id}, {"ok", true}, {"enabled", enabled()}, {"name", job_name}, {"markers", json::array()}};
    if (!enabled())
        return out;
    MarkerSet set;
    if (find_job(job_name, set)) {
        json s = to_json(set);
        for (auto it = s.begin(); it != s.end(); ++it)
            out[it.key()] = it.value();
    }
    return out;
}

wxColour colour(const Marker& m)
{
    switch (m.kind) {
    case Kind::Pause: return wxColour(245, 166, 35);
    case Kind::ColorChange: {
        if (m.color.size() == 7 && m.color[0] == '#') {
            wxColour c;
            if (c.Set(wxString::FromUTF8(m.color)))
                return c;
        }
        return wxColour(0, 174, 66);
    }
    case Kind::ToolChange: return wxColour(45, 156, 219);
    default: return wxColour(155, 89, 182);
    }
}

wxColour outline_colour(bool dark) { return dark ? wxColour(30, 30, 32) : wxColour(255, 255, 255); }

wxString kind_label(Kind k)
{
    switch (k) {
    case Kind::Pause: return _L("Pause");
    case Kind::ColorChange: return _L("Color change");
    case Kind::ToolChange: return _L("Tool change");
    default: return _L("Custom G-code");
    }
}

wxString format_duration(float seconds)
{
    return wxString::FromUTF8(short_time(get_time_dhms(seconds < 0.f ? 0.f : seconds)));
}

wxString describe(const Marker& m)
{
    wxString s = wxString::Format(_L("%s, layer %d, at %s"), kind_label(m.kind), m.layer, format_duration(m.time_s));
    if (!m.text.empty())
        s += " (" + wxString::FromUTF8(m.text) + ")";
    return s;
}

const Marker* next_marker(const MarkerSet& set, double progress, double left_s, double* seconds_until)
{
    if (seconds_until != nullptr)
        *seconds_until = -1.;
    for (const Marker& m : set.markers) {
        if (m.frac <= progress + 1e-4)
            continue;
        if (seconds_until != nullptr) {
            if (left_s > 0. && progress < 1.)
                *seconds_until = left_s * (m.frac - progress) / (1. - progress);
            else if (set.has_time)
                *seconds_until = std::max(0., double(m.time_s) - progress * double(set.total_time));
        }
        return &m;
    }
    return nullptr;
}

} // namespace PrintMarkers
} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
