// Ported from the "Creality CFS Sync" OrcaSlicer plugin by Heyde Moura (creality_cfs.py).
#include "CrealityCfs.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace Slic3r { namespace CrealityCfs {

using json = nlohmann::json;

namespace {

std::string to_upper(std::string s)
{
    for (char& c : s)
        c = char(std::toupper((unsigned char) c));
    return s;
}

std::string trim(const std::string& s)
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Lower case, runs of anything but [a-z0-9] become one space, trimmed.
std::string norm(const std::string& text)
{
    std::string out;
    bool        gap = false;
    for (unsigned char c : text) {
        const char lower = char(std::tolower(c));
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
            if (gap && !out.empty())
                out.push_back(' ');
            gap = false;
            out.push_back(lower);
        } else {
            gap = true;
        }
    }
    return out;
}

std::vector<std::string> words(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream       in(norm(text));
    std::string              word;
    while (in >> word)
        out.push_back(word);
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

// Locale independent text -> double ("50.100000" is 50.1 whatever the C locale: std::stod / strtod follow LC_NUMERIC, and
// under pt_BR they stop at the dot, which used to turn the K2's string temperatures into 0). False when not a number.
bool parse_double(const std::string& raw, double& out)
{
    const std::string text = trim(raw);
    const char*       first = text.data();
    const char*       last  = text.data() + text.size();
    if (first != last && *first == '+')
        ++first;
    if (first == last)
        return false;
    const auto res = std::from_chars(first, last, out);
    return res.ec == std::errc() && res.ptr == last;
}

// Python's float(value) with a default for None / "" / garbage.
double to_num(const json& value, double fallback = 0.)
{
    try {
        if (value.is_number())
            return value.get<double>();
        if (value.is_boolean())
            return value.get<bool>() ? 1. : 0.;
        if (value.is_string()) {
            double result = 0.;
            return parse_double(value.get<std::string>(), result) ? result : fallback;
        }
    } catch (...) {}
    return fallback;
}

// Reads a temperature field into `target`; an unreadable value ("", "xxx.xxxxxx", null) keeps the last known one.
void read_temp(const json& msg, const char* key, std::optional<double>& target)
{
    if (!msg.contains(key))
        return;
    const double v = to_num(msg[key], std::nan(""));
    if (!std::isnan(v))
        target = v;
}

int to_int(const json& value, int fallback = 0)
{
    const double v = to_num(value, std::nan(""));
    return std::isnan(v) ? fallback : int(v);
}

const json& field(const json& obj, const char* key)
{
    static const json null_value;
    if (!obj.is_object())
        return null_value;
    auto it = obj.find(key);
    return it == obj.end() ? null_value : *it;
}

std::string str(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_null())
        return {};
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    return value.dump();
}

std::string basename(const std::string& path)
{
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string hex_bits(unsigned long long bits)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llX", bits);
    return buffer;
}

std::string humidity_level(const std::optional<double>& percent)
{
    if (!percent)
        return "0";
    if (*percent < 20)
        return "1";
    if (*percent < 40)
        return "2";
    if (*percent < 60)
        return "3";
    if (*percent < 75)
        return "4";
    return "5";
}

std::string gcode_state(const PrinterSnapshot& snapshot)
{
    if (snapshot.device_state == DEVICE_STATE_PRINTING)
        return "RUNNING";
    if (snapshot.device_state == 2)
        return snapshot.print_left_seconds && *snapshot.print_left_seconds != 0 ? "PAUSE" : "FINISH";
    if (snapshot.device_state == 5)
        return "PREPARE";
    return "IDLE";
}

} // namespace

// ---- colours ---------------------------------------------------------------------------------

std::string normalize_hex(const std::string& color, const std::string& fallback)
{
    std::string value = trim(color);
    if (value.empty())
        return fallback;
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        value = value.substr(2);
    if (!value.empty() && value[0] == '#')
        value = value.substr(1);
    std::string hex;
    for (char c : value)
        if (std::isxdigit((unsigned char) c))
            hex.push_back(c);
    if (hex.size() == 7 && hex[0] == '0')
        hex = hex.substr(1);
    else if (hex.size() == 8) {
        if (to_upper(hex.substr(0, 2)) == "FF" && to_upper(hex.substr(6)) != "FF")
            hex = hex.substr(2);
        else
            hex = hex.substr(0, 6);
    }
    if (hex.size() != 6)
        return fallback;
    return "#" + to_upper(hex);
}

std::string hex_to_creality(const std::string& color)
{
    std::string value = normalize_hex(color).substr(1);
    for (char& c : value)
        c = char(std::tolower((unsigned char) c));
    return "#0" + value;
}

std::string hex_to_bbl(const std::string& color) { return normalize_hex(color).substr(1) + "FF"; }

std::string normalize_filament_type(const std::string& filament_type)
{
    static const char* base_types[] = {"PETG", "PET", "PLA", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS", "PP", "PPS", "BVOH"};
    const std::string ft    = trim(filament_type);
    const std::string upper = to_upper(ft);
    for (const char* base : base_types)
        if (upper.rfind(base, 0) == 0)
            return base;
    return ft;
}

// ---- data model -------------------------------------------------------------------------------

bool MaterialLibraryEntry::is_generic() const { return norm(brand) == "generic"; }

bool CfsSlot::is_loaded() const
{
    if (state == SLOT_STATE_EMPTY)
        return false;
    return !(vendor.empty() && type.empty() && name.empty() && color.empty());
}

std::string CfsSlot::label() const { return std::to_string(box_id) + char('A' + slot_id); }

json MaterialSpec::to_params(int box_id, int slot_id) const
{
    if (is_clear())
        return {{"modifyMaterial",
                 {{"boxId", box_id}, {"id", slot_id}, {"rfid", ""}, {"type", ""}, {"vendor", ""}, {"name", ""}, {"color", ""},
                  {"minTemp", 0}, {"maxTemp", 0}, {"pressure", 0}}}};
    // Creality Print adds 1e-8 so the temperatures serialise as floats; mirrored exactly.
    return {{"modifyMaterial",
             {{"boxId", box_id},
              {"id", slot_id},
              {"rfid", rfid},
              {"type", type},
              {"vendor", vendor},
              {"name", name},
              {"color", color.empty() ? std::string() : hex_to_creality(color)},
              {"minTemp", double(min_temp) + 1e-8},
              {"maxTemp", double(max_temp) + 1e-8},
              {"pressure", pressure}}}};
}

bool PrinterSnapshot::is_idle() const
{
    return device_state == DEVICE_STATE_IDLE && (!feed_state || *feed_state == 0 || *feed_state == 3 || *feed_state == 14);
}

std::vector<CfsBox> PrinterSnapshot::cfs_boxes() const
{
    std::vector<CfsBox> out;
    for (const CfsBox& box : boxes)
        if (box.is_cfs() && box.is_active())
            out.push_back(box);
    return out;
}

const CfsSlot* PrinterSnapshot::find_slot(int box_id, int slot_id) const
{
    for (const CfsBox& box : boxes)
        if (box.id == box_id)
            for (const CfsSlot& slot : box.slots)
                if (slot.slot_id == slot_id)
                    return &slot;
    return nullptr;
}

std::string PresetInfo::base_name() const { return trim(name.substr(0, name.find('@'))); }

// ---- parsing ------------------------------------------------------------------------------------

std::vector<CfsBox> parse_boxes(const json& boxs_info)
{
    std::vector<CfsBox> boxes;
    const json&         raw_boxes = field(boxs_info, "materialBoxs");
    if (!raw_boxes.is_array())
        return boxes;
    for (const json& raw : raw_boxes) {
        if (!raw.is_object())
            continue;
        CfsBox box;
        box.id    = to_int(field(raw, "id"));
        box.type  = to_int(field(raw, "type"));
        box.state = to_int(field(raw, "state"));
        if (raw.contains("temp"))
            box.temp = to_num(raw["temp"]);
        if (raw.contains("humidity"))
            box.humidity = to_num(raw["humidity"]);
        const json& materials = field(raw, "materials");
        if (materials.is_array()) {
            for (const json& mat : materials) {
                if (!mat.is_object())
                    continue;
                CfsSlot slot;
                slot.box_id      = box.id;
                slot.slot_id     = to_int(field(mat, "id"));
                slot.state       = to_int(field(mat, "state"));
                slot.vendor      = str(field(mat, "vendor"));
                slot.type        = str(field(mat, "type"));
                slot.name        = str(field(mat, "name"));
                slot.rfid        = str(field(mat, "rfid"));
                const std::string color_raw = str(field(mat, "color"));
                slot.color       = color_raw.empty() ? std::string() : normalize_hex(color_raw, "");
                slot.min_temp    = to_int(field(mat, "minTemp"));
                slot.max_temp    = to_int(field(mat, "maxTemp"));
                slot.pressure    = to_num(field(mat, "pressure"));
                slot.percent     = to_int(field(mat, "percent"));
                slot.selected    = to_int(field(mat, "selected")) != 0;
                slot.edit_status = to_int(field(mat, "editStatus"));
                box.slots.push_back(std::move(slot));
            }
        }
        std::stable_sort(box.slots.begin(), box.slots.end(), [](const CfsSlot& a, const CfsSlot& b) { return a.slot_id < b.slot_id; });
        boxes.push_back(std::move(box));
    }
    std::stable_sort(boxes.begin(), boxes.end(), [](const CfsBox& a, const CfsBox& b) { return a.id < b.id; });
    return boxes;
}

std::vector<MaterialLibraryEntry> parse_material_library(const json& ret_materials)
{
    std::vector<MaterialLibraryEntry> entries;
    if (!ret_materials.is_array())
        return entries;
    for (const json& raw : ret_materials) {
        if (!raw.is_object())
            continue;
        const json& base = field(raw, "base");
        const json& kv   = field(raw, "kvParam");
        if (!base.is_object())
            continue;
        MaterialLibraryEntry entry;
        entry.id    = str(field(base, "id"));
        entry.brand = str(field(base, "brand"));
        entry.type  = str(field(base, "meterialType"));
        if (entry.type.empty())
            entry.type = str(field(base, "materialType"));
        if (entry.type.empty())
            entry.type = str(field(kv, "filament_type"));
        entry.name             = str(field(base, "name"));
        entry.min_temp         = to_int(field(base, "minTemp"), to_int(field(kv, "nozzle_temperature_range_low")));
        entry.max_temp         = to_int(field(base, "maxTemp"), to_int(field(kv, "nozzle_temperature_range_high")));
        entry.pressure_advance = to_num(field(kv, "pressure_advance"));
        const json& colors     = field(base, "colors");
        if (colors.is_array())
            for (const json& c : colors)
                if (c.is_string())
                    entry.colors.push_back(normalize_hex(c.get<std::string>()));
        if (!entry.name.empty())
            entries.push_back(std::move(entry));
    }
    return entries;
}

void apply_status(PrinterSnapshot& s, const json& msg)
{
    if (!msg.is_object())
        return;
    if (msg.contains("model"))
        s.model = str(msg["model"]);
    if (msg.contains("hostname"))
        s.hostname = str(msg["hostname"]);
    if (msg.contains("modelVersion"))
        s.firmware = str(msg["modelVersion"]);
    if (msg.contains("deviceState"))
        s.device_state = to_int(msg["deviceState"]);
    else if (msg.contains("state") && !s.device_state)
        s.device_state = to_int(msg["state"]);
    if (msg.contains("feedState"))
        s.feed_state = to_int(msg["feedState"]);
    if (msg.contains("cfsConnect"))
        s.cfs_connected = to_int(msg["cfsConnect"]) != 0;
    if (msg.contains("aiFirstFloor"))
        s.ai_first_floor = to_int(msg["aiFirstFloor"]);
    if (msg.contains("printFileName"))
        s.print_file = str(msg["printFileName"]);
    if (msg.contains("printProgress"))
        s.print_progress = to_int(msg["printProgress"]);
    if (msg.contains("printLeftTime"))
        s.print_left_seconds = to_int(msg["printLeftTime"]);
    if (msg.contains("layer"))
        s.layer = to_int(msg["layer"]);
    if (msg.contains("TotalLayer"))
        s.total_layers = to_int(msg["TotalLayer"]);
    // Absent keys (incremental messages) and unreadable values keep the last known reading; bed = bedTemp0 (main bed).
    read_temp(msg, "nozzleTemp", s.nozzle_temp);
    read_temp(msg, "targetNozzleTemp", s.nozzle_target);
    read_temp(msg, "bedTemp0", s.bed_temp);
    read_temp(msg, "targetBedTemp0", s.bed_target);
    read_temp(msg, "boxTemp", s.box_temp);
    if (msg.contains("boxConfig") && msg["boxConfig"].is_object())
        s.box_config = msg["boxConfig"];
    if (msg.contains("boxsInfo") && msg["boxsInfo"].is_object() && msg["boxsInfo"].contains("materialBoxs"))
        s.boxes = parse_boxes(msg["boxsInfo"]);
    if (msg.contains("retMaterials") && msg["retMaterials"].is_array())
        s.library = parse_material_library(msg["retMaterials"]);
}

std::string check_writable(const PrinterSnapshot& s)
{
    if (!s.connected)
        return "printer is not connected";
    if (s.cfs_connected && !*s.cfs_connected)
        return "no CFS is connected to the printer";
    if (!s.device_state)
        return "printer state unknown yet, try again in a moment";
    if (*s.device_state != DEVICE_STATE_IDLE) {
        if (*s.device_state == DEVICE_STATE_PRINTING)
            return "printer is printing; the CFS slot table is locked while a job runs";
        return "printer is busy (deviceState=" + std::to_string(*s.device_state) + ")";
    }
    if (s.feed_state && !(*s.feed_state == 0 || *s.feed_state == 3 || *s.feed_state == 14))
        return "filament is being loaded or unloaded, please wait";
    return {};
}

// ---- matching -----------------------------------------------------------------------------------

const MaterialLibraryEntry* match_library_entry(const std::vector<MaterialLibraryEntry>& entries, const std::string& vendor,
                                                const std::string& filament_type, const std::string& name)
{
    if (entries.empty())
        return nullptr;
    const std::string v = norm(vendor), t = norm(filament_type), n = norm(name);
    const std::string base_t = norm(normalize_filament_type(filament_type));

    auto type_ok = [&](const MaterialLibraryEntry& e, bool strict) {
        if (t.empty())
            return true;
        const std::string et = norm(e.type);
        if (strict)
            return et == t;
        return et == t || norm(normalize_filament_type(e.type)) == base_t;
    };

    // 1. exact brand + type + name
    for (const auto& e : entries)
        if (norm(e.brand) == v && norm(e.type) == t && norm(e.name) == n)
            return &e;
    // 2. same name and type whatever the brand spelling
    for (const auto& e : entries)
        if (type_ok(e, true) && norm(e.name) == n)
            return &e;
    // 3. name containment / word overlap
    std::vector<std::pair<int, const MaterialLibraryEntry*>> scored;
    const std::vector<std::string>                           name_words = words(name);
    const std::set<std::string>                              nw(name_words.begin(), name_words.end());
    for (const auto& e : entries) {
        if (!type_ok(e, false))
            continue;
        const std::string en = norm(e.name);
        if (en.empty())
            continue;
        int score = 0;
        if (contains(n, en) || contains(en, n))
            score += 20;
        std::set<std::string> ew;
        for (const auto& w : words(e.name))
            if (w != "generic" && w != norm(e.type))
                ew.insert(w);
        if (!ew.empty() && std::includes(nw.begin(), nw.end(), ew.begin(), ew.end()))
            score += 10;
        if (!v.empty() && norm(e.brand) == v)
            score += 5;
        if (score)
            scored.emplace_back(score, &e);
    }
    if (!scored.empty()) {
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
            return std::make_tuple(-a.first, a.second->is_generic(), a.second->name) <
                   std::make_tuple(-b.first, b.second->is_generic(), b.second->name);
        });
        return scored.front().second;
    }
    // 4. the generic entry of the same type
    if (t.empty())
        return nullptr;
    for (bool strict : {true, false})
        for (const auto& e : entries)
            if (e.is_generic() && type_ok(e, strict))
                return &e;
    return nullptr;
}

const PresetInfo* match_preset_for_slot(const std::vector<PresetInfo>& presets, const CfsSlot& slot)
{
    const std::string slot_type = norm(normalize_filament_type(slot.type));
    const std::string brand     = norm(slot.name);
    const std::string vendor    = norm(slot.vendor);
    std::set<std::string> spool_words{"generic"};
    for (const std::string* src : {&slot.name, &slot.vendor, &slot.type})
        for (auto& w : words(*src))
            spool_words.insert(w);

    struct Candidate
    {
        int               score;
        int               user;
        const PresetInfo* preset;
    };
    std::vector<Candidate> best;
    for (const PresetInfo& p : presets) {
        if (!(p.is_visible && p.is_compatible))
            continue;
        if (norm(normalize_filament_type(p.type)) != slot_type)
            continue;
        const std::string name_n = norm(p.name);
        int               score  = 0;
        if (!brand.empty() && contains(name_n, brand))
            score += 20;
        if (!vendor.empty() && (vendor == norm(p.vendor) || contains(name_n, vendor)))
            score += 10;
        if (score == 0)
            continue;
        for (const auto& w : words(p.base_name()))
            if (!spool_words.count(w))
                score -= 5;
        best.push_back({score, p.is_system ? 0 : 1, &p});
    }
    if (best.empty()) {
        std::vector<const PresetInfo*> fallback;
        for (const PresetInfo& p : presets)
            if (p.is_visible && p.is_compatible && norm(normalize_filament_type(p.type)) == slot_type)
                fallback.push_back(&p);
        std::stable_sort(fallback.begin(), fallback.end(), [](const PresetInfo* a, const PresetInfo* b) {
            auto key = [](const PresetInfo* p) { return std::make_tuple(contains(norm(p->name), "generic") ? 0 : 1, p->is_system ? 0 : 1, p->name); };
            return key(a) < key(b);
        });
        return fallback.empty() ? nullptr : fallback.front();
    }
    std::stable_sort(best.begin(), best.end(), [](const Candidate& a, const Candidate& b) {
        return std::make_tuple(-a.score, a.user, a.preset->name) < std::make_tuple(-b.score, b.user, b.preset->name);
    });
    return best.front().preset;
}

MaterialSpec spec_from_preset(const PresetInfo& preset, const std::string& color, const std::vector<MaterialLibraryEntry>& library,
                              const std::string& temps_from)
{
    const MaterialLibraryEntry* entry = match_library_entry(library, preset.vendor, preset.type, preset.base_name());
    MaterialSpec                spec;
    spec.color = color.empty() ? std::string() : normalize_hex(color);
    if (entry != nullptr) {
        spec.rfid     = entry->id;
        spec.vendor   = entry->brand;
        spec.type     = entry->type;
        spec.name     = entry->name;
        spec.min_temp = entry->min_temp;
        spec.max_temp = entry->max_temp;
        spec.pressure = entry->pressure_advance;
    } else {
        spec.vendor = preset.vendor.empty() ? "Generic" : preset.vendor;
        spec.type   = preset.type.empty() ? "PLA" : preset.type;
        spec.name   = preset.base_name().empty() ? spec.vendor + " " + spec.type : preset.base_name();
    }
    if (temps_from != "library" || entry == nullptr) {
        if (preset.min_temp)
            spec.min_temp = preset.min_temp;
        if (preset.max_temp)
            spec.max_temp = preset.max_temp;
        if (preset.pressure_advance != 0.)
            spec.pressure = preset.pressure_advance;
    }
    if (!spec.min_temp || !spec.max_temp) {
        // Never hand the printer a 0..0 range.
        const MaterialLibraryEntry* generic = match_library_entry(library, "Generic", spec.type, "Generic " + spec.type);
        if (!spec.min_temp)
            spec.min_temp = generic ? generic->min_temp : 190;
        if (!spec.max_temp)
            spec.max_temp = generic ? generic->max_temp : 240;
    }
    return spec;
}

bool slot_matches(const CfsSlot& slot, const MaterialSpec& spec)
{
    if (spec.is_clear())
        return !slot.is_loaded() || (slot.vendor.empty() && slot.name.empty() && slot.type.empty());
    if (norm(slot.type) != norm(spec.type) || norm(slot.name) != norm(spec.name))
        return false;
    if (!spec.color.empty() && normalize_hex(slot.color, "") != normalize_hex(spec.color))
        return false;
    if (!spec.vendor.empty() && norm(slot.vendor) != norm(spec.vendor))
        return false;
    return true;
}

// ---- project sync -------------------------------------------------------------------------------

std::string project_signature(const std::vector<ProjectFilament>& project)
{
    json list = json::array();
    for (const auto& f : project)
        list.push_back({f.name, normalize_hex(f.color, "")});
    return list.dump();
}

std::vector<std::pair<CfsSlot, MaterialSpec>> plan_project_sync(const std::vector<ProjectFilament>& project,
                                                                const std::vector<PresetInfo>& presets, const PrinterSnapshot& snapshot,
                                                                const std::string& temps_from)
{
    std::vector<CfsSlot> slots;
    for (const CfsBox& box : snapshot.cfs_boxes())
        slots.insert(slots.end(), box.slots.begin(), box.slots.end());
    std::vector<std::pair<CfsSlot, MaterialSpec>> plan;
    for (std::size_t i = 0; i < project.size() && i < slots.size(); ++i) {
        const CfsSlot& slot = slots[i];
        if (slot.is_rfid_locked())
            continue;
        const ProjectFilament& f      = project[i];
        const PresetInfo*      preset = nullptr;
        for (const PresetInfo& p : presets)
            if (p.name == f.name) {
                preset = &p;
                break;
            }
        PresetInfo adhoc;
        if (preset == nullptr) {
            adhoc.name   = f.name;
            adhoc.vendor = f.vendor;
            adhoc.type   = f.type;
            preset       = &adhoc;
        }
        MaterialSpec spec = spec_from_preset(*preset, f.color, snapshot.library, temps_from);
        if (slot_matches(slot, spec) && slot.is_loaded())
            continue;
        plan.emplace_back(slot, std::move(spec));
    }
    return plan;
}

// ---- Bambu-dialect payloads ---------------------------------------------------------------------

// One tray of the Bambu dialect (an AMS slot, or the external spool as "vt_tray").
static json make_tray(const CfsSlot* slot, const std::string& id, const PresetForSlot& preset_for_slot)
{
    json tray = {{"id", id}, {"tag_uid", "0000000000000000"}};
    if (slot != nullptr && slot->is_loaded()) {
        const PresetInfo* preset = preset_for_slot ? preset_for_slot(*slot) : nullptr;
        // Orca skips trays with an empty id ("unknown spool") but falls back to the generic
        // preset for an id it does not know; never 'P'-prefixed (Bambu ids).
        if (preset != nullptr && !preset->filament_id.empty())
            tray["tray_info_idx"] = preset->filament_id;
        else {
            std::string rid = slot->rfid;
            if (rid.empty())
                rid = normalize_filament_type(slot->type);
            tray["tray_info_idx"] = "CFS-" + (rid.empty() ? std::string("UNKNOWN") : rid);
        }
        const std::string type = normalize_filament_type(slot->type);
        tray["tray_type"]       = type.empty() ? slot->type : type;
        tray["tray_sub_brands"] = slot->name;
        tray["tray_color"]      = slot->color.empty() ? std::string("FFFFFFFF") : hex_to_bbl(slot->color);
        tray["tray_diameter"]   = "1.75";
        tray["tray_weight"]     = "";
        tray["remain"]          = slot->percent ? std::max(0, std::min(100, slot->percent)) : -1;
        if (slot->min_temp)
            tray["nozzle_temp_min"] = std::to_string(slot->min_temp);
        if (slot->max_temp)
            tray["nozzle_temp_max"] = std::to_string(slot->max_temp);
        if (slot->pressure != 0.)
            tray["k"] = std::round(slot->pressure * 10000.) / 10000.;
        tray["cols"]  = json::array({tray["tray_color"]});
        tray["ctype"] = 0;
    } else {
        tray["tray_info_idx"]         = "";
        tray["tray_type"]             = "";
        tray["tray_color"]            = "00000000";
        tray["tray_slot_placeholder"] = "1";
    }
    return tray;
}

json build_ams_json(const PrinterSnapshot& snapshot, const PresetForSlot& preset_for_slot)
{
    json               units           = json::array();
    unsigned long long ams_exist_bits  = 0;
    unsigned long long tray_exist_bits = 0;
    const auto         boxes           = snapshot.cfs_boxes();
    for (std::size_t index = 0; index < boxes.size(); ++index) {
        const CfsBox& box = boxes[index];
        ams_exist_bits |= 1ULL << index;
        std::map<int, const CfsSlot*> by_id;
        int                           max_id = -1;
        for (const CfsSlot& s : box.slots) {
            by_id[s.slot_id] = &s;
            max_id           = std::max(max_id, s.slot_id);
        }
        const int slot_count = std::max(4, max_id + 1);
        json      trays      = json::array();
        for (int slot_id = 0; slot_id < slot_count; ++slot_id) {
            const CfsSlot* slot = by_id.count(slot_id) ? by_id[slot_id] : nullptr;
            if (slot != nullptr && slot->is_loaded() && index * 4 + slot_id < 64)
                tray_exist_bits |= 1ULL << (index * 4 + slot_id);
            json tray = make_tray(slot, std::to_string(slot_id), preset_for_slot);
            trays.push_back(std::move(tray));
        }
        json unit = {{"id", std::to_string(index)}, {"info", "0002"}, {"tray", std::move(trays)}};
        if (box.humidity) {
            unit["humidity"]     = humidity_level(box.humidity);
            unit["humidity_raw"] = std::to_string(int(*box.humidity));
        }
        if (box.temp) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.1f", *box.temp);
            unit["temp"] = buffer;
        }
        units.push_back(std::move(unit));
    }
    return {{"ams", std::move(units)},
            {"ams_exist_bits", hex_bits(ams_exist_bits)},
            {"tray_exist_bits", hex_bits(tray_exist_bits)},
            {"tray_is_bbl_bits", "0"},
            {"tray_read_done_bits", hex_bits(tray_exist_bits)},
            {"tray_reading_bits", "0"},
            {"version", 1},
            {"insert_flag", true},
            {"power_on_flag", false}};
}

json build_push_status(const PrinterSnapshot& s, int sequence_id, const PresetForSlot& preset_for_slot)
{
    json p = {{"command", "push_status"},
              {"msg", 0},
              {"sequence_id", std::to_string(sequence_id)},
              {"support_mqtt_alive", true},
              {"gcode_state", gcode_state(s)},
              {"mc_print_stage", s.device_state == DEVICE_STATE_PRINTING ? "2" : "1"},
              {"mc_print_error_code", "0"},
              {"print_error", 0},
              {"home_flag", 256}, // bits 8-9 = storage state 1 (normal); Orca reads the storage from here too
              {"support_bed_leveling", false},
              {"support_send_to_sd", false},
              {"sdcard", true}, // the printer stores its own G-code: tells Orca's dialogs the storage is present
              {"ams", build_ams_json(s, preset_for_slot)}};
    // External spool holder (box type 1) as Orca's "Ext" tray; an empty tray when the printer lists none.
    const CfsSlot* ext = nullptr;
    for (const CfsBox& box : s.boxes)
        if (box.type == BOX_TYPE_EXTERNAL && !box.slots.empty()) {
            ext = &box.slots.front();
            break;
        }
    p["vt_tray"] = make_tray(ext, "255", preset_for_slot);
    if (s.nozzle_temp)
        p["nozzle_temper"] = *s.nozzle_temp;
    if (s.nozzle_target)
        p["nozzle_target_temper"] = *s.nozzle_target;
    if (s.bed_temp)
        p["bed_temper"] = *s.bed_temp;
    if (s.bed_target)
        p["bed_target_temper"] = *s.bed_target;
    if (s.box_temp)
        p["chamber_temper"] = *s.box_temp;
    if (s.print_progress)
        p["mc_percent"] = *s.print_progress;
    if (s.print_left_seconds)
        p["mc_remaining_time"] = std::max(0, *s.print_left_seconds / 60);
    if (s.layer)
        p["layer_num"] = *s.layer;
    if (s.total_layers)
        p["total_layer_num"] = *s.total_layers;
    if (!s.print_file.empty()) {
        const std::string file = basename(s.print_file);
        p["gcode_file"]        = file;
        const auto dot         = file.find_last_of('.');
        p["subtask_name"]      = dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
    }
    return {{"print", std::move(p)}};
}

json build_get_version(const PrinterSnapshot& s, int sequence_id)
{
    std::string sw = "1.0.0";
    if (!s.firmware.empty()) {
        const std::string marker = "DWIN sw ver:";
        const auto        pos    = s.firmware.rfind(marker);
        sw                       = pos == std::string::npos ? s.firmware : s.firmware.substr(pos + marker.size());
        while (!sw.empty() && sw.back() == ';')
            sw.pop_back();
    }
    return {{"info",
             {{"command", "get_version"},
              {"sequence_id", std::to_string(sequence_id)},
              {"module", json::array({{{"name", "ota"},
                                       {"project_name", s.model.empty() ? std::string("Creality") : s.model},
                                       {"sw_ver", sw},
                                       {"hw_ver", s.model},
                                       {"sn", s.mac}}})}}}};
}

bool ams_setting_to_spec(const json& cmd, const std::vector<PresetInfo>& presets, const std::vector<MaterialLibraryEntry>& library,
                         MaterialSpec& spec, int& ams_index, int& slot_id)
{
    const double ams = to_num(field(cmd, "ams_id"), 0.);
    const json&  slot_field = cmd.contains("slot_id") ? cmd["slot_id"] : field(cmd, "tray_id");
    const double slot = slot_field.is_null() ? 0. : to_num(slot_field, std::nan(""));
    if (std::isnan(ams) || std::isnan(slot))
        return false;
    ams_index = int(ams);
    slot_id   = int(slot);

    const std::string filament_id = str(field(cmd, "tray_info_idx"));
    const std::string tray_type   = str(field(cmd, "tray_type"));
    const std::string color       = str(field(cmd, "tray_color"));
    if (filament_id.empty() && tray_type.empty()) {
        spec = MaterialSpec(); // the AMS dialog's reset
        return true;
    }
    const PresetInfo* preset = nullptr;
    for (const PresetInfo& p : presets)
        if (!p.filament_id.empty() && p.filament_id == filament_id) {
            preset = &p;
            break;
        }
    const int  temp_min = to_int(field(cmd, "nozzle_temp_min"));
    const int  temp_max = to_int(field(cmd, "nozzle_temp_max"));
    PresetInfo adhoc;
    if (preset == nullptr) {
        adhoc.name     = tray_type;
        adhoc.type     = tray_type;
        adhoc.min_temp = temp_min;
        adhoc.max_temp = temp_max;
        preset         = &adhoc;
    }
    spec = spec_from_preset(*preset, color.empty() ? std::string() : normalize_hex(color), library);
    if (temp_min)
        spec.min_temp = temp_min;
    if (temp_max)
        spec.max_temp = temp_max;
    return true;
}

std::string fallback_model_id(const std::string& printer_model)
{
    static const std::map<std::string, std::string> ids = {
        {"Creality K2 Plus", "Creality-K2-Plus"},       {"Creality K2 Pro", "Creality_K2_Pro"},
        {"Creality K2", "Creality_K2"},                 {"Creality K2 SE", "Creality_K2_SE"},
        {"Creality Hi", "Creality-Hi"},                 {"Creality K1", "Creality-K1"},
        {"Creality K1C", "Creality-K1C"},               {"Creality K1 Max", "Creality-K1-Max"},
        {"Creality K1 SE", "Creality-K1-SE"},           {"Creality K1_CFS-C", "Creality_K1_CFS_C"},
        {"Creality K1C_CFS-C", "Creality_K1C_CFS_C"},   {"Creality K1 Max_CFS-C", "Creality_K1_Max_CFS_C"},
        {"Creality K1 SE_CFS-C", "Creality_K1_SE_CFS_C"}};
    auto it = ids.find(printer_model);
    return it == ids.end() ? printer_model : it->second;
}

}} // namespace Slic3r::CrealityCfs
