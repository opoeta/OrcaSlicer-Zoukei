#include "FlashforgeIfsAutoSync.hpp"

#include "CrealityCfsUi.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "GUI_ObjectList.hpp"
#include "NotificationManager.hpp"
#include "SpoolmanUi.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsAgent.hpp"
#include "slic3r/Utils/FlashforgePrinterAgent.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <sstream>
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
        {   // dual / tri / quad / gradient: filament_multi_colour "c1 c2 ..." + filament_colour_type ("0" = gradient)
            const auto* mc = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
            const auto* ty = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
            if (mc != nullptr && i < mc->values.size()) {
                std::istringstream is(mc->values[i]);
                std::vector<std::string> cs;
                for (std::string t; is >> t;) {
                    for (char& c : t)
                        c = char(std::toupper(static_cast<unsigned char>(c)));
                    if (t.size() == 7 && t.front() == '#')
                        cs.push_back(t);
                }
                if (cs.size() >= 2 && cs.size() <= 4 && cs.front() == f.write.color) {
                    f.write.colors   = cs;
                    f.write.gradient = ty != nullptr && i < ty->values.size() && ty->values[i] == "0";
                }
            }
        }
        f.signature = f.write.name + "|" + f.write.type + "|" + f.write.color;
        for (const std::string& c : f.write.colors)
            f.signature += (f.write.gradient ? "g" : "m") + c;
        {   // raw multi-colour project values: any edit of them (type, colours, gradient length) is a change
            const auto* mc = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
            const auto* ty = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
            const auto* gl = bundle->project_config.option<ConfigOptionFloats>("filament_gradient_length");
            f.signature += "|" + (mc != nullptr && i < mc->values.size() ? mc->values[i] : std::string()) + "|" +
                           (ty != nullptr && i < ty->values.size() ? ty->values[i] : std::string()) + "|" +
                           (gl != nullptr && i < gl->values.size() ? std::to_string(gl->values[i]) : std::string());
        }
        {   // the Spoolman spool of the filament goes to the lane together with its colour and material
            const int spool = SpoolmanUi::project_spool_id(int(i));
            if (spool > 0) {
                f.write.spool_id = spool;
                if (Spoolman::Spool s; Spoolman::cached_spool(spool, s)) {
                    f.write.spool_filament_id = s.filament_id;
                    f.write.spool_vendor_id   = s.vendor_id;
                    f.write.spool_vendor      = s.vendor;
                    f.write.spool_name        = s.name;
                }
            }
            f.signature += "|spool" + std::to_string(spool);
        }
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

std::string base_material(std::string t)
{
    boost::to_upper(t);
    return t == "SILK" || t == "PLA-SILK" ? std::string("PLA") : t;
}

std::string filament_type_of(const Preset* p)
{
    if (p != nullptr)
        if (const auto* o = p->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
            return o->values.front();
    return {};
}

// Closest compatible preset of the given type: a "Generic <type>" one first.
std::string closest_preset(PresetBundle& bundle, const std::string& type)
{
    std::string best;
    for (const Preset& p : bundle.filaments) {
        if (!p.is_visible || !p.is_compatible || !boost::iequals(filament_type_of(&p), type))
            continue;
        if (boost::icontains(p.name, "generic") && !boost::icontains(p.name, "silk"))
            return p.name;
        if (best.empty())
            best = p.name;
    }
    return best;
}

std::unique_ptr<FlashforgeIfsAutoSync> s_instance;

} // namespace

void FlashforgeIfsAutoSync::start()
{
    if (!s_instance)
        s_instance = std::make_unique<FlashforgeIfsAutoSync>();
}

void FlashforgeIfsAutoSync::shutdown() { s_instance.reset(); }

void FlashforgeIfsAutoSync::rebaseline()
{
    if (s_instance) {
        s_instance->m_have_baseline = false;
        s_instance->m_has_pending   = false;
    }
}

int FlashforgeIfsAutoSync::apply_slot_to_project(int port, const std::string& type, const std::string& color, const std::string& preset,
                                                 const std::vector<std::string>& multi, bool gradient)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || port < 1 || port > 4)
        return 0;
    auto* colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colours == nullptr)
        return 0;
    std::string hex = color;
    for (char& c : hex)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    int changed = 0;
    for (size_t i = 0; i < bundle->filament_presets.size() && i < colours->values.size(); ++i) {
        const auto slot = CfsUi::mapped_slot(int(i));
        if (!slot || slot->first != 1 || slot->second + 1 != port)
            continue;
        std::string target = preset;
        if (target.empty() && !type.empty()) {
            const std::string want = base_material(type);
            if (!boost::iequals(filament_type_of(bundle->filaments.find_preset(bundle->filament_presets[i])), want))
                target = closest_preset(*bundle, want);
        }
        bool touched = false;
        if (!target.empty() && target != bundle->filament_presets[i] && bundle->filaments.find_preset(target) != nullptr) {
            bundle->set_filament_preset(i, target);
            touched = true;
        }
        if (!hex.empty() && boost::to_upper_copy(colours->values[i]) != hex) {
            colours->values[i] = hex;
            touched            = true;
        }
        // multi colours: only touch the lists when the project filament already has them or the slot has them
        auto* mcol = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
        auto* ctyp = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
        if (mcol != nullptr && ctyp != nullptr && i < mcol->values.size() && i < ctyp->values.size()) {
            const bool is_multi = multi.size() >= 2 && multi.size() <= 4 && multi.front() == hex;
            std::string want_c, want_t = "1";
            if (is_multi) {
                for (const std::string& c : multi)
                    want_c += (want_c.empty() ? "" : " ") + c;
                if (gradient)
                    want_t = "0";
            }
            if (is_multi || !mcol->values[i].empty() || ctyp->values[i] != "1") {
                std::string cur = mcol->values[i];
                for (char& c : cur)
                    c = char(std::toupper(static_cast<unsigned char>(c)));
                if (cur != want_c || ctyp->values[i] != want_t) {
                    mcol->values[i] = want_c;
                    ctyp->values[i] = want_t;
                    touched         = true;
                }
            }
        }
        changed += touched ? 1 : 0;
    }
    if (changed > 0) {
        Plater* plater = wxGetApp().plater();
        plater->update_filament_colors_in_full_config();
        wxGetApp().sidebar().update_presets(Preset::TYPE_FILAMENT);
        wxGetApp().sidebar().update_dynamic_filament_list();
        if (wxGetApp().obj_list() != nullptr)
            wxGetApp().obj_list()->update_filament_colors();
        plater->update_project_dirty_from_presets();
        bundle->export_selections(*wxGetApp().app_config);
    }
    rebaseline(); // the next tick records this state: the project -> printer sync must not echo it back
    return changed;
}

std::string FlashforgeIfsAutoSync::apply_all_to_project()
{
    std::string out;
    if (const auto agent = CfsUi::ifs_agent())
        for (const auto& s : agent->ifs_slots())
            if (s.has_filament)
                if (apply_slot_to_project(s.port, s.material, s.color, "", s.colors, s.gradient) > 0)
                    out += (out.empty() ? "" : ", ") + std::string("IFS") + std::to_string(s.port) + " " + s.material + " " + s.color;
    return out;
}

// Printer -> Orca: a slot that changed on the printer (HelixScreen, Mainsail, the screen) and no longer matches the project
// filament mapped to it raises a notification with a "Sincronizar" link (or applies at once with the opt-in preference).
void FlashforgeIfsAutoSync::check_printer_changes(const std::shared_ptr<FlashforgePrinterAgent>& agent)
{
    std::map<int, std::string> now;
    std::map<int, FlashforgePrinterAgent::IfsSlot> slots;
    for (const auto& s : agent->ifs_slots()) {
        now[s.port]   = s.has_filament ? boost::to_upper_copy(s.material) + "|" + boost::to_upper_copy(s.color) : std::string();
        if (s.has_filament)
            for (const std::string& c : s.colors)
                now[s.port] += (s.gradient ? "g" : "m") + c;
        slots[s.port] = s;
    }
    if (now.empty())
        return;
    const std::string dev = agent->get_user_selected_machine();
    if (dev != m_printer_dev || m_printer_last.empty()) {
        m_printer_dev  = dev;
        m_printer_last = now;
        return;
    }
    const std::string auto_apply = wxGetApp().app_config->get("flashforge_apply_printer_changes");
    for (const auto& port_sig : now) {
        const int   port = port_sig.first;
        const auto& sig  = port_sig.second;
        if (m_printer_last[port] == sig)
            continue;
        m_printer_last[port] = sig;
        const auto& s = slots[port];
        if (!s.has_filament)
            continue;
        // does a mapped project filament already match?
        PresetBundle* bundle  = wxGetApp().preset_bundle;
        const auto*   colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        bool          mapped = false, differs = false;
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            const auto slot = CfsUi::mapped_slot(int(i));
            if (!slot || slot->first != 1 || slot->second + 1 != port)
                continue;
            mapped = true;
            const std::string type = filament_type_of(bundle->filaments.find_preset(bundle->filament_presets[i]));
            if (colours == nullptr || i >= colours->values.size() || !boost::iequals(colours->values[i], s.color) ||
                !boost::iequals(type, base_material(s.material)))
                differs = true;
            else if (const auto* mc = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
                     mc != nullptr && i < mc->values.size()) {
                std::string want;
                if (s.colors.size() >= 2)
                    for (const std::string& c : s.colors)
                        want += (want.empty() ? "" : " ") + c;
                if (!boost::iequals(mc->values[i], want))
                    differs = true;
            }
        }
        if (!mapped || !differs)
            continue;
        const std::string type = s.material, color = s.color;
        const auto        cols = s.colors;
        const bool        grad = s.gradient;
        if (auto_apply == "true" || auto_apply == "1") {
            apply_slot_to_project(port, type, color, "", cols, grad);
            CrealityCfsAgent::notify("IFS " + std::to_string(port) + " mudou na impressora - projeto atualizado.");
            continue;
        }
        if (Plater* plater = wxGetApp().plater(); plater != nullptr && plater->get_notification_manager() != nullptr)
            plater->get_notification_manager()->push_notification(
                NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
                "Slot IFS " + std::to_string(port) + " mudou na impressora (" + s.material + " " + s.color + ")", "Sincronizar",
                [port, type, color, cols, grad](wxEvtHandler*) {
                    apply_slot_to_project(port, type, color, "", cols, grad);
                    return true;
                });
    }
}

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
        if (agent && agent->mode() == FlashforgePrinterAgent::Mode::zmod)
            check_printer_changes(agent);
        else
            m_printer_last.clear();
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
