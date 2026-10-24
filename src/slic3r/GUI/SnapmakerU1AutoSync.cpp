#include "SnapmakerU1AutoSync.hpp"
#include "FlashforgeIfsAutoSync.hpp"

#include "CrealityCfsUi.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "NotificationManager.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfs.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/DeviceActivityLog.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <cctype>
#include <chrono>
#include <exception>
#include <memory>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

constexpr int         TICK_MS     = 2000;
constexpr long long   DEBOUNCE_MS = 2000;
constexpr long long   HOLD_MS     = 20000; // the heads report what was written a little later
constexpr const char* CONFIG_KEY  = "creality_cfs_auto_sync";

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Fil
{
    SnapmakerPrinterAgent::HeadWrite write;
    std::string                      signature;
};

std::string upper_copy(std::string s)
{
    for (char& c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The project's filaments as head writes: preset name (-> sub type), type, vendor, colours.
std::vector<Fil> project_filaments()
{
    std::vector<Fil> out;
    PresetBundle*    bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    const auto* colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const auto* mc      = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
    const auto* ty      = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
    const auto* gl      = bundle->project_config.option<ConfigOptionFloats>("filament_gradient_length");
    for (std::size_t i = 0; i < bundle->filament_presets.size(); ++i) {
        Fil f;
        f.write.name  = bundle->filament_presets[i];
        f.write.color = colours != nullptr && i < colours->values.size() ? colours->values[i] : std::string("#FFFFFF");
        if (const Preset* preset = bundle->filaments.find_preset(f.write.name)) {
            if (const auto* o = preset->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
                f.write.type = o->values.front();
            if (const auto* o = preset->config.option<ConfigOptionStrings>("filament_vendor"); o != nullptr && !o->values.empty())
                f.write.vendor = o->values.front();
        }
        f.write.color = upper_copy(f.write.color);
        if (f.write.color.size() != 7 || f.write.color.front() != '#')
            f.write.color = "#FFFFFF";
        // dual / tri / quad / gradient: filament_multi_colour "c1 c2 ..." + filament_colour_type ("0" = gradient)
        if (mc != nullptr && i < mc->values.size()) {
            std::istringstream       is(mc->values[i]);
            std::vector<std::string> cs;
            for (std::string t; is >> t;) {
                t = upper_copy(t);
                if (t.size() == 7 && t.front() == '#')
                    cs.push_back(t);
            }
            if (cs.size() >= 2 && cs.size() <= 4 && cs.front() == f.write.color) {
                f.write.colors   = cs;
                f.write.gradient = ty != nullptr && i < ty->values.size() && ty->values[i] == "0";
            }
        }
        f.signature = f.write.name + "|" + f.write.type + "|" + f.write.color;
        for (const std::string& c : f.write.colors)
            f.signature += (f.write.gradient ? "g" : "m") + c;
        // raw multi-colour project values: any edit of them (type, colours, gradient length) is a change
        f.signature += "|" + (mc != nullptr && i < mc->values.size() ? mc->values[i] : std::string()) + "|" +
                       (ty != nullptr && i < ty->values.size() ? ty->values[i] : std::string()) + "|" +
                       (gl != nullptr && i < gl->values.size() ? std::to_string(gl->values[i]) : std::string());
        out.push_back(std::move(f));
    }
    return out;
}

// Head (0-3) each project filament is mapped to by its sidebar badge WITHOUT the positional fallback (explicit choice or
// same type + colour), -1 = none; empty while the printer reports no heads.
std::vector<int> current_heads(size_t n)
{
    const auto matched = CfsUi::matched_slots();
    if (matched.empty())
        return {};
    std::vector<int> heads(n, -1);
    for (size_t i = 0; i < n && i < matched.size(); ++i)
        if (matched[i] && matched[i]->first == 1)
            heads[i] = matched[i]->second;
    return heads;
}

// Head the user picked for each filament, -1 = automatic.
std::vector<int> chosen_heads(size_t n)
{
    std::vector<int> heads(n, -1);
    for (size_t i = 0; i < n; ++i)
        if (const auto c = CfsUi::chosen_mapped_slot(int(i)); c && c->first == 1)
            heads[i] = c->second;
    return heads;
}

std::string filament_type_of(const Preset* p)
{
    if (p != nullptr)
        if (const auto* o = p->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
            return o->values.front();
    return {};
}

std::unique_ptr<SnapmakerU1AutoSync> s_instance;

} // namespace

void SnapmakerU1AutoSync::start()
{
    if (!s_instance)
        s_instance = std::make_unique<SnapmakerU1AutoSync>();
}

void SnapmakerU1AutoSync::shutdown() { s_instance.reset(); }

void SnapmakerU1AutoSync::rebaseline()
{
    if (s_instance) {
        s_instance->m_have_baseline = false;
        s_instance->m_has_pending   = false;
    }
}

std::string SnapmakerU1AutoSync::apply_all_to_project()
{
    std::string out;
    if (const auto agent = CfsUi::u1_agent()) {
        const auto state = agent->u1_state();
        for (const auto& h : state.heads)
            if (h.present && !h.type.empty())
                if (FlashforgeIfsAutoSync::apply_slot_to_project(h.index + 1, h.type, h.color, "", h.colors, h.color_mode == 1) > 0)
                    out += (out.empty() ? "" : ", ") + std::string("T") + std::to_string(h.index + 1) + " " + h.type + " " + h.color;
    }
    rebaseline();
    return out;
}

// Printer -> Orca: a head that changed on the printer (its screen, Fluidd, an NFC spool) and no longer matches the project
// filament mapped to it raises a notification with a "Sync" link (or applies at once with the opt-in preference).
void SnapmakerU1AutoSync::check_printer_changes(const std::shared_ptr<SnapmakerPrinterAgent>& agent)
{
    const auto state = agent->u1_state();
    if (!state.valid)
        return;
    std::map<int, std::string> now;
    for (const auto& h : state.heads) {
        now[h.index] = h.present ? boost::to_upper_copy(h.type) + "|" + boost::to_upper_copy(h.color) : std::string();
        if (h.present)
            for (const std::string& c : h.colors)
                now[h.index] += (h.color_mode == 1 ? "g" : "m") + c;
    }
    if (m_printer_last.empty()) {
        m_printer_last = now;
        return;
    }
    const std::string auto_apply = wxGetApp().app_config->get("snapmaker_u1_apply_printer_changes");
    for (const auto& [index, sig] : now) {
        if (m_printer_last[index] == sig)
            continue;
        m_printer_last[index] = sig;
        const auto& h = state.heads[size_t(index)];
        if (!h.present || h.type.empty())
            continue;
        // does a mapped project filament already match?
        PresetBundle* bundle  = wxGetApp().preset_bundle;
        const auto*   colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        bool          mapped = false, differs = false;
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            const auto slot = CfsUi::mapped_slot(int(i));
            if (!slot || slot->first != 1 || slot->second != index)
                continue;
            mapped = true;
            const std::string type = filament_type_of(bundle->filaments.find_preset(bundle->filament_presets[i]));
            if (colours == nullptr || i >= colours->values.size() || !boost::iequals(colours->values[i], h.color) || !boost::iequals(type, h.type))
                differs = true;
            else if (const auto* mc = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour"); mc != nullptr && i < mc->values.size()) {
                std::string want;
                for (const std::string& c : h.colors)
                    want += (want.empty() ? "" : " ") + c;
                if (!boost::iequals(mc->values[i], want))
                    differs = true;
            }
        }
        if (!mapped || !differs)
            continue;
        const int         port = index + 1;
        const std::string type = h.type, color = h.color;
        const auto        cols = h.colors;
        const bool        grad = h.color_mode == 1;
        if (auto_apply == "true" || auto_apply == "1") {
            FlashforgeIfsAutoSync::apply_slot_to_project(port, type, color, "", cols, grad);
            rebaseline();
            CrealityCfsAgent::notify(_u8L("Snapmaker U1") + " T" + std::to_string(port) + ": " + _u8L("changed on the printer - project updated."));
            continue;
        }
        if (Plater* plater = wxGetApp().plater(); plater != nullptr && plater->get_notification_manager() != nullptr)
            plater->get_notification_manager()->push_notification(
                NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("Snapmaker U1") + " T" + std::to_string(port) + ": " + _u8L("changed on the printer") + " (" + h.type + " " + h.color + ")", _u8L("Sync"),
                [port, type, color, cols, grad](wxEvtHandler*) {
                    FlashforgeIfsAutoSync::apply_slot_to_project(port, type, color, "", cols, grad);
                    SnapmakerU1AutoSync::rebaseline();
                    return true;
                });
    }
}

SnapmakerU1AutoSync::SnapmakerU1AutoSync()
{
    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); }, m_timer.GetId());
    m_timer.Start(TICK_MS);
}

SnapmakerU1AutoSync::~SnapmakerU1AutoSync() { m_timer.Stop(); }

void SnapmakerU1AutoSync::tick()
{
    try {
        AppConfig* config = wxGetApp().app_config;
        if (config == nullptr || wxGetApp().preset_bundle == nullptr || wxGetApp().plater() == nullptr)
            return;
        const auto agent = CfsUi::u1_agent();
        if (!agent) {
            m_printer_last.clear();
            m_have_baseline = false;
            m_has_pending   = false;
            return;
        }
        check_printer_changes(agent);
        const std::string enabled = config->get(CONFIG_KEY);
        if (!(enabled.empty() || enabled == "true" || enabled == "1")) {
            m_have_baseline = false;
            m_has_pending   = false;
            return;
        }
        // Opening or creating a project: its filament list becomes the baseline; only later edits sync.
        const std::string project_key = wxGetApp().plater()->get_project_filename().ToUTF8().data();
        if (project_key != m_project_key) {
            m_project_key   = project_key;
            m_have_baseline = false;
            m_has_pending   = false;
        }
        const std::vector<Fil>   project = project_filaments();
        std::vector<std::string> sigs;
        for (const Fil& f : project)
            sigs.push_back(f.signature);
        if (!m_have_baseline) {
            m_have_baseline = true;
            m_last          = sigs;
            m_heads         = current_heads(project.size());
            BOOST_LOG_TRIVIAL(info) << "Snapmaker U1 auto-sync: baseline recorded (" << project.size() << " project filaments)";
            DeviceActivity::sync(CfsUi::activity_host(), "U1 baseline: " + std::to_string(project.size()) + " project filament(s); only later edits are written");
            return;
        }
        const long long now = now_ms();
        if (sigs == m_last && !m_has_pending) {
            if (now >= m_hold_until_ms) // the mapping as it was before the next edit
                if (auto heads = current_heads(project.size()); !heads.empty())
                    m_heads = std::move(heads);
            return;
        }
        if (!m_has_pending || sigs != m_pending) {
            m_has_pending      = true;
            m_pending          = sigs;
            m_pending_since_ms = now;
            m_blocked_notified = false;
            DeviceActivity::sync(CfsUi::activity_host(), "U1: project filaments changed (" + std::to_string(project.size()) + "); waiting for a quiet period");
            return;
        }
        if (now - m_pending_since_ms < DEBOUNCE_MS)
            return;
        if (sigs == m_last) {
            m_has_pending = false;
            return;
        }
        if (const std::string reason = agent->write_block_reason(); !reason.empty()) {
            if (!m_blocked_notified) {
                DeviceActivity::sync(CfsUi::activity_host(), "U1 write held: " + reason);
                CrealityCfsAgent::notify(_u8L("Snapmaker U1") + ": " + _u8L("filament change queued") + ", " + reason + ".");
                m_blocked_notified = true;
            }
            return;
        }
        // Only edited filaments, each to the head it occupies (or was given); moved/removed filaments and unmapped ones
        // write nothing (same rule as the Creality CFS sync, CrealityCfs::sync_targets).
        std::vector<SnapmakerPrinterAgent::HeadWrite> writes;
        std::vector<int>                              heads_now = current_heads(project.size());
        if (heads_now.empty())
            heads_now.assign(project.size(), -1);
        const CrealityCfs::SyncTargets targets = CrealityCfs::sync_targets(m_last, m_heads, sigs, chosen_heads(project.size()), heads_now);
        {
            const std::vector<int> chosen = chosen_heads(project.size());
            for (size_t i = 0; i < project.size(); ++i) {
                const int   t   = i < targets.write.size() ? targets.write[i] : -1;
                std::string why = t < 0 ? "unchanged, not mapped or head already used" :
                                  (i < chosen.size() && chosen[i] >= 0) ? "explicit choice" :
                                  (i < heads_now.size() && heads_now[i] == t) ? "matched (head with same type+colour)" : "head it held before the edit";
                DeviceActivity::sync(CfsUi::activity_host(), "U1 filament " + std::to_string(i + 1) + " [" + sigs[i] + "] -> " +
                                                                 (t < 0 ? std::string("no write") : "head T" + std::to_string(t + 1)) + " [" + why + "]");
            }
            if (targets.conflicts > 0)
                DeviceActivity::error(CfsUi::activity_host(), "U1 conflict: " + std::to_string(targets.conflicts) + " head(s) claimed by two different filaments; left as they are");
        }
        for (size_t i = 0; i < project.size(); ++i) {
            const int head = targets.write[i];
            if (head < 0 || head >= SnapmakerPrinterAgent::HEAD_COUNT)
                continue; // no head for this filament
            auto w = project[i].write;
            w.head = head;
            writes.push_back(std::move(w));
        }
        m_last          = sigs;
        m_heads         = targets.slots;
        m_hold_until_ms = now + HOLD_MS;
        m_has_pending   = false;
        if (targets.conflicts > 0)
            CrealityCfsAgent::notify(_u8L("Snapmaker U1") + ": " + _u8L("two different filaments are mapped to the same slot; that slot was left as it is."));
        if (writes.empty())
            return;
        int written = 0;
        agent->write_heads(writes, &written); // official and empty heads are skipped by the agent, never forced
        DeviceActivity::sync(CfsUi::activity_host(), "U1 wrote " + std::to_string(written) + " of " + std::to_string(writes.size()) + " head(s)" +
                                                         (written < int(writes.size()) ? " (official or empty heads are never forced)" : ""));
        if (written < int(writes.size()))
            CrealityCfsAgent::notify(_u8L("Snapmaker U1") + ": " + _u8L("official or empty heads were left as they are."));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker U1 auto-sync tick failed: " << e.what();
    }
}

}} // namespace Slic3r::GUI
