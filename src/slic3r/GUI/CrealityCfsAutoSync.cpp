// Ported from _AutoSync of the "Creality CFS Sync" OrcaSlicer plugin by Heyde Moura.
#include "CrealityCfsAutoSync.hpp"

#include "CrealityCfsUi.hpp"
#include "CrealityDevicePanel.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"
#include "slic3r/Utils/DeviceActivityLog.hpp"
#include "Zoukei/BackgroundWorkers.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace Slic3r { namespace GUI {

using namespace CrealityCfs;

namespace {

constexpr int       TICK_MS     = 2000;
constexpr long long DEBOUNCE_MS = 2000;
// After a write the CFS reports the new contents a little later: keep the slots the sync wrote meanwhile.
constexpr long long HOLD_MS = 20000;

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string selected_print_host()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    const DynamicPrintConfig* config = nullptr;
    if (bundle->physical_printers.has_selection())
        config = bundle->physical_printers.get_selected_printer_config();
    if (config == nullptr || !config->has("print_host") || config->opt_string("print_host").empty())
        config = &bundle->printers.get_edited_preset().config;
    return config->has("print_host") ? Client::strip_host(config->opt_string("print_host")) : std::string();
}

std::string first_value(const DynamicPrintConfig& config, const char* key)
{
    if (const auto* opt = config.option<ConfigOptionStrings>(key); opt != nullptr && !opt->values.empty())
        return opt->values.front();
    return {};
}

std::vector<ProjectFilament> project_filaments()
{
    std::vector<ProjectFilament> out;
    PresetBundle*                bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    const auto* colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    for (std::size_t i = 0; i < bundle->filament_presets.size(); ++i) {
        ProjectFilament f;
        f.name  = bundle->filament_presets[i];
        f.color = colours != nullptr && i < colours->values.size() ? normalize_hex(colours->values[i], "#FFFFFF") : "#FFFFFF";
        if (const Preset* preset = bundle->filaments.find_preset(f.name)) {
            f.type        = first_value(preset->config, "filament_type");
            f.vendor      = first_value(preset->config, "filament_vendor");
            f.filament_id = preset->filament_id;
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<std::string> signatures(const std::vector<ProjectFilament>& project)
{
    std::vector<std::string> out;
    for (const ProjectFilament& f : project)
        out.push_back(filament_signature(f));
    return out;
}

// The sidebar badge's mapping without its positional fallback, as slot keys; empty while the CFS reports nothing.
std::vector<int> matched_keys(std::size_t n)
{
    const auto matched = CfsUi::matched_slots();
    if (matched.empty())
        return {};
    std::vector<int> out(n, NO_SLOT);
    for (std::size_t i = 0; i < n && i < matched.size(); ++i)
        if (matched[i])
            out[i] = slot_key(matched[i]->first, matched[i]->second);
    return out;
}

std::vector<int> chosen_keys(std::size_t n)
{
    std::vector<int> out(n, NO_SLOT);
    for (std::size_t i = 0; i < n; ++i)
        if (const auto c = CfsUi::chosen_mapped_slot(int(i)))
            out[i] = slot_key(c->first, c->second);
    return out;
}

std::string slot_text(int key)
{
    return key == NO_SLOT ? std::string("none") : "box" + std::to_string(key_box(key)) + "/slot" + std::to_string(key_slot(key));
}

std::string filament_text(const std::vector<ProjectFilament>& project, std::size_t i)
{
    if (i >= project.size())
        return "filament " + std::to_string(i + 1);
    return "filament " + std::to_string(i + 1) + " \"" + project[i].name + "\" " + project[i].type + " " + project[i].color;
}

std::unique_ptr<CrealityCfsAutoSync> s_instance;

} // namespace

void CrealityCfsAutoSync::start()
{
    if (!s_instance)
        s_instance = std::make_unique<CrealityCfsAutoSync>();
}

void CrealityCfsAutoSync::shutdown() { s_instance.reset(); }

CrealityCfsAutoSync::CrealityCfsAutoSync() : m_writing(std::make_shared<std::atomic<bool>>(false))
{
    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); }, m_timer.GetId());
    m_timer.Start(TICK_MS);
}

CrealityCfsAutoSync::~CrealityCfsAutoSync()
{
    m_timer.Stop();
    release_client();
}

void CrealityCfsAutoSync::release_client()
{
    m_client.reset();
    m_have_baseline = false;
    m_has_pending   = false;
}

void CrealityCfsAutoSync::tick()
{
    try {
        AppConfig*    config = wxGetApp().app_config;
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (config == nullptr || bundle == nullptr || wxGetApp().plater() == nullptr)
            return;
        const std::string enabled = config->get(CONFIG_KEY);
        if (!(enabled.empty() || enabled == "true" || enabled == "1") || !CrealityDevicePanel::is_creality_printer(*bundle)) {
            release_client();
            return;
        }
        const std::string host = selected_print_host();
        if (host.empty()) {
            release_client();
            return;
        }
        if (!m_client || m_client->host() != host) {
            // Holding the link keeps it open while we watch.
            m_client        = Client::get(host);
            m_have_baseline = false;
            m_has_pending   = false;
            if (!m_client)
                return;
        }

        // Opening or creating a project: its filament list becomes the baseline; only later edits sync.
        const std::string project_key = wxGetApp().plater()->get_project_filename().ToUTF8().data();
        if (project_key != m_project_key) {
            m_project_key   = project_key;
            m_have_baseline = false;
            m_has_pending   = false;
        }

        const std::vector<ProjectFilament> project = project_filaments();
        const std::string                  sig     = project_signature(project);
        if (!m_have_baseline) {
            m_have_baseline = true;
            m_last_sig      = sig;
            m_last_sigs     = signatures(project);
            m_last_slots    = matched_keys(project.size());
            BOOST_LOG_TRIVIAL(info) << "Creality CFS auto-sync: baseline recorded (" << project.size() << " project filaments)";
            DeviceActivity::sync(host, "baseline: project \"" + project_key + "\" has " + std::to_string(project.size()) +
                                           " filament(s); only later edits are written");
            return;
        }
        const long long now = now_ms();
        if (sig == m_last_sig && !m_has_pending) {
            // the slot every filament occupies before the next edit
            if (now >= m_hold_until_ms)
                if (auto keys = matched_keys(project.size()); !keys.empty())
                    m_last_slots = std::move(keys);
            return;
        }
        if (!m_has_pending || sig != m_pending_sig) {
            m_has_pending      = true;
            m_pending_sig      = sig;
            m_pending_since_ms = now;
            m_pending_project  = project;
            m_blocked_notified = false;
            DeviceActivity::sync(host, "project filaments changed (" + std::to_string(project.size()) + " filament(s)); waiting " +
                                           std::to_string(DEBOUNCE_MS) + " ms of quiet before writing");
            return;
        }
        if (now - m_pending_since_ms < DEBOUNCE_MS)
            return;
        if (sig == m_last_sig) {
            m_has_pending = false;
            return;
        }
        if (m_writing->load())
            return;
        const PrinterSnapshot snap = m_client->snapshot();
        if (!snap.connected || snap.boxes.empty())
            return;
        const std::string reason = check_writable(snap);
        if (!reason.empty()) {
            if (!m_blocked_notified) {
                DeviceActivity::sync(host, "write held: " + reason);
                CrealityCfsAgent::notify("Creality CFS: filament change queued, " + reason + ".");
                m_blocked_notified = true;
            }
            return;
        }
        if (now - m_presets_refreshed_ms > 10000) {
            CrealityCfsAgent::refresh_presets();
            m_presets_refreshed_ms = now;
        }
        // Each edited filament goes to the slot it occupies (or was given), never to "slot i" (see sync_targets).
        const std::vector<std::string> sigs    = signatures(m_pending_project);
        std::vector<int>               matched = matched_keys(sigs.size());
        if (matched.empty())
            matched.assign(sigs.size(), NO_SLOT);
        const SyncTargets targets = sync_targets(m_last_sigs, m_last_slots, sigs, chosen_keys(sigs.size()), matched);
        auto              plan    = plan_project_sync(m_pending_project, CrealityCfsAgent::presets(), snap, targets.write);
        {
            // Why each filament got (or did not get) a slot - same rules as sync_targets, for the console.
            const std::vector<int> chosen = chosen_keys(sigs.size());
            for (std::size_t i = 0; i < sigs.size(); ++i) {
                const int target = i < targets.write.size() ? targets.write[i] : NO_SLOT;
                std::string why;
                if (target != NO_SLOT)
                    why = chosen[i] != NO_SLOT ? "explicit choice" : (i < matched.size() && matched[i] == target) ? "matched (loaded slot of same type+colour)" :
                                                                                                                   "slot it held before the edit";
                else if (std::find(m_last_sigs.begin(), m_last_sigs.end(), sigs[i]) != m_last_sigs.end())
                    why = "unchanged (same filament as at the last sync)";
                else if (chosen[i] == NO_SLOT && (i >= matched.size() || matched[i] == NO_SLOT))
                    why = "not mapped to any slot (no explicit choice, no loaded slot of same type+colour)";
                else
                    why = "target slot already used by another filament";
                DeviceActivity::sync(host, filament_text(m_pending_project, i) + " -> " + (target == NO_SLOT ? std::string("no write") : slot_text(target)) +
                                               " [" + why + "]");
            }
            if (targets.conflicts > 0)
                DeviceActivity::error(host, "conflict: " + std::to_string(targets.conflicts) + " slot(s) claimed by two different filaments; left as they are");
            for (std::size_t i = 0; i < targets.write.size(); ++i)
                if (targets.write[i] != NO_SLOT) {
                    bool planned = false;
                    for (const auto& [slot, spec] : plan)
                        planned = planned || slot_key(slot.box_id, slot.slot_id) == targets.write[i];
                    if (!planned)
                        DeviceActivity::sync(host, slot_text(targets.write[i]) + " skipped: already holds this filament, RFID spool, external spool, unknown slot or written by another filament");
                }
        }
        m_last_sig       = sig;
        m_last_sigs      = sigs;
        m_last_slots     = targets.slots;
        m_hold_until_ms  = now + HOLD_MS;
        m_has_pending    = false;
        if (targets.conflicts > 0)
            CrealityCfsAgent::notify("Creality CFS: " + _u8L("two different filaments are mapped to the same slot; that slot was left as it is."));
        if (plan.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Creality CFS auto-sync: project filaments changed; nothing to write (moved, unmapped or the slot already matches)";
            DeviceActivity::sync(host, "nothing to write (moved, unmapped or the slot already matches)");
            return;
        }
        m_writing->store(true);
        DeviceActivity::sync(host, "writing " + std::to_string(plan.size()) + " slot(s)");
        std::thread([client = m_client, plan = std::move(plan), writing = m_writing] {
            const Zoukei::WorkerToken token;
            std::string ok, bad;
            for (const auto& [slot, spec] : plan) {
                if (!token || Zoukei::shutting_down())
                    break;
                const WriteResult result = client->modify_material(slot.box_id, slot.slot_id, spec);
                BOOST_LOG_TRIVIAL(info) << "Creality CFS auto-sync " << slot.label() << ": " << result.message;
                std::string& list = result.ok ? ok : bad;
                if (!list.empty())
                    list += result.ok ? ", " : "; ";
                list += result.ok ? slot.label() + " " + (spec.is_clear() ? std::string("cleared") : spec.name) :
                                    slot.label() + ": " + result.message;
            }
            writing->store(false);
            if (!ok.empty())
                DeviceActivity::sync(client->host(), "write finished OK: " + ok);
            if (!bad.empty())
                DeviceActivity::error(client->host(), "write FAILED: " + bad);
            if (!ok.empty())
                CrealityCfsAgent::notify("Creality CFS updated: " + ok);
            if (!bad.empty())
                CrealityCfsAgent::notify("Creality CFS sync failed for " + bad, true);
        }).detach();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality CFS auto-sync tick failed: " << e.what();
    }
}

}} // namespace Slic3r::GUI
