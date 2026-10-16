// Ported from _AutoSync of the "Creality CFS Sync" OrcaSlicer plugin by Heyde Moura.
#include "CrealityCfsAutoSync.hpp"

#include "CrealityDevicePanel.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"

#include <boost/log/trivial.hpp>

#include <chrono>
#include <thread>

namespace Slic3r { namespace GUI {

using namespace CrealityCfs;

namespace {

constexpr int       TICK_MS     = 2000;
constexpr long long DEBOUNCE_MS = 2000;

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
            BOOST_LOG_TRIVIAL(info) << "Creality CFS auto-sync: baseline recorded (" << project.size() << " project filaments)";
            return;
        }
        if (sig == m_last_sig && !m_has_pending)
            return;
        const long long now = now_ms();
        if (!m_has_pending || sig != m_pending_sig) {
            m_has_pending      = true;
            m_pending_sig      = sig;
            m_pending_since_ms = now;
            m_pending_project  = project;
            m_blocked_notified = false;
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
                CrealityCfsAgent::notify("Creality CFS: filament change queued, " + reason + ".");
                m_blocked_notified = true;
            }
            return;
        }
        if (now - m_presets_refreshed_ms > 10000) {
            CrealityCfsAgent::refresh_presets();
            m_presets_refreshed_ms = now;
        }
        auto plan     = plan_project_sync(m_pending_project, CrealityCfsAgent::presets(), snap);
        m_last_sig    = sig;
        m_has_pending = false;
        if (plan.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Creality CFS auto-sync: project filaments changed but the CFS already matches";
            return;
        }
        m_writing->store(true);
        std::thread([client = m_client, plan = std::move(plan), writing = m_writing] {
            std::string ok, bad;
            for (const auto& [slot, spec] : plan) {
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
                CrealityCfsAgent::notify("Creality CFS updated: " + ok);
            if (!bad.empty())
                CrealityCfsAgent::notify("Creality CFS sync failed for " + bad, true);
        }).detach();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality CFS auto-sync tick failed: " << e.what();
    }
}

}} // namespace Slic3r::GUI
