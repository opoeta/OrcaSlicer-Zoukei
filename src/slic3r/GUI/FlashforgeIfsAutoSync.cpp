#include "FlashforgeIfsAutoSync.hpp"

#include "CrealityCfsUi.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/FlashforgePrinterAgent.hpp"

#include <boost/log/trivial.hpp>

#include <chrono>
#include <memory>

namespace Slic3r { namespace GUI {

namespace {

constexpr int         TICK_MS     = 2000;
constexpr long long   DEBOUNCE_MS = 2000;
constexpr const char* CONFIG_KEY  = "creality_cfs_auto_sync";

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Fil
{
    FlashforgePrinterAgent::SlotWrite write;
    std::string                       signature;
};

std::vector<Fil> project_filaments()
{
    std::vector<Fil> out;
    PresetBundle*    bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    const auto* colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    for (std::size_t i = 0; i < bundle->filament_presets.size(); ++i) {
        Fil f;
        f.write.name  = bundle->filament_presets[i];
        f.write.color = colours != nullptr && i < colours->values.size() ? colours->values[i] : std::string("#FFFFFF");
        if (const Preset* preset = bundle->filaments.find_preset(f.write.name)) {
            if (const auto* o = preset->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
                f.write.type = o->values.front();
            if (const auto* o = preset->config.option<ConfigOptionStrings>("filament_vendor"); o != nullptr && !o->values.empty())
                f.write.vendor = o->values.front();
            if (const auto* o = preset->config.option<ConfigOptionInts>("nozzle_temperature"); o != nullptr && !o->values.empty())
                f.write.nozzle_temp = o->values.front();
            if (const auto* o = preset->config.option<ConfigOptionInts>("hot_plate_temp"); o != nullptr && !o->values.empty())
                f.write.bed_temp = o->values.front();
        }
        for (char& c : f.write.color)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        if (f.write.color.size() != 7 || f.write.color.front() != '#')
            f.write.color = "#FFFFFF";
        f.signature = f.write.name + "|" + f.write.type + "|" + f.write.color;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<int> current_ports(size_t n)
{
    std::vector<int> ports(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (const auto slot = CfsUi::mapped_slot(int(i)); slot && slot->first == 1)
            ports[i] = slot->second + 1;
    return ports;
}

std::unique_ptr<FlashforgeIfsAutoSync> s_instance;

} // namespace

void FlashforgeIfsAutoSync::start()
{
    if (!s_instance)
        s_instance = std::make_unique<FlashforgeIfsAutoSync>();
}

void FlashforgeIfsAutoSync::shutdown() { s_instance.reset(); }

FlashforgeIfsAutoSync::FlashforgeIfsAutoSync()
{
    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); }, m_timer.GetId());
    m_timer.Start(TICK_MS);
}

FlashforgeIfsAutoSync::~FlashforgeIfsAutoSync() { m_timer.Stop(); }

void FlashforgeIfsAutoSync::tick()
{
    try {
        AppConfig* config = wxGetApp().app_config;
        if (config == nullptr || wxGetApp().preset_bundle == nullptr || wxGetApp().plater() == nullptr)
            return;
        const std::string enabled = config->get(CONFIG_KEY);
        const auto        agent   = CfsUi::ifs_agent();
        if (!(enabled.empty() || enabled == "true" || enabled == "1") || !agent || agent->mode() != FlashforgePrinterAgent::Mode::zmod) {
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
            m_ports         = current_ports(project.size());
            BOOST_LOG_TRIVIAL(info) << "Flashforge IFS auto-sync: baseline recorded (" << project.size() << " project filaments)";
            return;
        }
        if (sigs == m_last && !m_has_pending) {
            m_ports = current_ports(project.size()); // the mapping as it was before the next edit
            return;
        }
        const long long now = now_ms();
        if (!m_has_pending || sigs != m_pending) {
            m_has_pending      = true;
            m_pending          = sigs;
            m_pending_since_ms = now;
            m_blocked_notified = false;
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
                CrealityCfsAgent::notify("Flashforge IFS: filament change queued, " + reason + ".");
                m_blocked_notified = true;
            }
            return;
        }
        std::vector<FlashforgePrinterAgent::SlotWrite> writes;
        const std::vector<int>                         ports_now = current_ports(project.size());
        for (size_t i = 0; i < project.size(); ++i) {
            if (i < m_last.size() && m_last[i] == sigs[i])
                continue;
            int port = i < m_ports.size() ? m_ports[i] : 0;
            if (port == 0)
                port = ports_now[i];
            if (port < 1 || port > 4)
                continue; // external spool or no slot
            auto w = project[i].write;
            w.port = port;
            writes.push_back(std::move(w));
        }
        m_last        = sigs;
        m_has_pending = false;
        if (writes.empty())
            return;
        agent->write_slots(std::move(writes));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Flashforge IFS auto-sync tick failed: " << e.what();
    }
}

}} // namespace Slic3r::GUI
