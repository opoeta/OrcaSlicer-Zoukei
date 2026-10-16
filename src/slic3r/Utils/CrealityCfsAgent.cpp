// Ported from the CrealityCfsAgent of the "Creality CFS Sync" plugin by Heyde Moura (creality_cfs.py).
#include "CrealityCfsAgent.hpp"

#include "CrealityCfsClient.hpp"
#include "CrealityPrint.hpp"
#include "Http.hpp"
#include "slic3r/GUI/CrealityDevice/DeviceStore.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/log/trivial.hpp>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <chrono>
#include <map>
#include <thread>

namespace Slic3r {

using json = nlohmann::json;
using namespace CrealityCfs;

namespace {

constexpr const char* AGENT_VERSION = "1.0.0";

std::mutex              s_presets_mutex;
std::vector<PresetInfo> s_presets;

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string first_string(const DynamicPrintConfig& config, const char* key)
{
    if (const auto* opt = config.option<ConfigOptionStrings>(key); opt != nullptr && !opt->values.empty())
        return opt->values.front();
    if (const auto* opt = config.option<ConfigOptionString>(key))
        return opt->value;
    return {};
}

int first_int(const DynamicPrintConfig& config, const char* key)
{
    if (const auto* opt = config.option<ConfigOptionInts>(key); opt != nullptr && !opt->values.empty())
        return opt->values.front();
    return 0;
}

double first_float(const DynamicPrintConfig& config, const char* key)
{
    if (const auto* opt = config.option<ConfigOptionFloats>(key); opt != nullptr && !opt->values.empty())
        return opt->values.front();
    return 0.;
}

} // namespace

// ---- shared preset data --------------------------------------------------------------------------

void CrealityCfsAgent::refresh_presets()
{
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    std::vector<PresetInfo> out;
    for (const Preset& preset : bundle->filaments.get_presets()) {
        if (preset.is_default)
            continue;
        PresetInfo info;
        info.name             = preset.name;
        info.vendor           = first_string(preset.config, "filament_vendor");
        info.type             = first_string(preset.config, "filament_type");
        info.filament_id      = preset.filament_id;
        info.min_temp         = first_int(preset.config, "nozzle_temperature_range_low");
        info.max_temp         = first_int(preset.config, "nozzle_temperature_range_high");
        info.pressure_advance = first_float(preset.config, "pressure_advance");
        const std::string color = first_string(preset.config, "default_filament_colour");
        info.color            = color.empty() ? std::string() : normalize_hex(color, "");
        info.is_system        = preset.is_system;
        info.is_visible       = preset.is_visible;
        info.is_compatible    = preset.is_compatible;
        out.push_back(std::move(info));
    }
    std::lock_guard<std::mutex> lock(s_presets_mutex);
    s_presets = std::move(out);
}

std::vector<PresetInfo> CrealityCfsAgent::presets()
{
    std::lock_guard<std::mutex> lock(s_presets_mutex);
    return s_presets;
}

std::string CrealityCfsAgent::current_model_id()
{
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    const Preset&     printer = bundle->printers.get_edited_preset();
    const std::string model   = printer.config.has("printer_model") ? printer.config.opt_string("printer_model") : std::string();
    if (model.empty())
        return {};
    // Orca compares the device's model with the vendor profile's model_id, not with the name;
    // a mismatch makes the sidebar sync bail out silently.
    for (const auto& [vendor_id, vendor] : bundle->vendors)
        for (const auto& printer_model : vendor.models)
            if (printer_model.name == model || printer_model.id == model)
                return !printer_model.model_id.empty() ? printer_model.model_id : fallback_model_id(model);
    return fallback_model_id(model);
}

void CrealityCfsAgent::notify(const std::string& text, bool error)
{
    BOOST_LOG_TRIVIAL(info) << "Creality CFS: " << text;
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([text, error] {
        if (GUI::Plater* plater = GUI::wxGetApp().plater(); plater != nullptr && plater->get_notification_manager() != nullptr)
            plater->get_notification_manager()->push_notification(GUI::NotificationType::CustomNotification,
                                                                  error ? GUI::NotificationManager::NotificationLevel::ErrorNotificationLevel :
                                                                          GUI::NotificationManager::NotificationLevel::RegularNotificationLevel,
                                                                  text);
    });
}

// ---- agent ---------------------------------------------------------------------------------------

CrealityCfsAgent::CrealityCfsAgent(std::string /*log_dir*/) {}

CrealityCfsAgent::~CrealityCfsAgent() { disconnect_printer(); }

AgentInfo CrealityCfsAgent::get_agent_info_static()
{
    return AgentInfo{CREALITY_CFS_PRINTER_AGENT_ID, "Creality CFS (LAN)", AGENT_VERSION, "Creality K-series CFS over LAN (port 9999)"};
}

void CrealityCfsAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_cloud_agent = std::move(cloud);
}

int CrealityCfsAgent::connect_printer(const PrinterConnectionParams& params)
{
    const std::string host = Client::strip_host(params.host);
    if (params.dev_id.empty() || host.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Creality CFS agent: connect_printer without dev_id/host";
        return -1;
    }
    disconnect_printer();
    refresh_presets(); // UI thread: MachineObject::connect()
    std::shared_ptr<Client> client = Client::get(host);
    if (!client)
        return -1;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_dev_id             = params.dev_id;
        m_host               = host;
        m_model_id           = current_model_id();
        m_store_name.clear();
        if (const auto device = GUI::CrealityDevice::DeviceStore::Ins().FindByAddress(host))
            m_store_name = device->name;
        m_nozzle_diameter.clear();
        if (PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle)
            if (const auto* nozzle = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
                nozzle != nullptr && !nozzle->values.empty() && nozzle->values.front() > 0.)
                m_nozzle_diameter = (boost::format("%.1f") % nozzle->values.front()).str();
        m_connected_reported = false;
        m_announced          = false;
        m_announced_name.clear();
        m_client             = client;
        std::weak_ptr<CrealityCfsAgent> weak = weak_from_this();
        m_listener = client->add_listener([weak](const json& msg) {
            // The factory creates agents with make_shared; a destroyed agent gets nothing.
            if (auto self = weak.lock())
                self->on_printer_message(msg);
        });
    }
    BOOST_LOG_TRIVIAL(info) << "Creality CFS agent: connecting to " << host << " as " << params.dev_id;
    if (client->snapshot().connected)
        on_printer_message({{"__connected__", true}});
    return BAMBU_NETWORK_SUCCESS;
}

int CrealityCfsAgent::disconnect_printer()
{
    std::shared_ptr<Client> client;
    int                     listener = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        client   = std::move(m_client);
        listener = m_listener;
        m_client.reset();
        m_listener = 0;
        m_dev_id.clear();
    }
    if (client && listener)
        client->remove_listener(listener);
    return BAMBU_NETWORK_SUCCESS;
}

void CrealityCfsAgent::run_on_main(std::function<void()> fn)
{
    QueueOnMainFn queue;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        queue = m_queue_on_main;
    }
    if (queue)
        queue(std::move(fn));
    else
        fn();
}

// Client I/O thread: connection state and status for Orca.
void CrealityCfsAgent::on_printer_message(const json& msg)
{
    std::string             dev_id;
    std::shared_ptr<Client> client;
    OnLocalConnectedFn      local_connect;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        dev_id        = m_dev_id;
        client        = m_client;
        local_connect = m_on_local_connect;
    }
    if (dev_id.empty() || !client)
        return;
    if (msg.contains("__disconnected__")) {
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_connected_reported = false;
        }
        if (local_connect) {
            const std::string reason = msg["__disconnected__"].is_string() ? msg["__disconnected__"].get<std::string>() : std::string();
            run_on_main([local_connect, dev_id, reason] { local_connect(2 /* ConnectStatusLost */, dev_id, reason); });
        }
        return;
    }
    bool report = msg.contains("__connected__");
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        report = report || !m_connected_reported;
    }
    if (report) {
        const PrinterSnapshot snap = client->snapshot();
        if (!snap.connected)
            return;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_connected_reported = true;
        }
        announce(snap);
        if (local_connect)
            run_on_main([local_connect, dev_id] { local_connect(0 /* ConnectStatusOk */, dev_id, "0"); });
        publish_status(true);
        return;
    }
    if (msg.contains("hostname"))
        announce(client->snapshot());
    static const char* interesting[] = {"boxsInfo", "deviceState", "state", "nozzleTemp", "bedTemp0", "printProgress", "boxConfig"};
    for (const char* key : interesting)
        if (msg.contains(key)) {
            publish_status(msg.contains("boxsInfo"));
            return;
        }
}

void CrealityCfsAgent::announce(const PrinterSnapshot& snap)
{
    OnMsgArrivedFn ssdp;
    json           payload;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_on_ssdp)
            return;
        // The device name the user knows: the printer's hostname (K2Plus-5D3C), else its entry in the Creality
        // device list, else the model. The hostname often arrives after connecting: announce again when it does.
        const std::string name = !snap.hostname.empty() ? snap.hostname : !m_store_name.empty() ? m_store_name :
                                 !snap.model.empty() ? snap.model : m_host;
        if (m_announced && name == m_announced_name)
            return;
        ssdp    = m_on_ssdp;
        payload = {{"dev_name", name},
                   {"dev_id", m_dev_id},
                   {"dev_ip", m_host},
                   {"dev_type", !m_model_id.empty() ? m_model_id : !snap.model.empty() ? snap.model : std::string("Creality")},
                   {"dev_signal", "0"},
                   {"connect_type", "lan"},
                   {"bind_state", "free"},
                   {"sec_link", "secure"},
                   {"ssdp_version", "v1"}};
        m_announced      = true;
        m_announced_name = name;
        if (m_selected_machine.empty())
            m_selected_machine = m_dev_id;
    }
    const std::string text = payload.dump();
    run_on_main([ssdp, text] { ssdp(text); });
}

void CrealityCfsAgent::publish(const json& message)
{
    OnMessageFn local_fn, cloud_fn;
    std::string dev_id;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        local_fn = m_on_local_message;
        cloud_fn = m_on_message;
        dev_id   = m_dev_id;
    }
    if (dev_id.empty() || (!local_fn && !cloud_fn))
        return;
    const std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    run_on_main([local_fn, cloud_fn, dev_id, text] {
        if (local_fn)
            local_fn(dev_id, text);
        else
            cloud_fn(dev_id, text);
    });
}

void CrealityCfsAgent::publish_status(bool force)
{
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        client = m_client;
    }
    if (!client)
        return;
    const long long now = now_ms();
    if (!force && now - m_last_push_ms.load() < 2000)
        return;
    m_last_push_ms = now;
    const std::vector<PresetInfo> preset_list = presets();
    const PrinterSnapshot         snap        = client->snapshot();
    json status = build_push_status(snap, ++m_sequence, [&preset_list](const CfsSlot& slot) { return match_preset_for_slot(preset_list, slot); });
    std::string nozzle;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        nozzle = m_nozzle_diameter;
    }
    if (!nozzle.empty()) { // lets Orca show the nozzle in the printer card instead of "Unknown"
        status["print"]["nozzle_diameter"] = nozzle;
        status["print"]["nozzle_type"]     = "N/A"; // unknown: keeps the hardness check of Orca inert
    }
    publish(status);
}

int CrealityCfsAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    // No cloud relay for Creality LAN printers.
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

int CrealityCfsAgent::send_message_to_printer(std::string /*dev_id*/, std::string json_str, int /*qos*/, int /*flag*/)
{
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        client = m_client;
    }
    if (!client)
        return -1;
    const json msg = json::parse(json_str, nullptr, false);
    if (msg.is_discarded() || !msg.is_object())
        return -1;
    if (msg.contains("pushing")) { // {"pushing": {"command": "pushall"}}
        publish_status(true);
        return BAMBU_NETWORK_SUCCESS;
    }
    if (msg.contains("info") && msg["info"].is_object() && msg["info"].value("command", std::string()) == "get_version") {
        publish(build_get_version(client->snapshot(), ++m_sequence));
        return BAMBU_NETWORK_SUCCESS;
    }
    if (msg.contains("print") && msg["print"].is_object())
        return handle_print_command(msg["print"]);
    if (msg.contains("system"))
        return BAMBU_NETWORK_SUCCESS; // ledctrl and friends: accepted silently
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int CrealityCfsAgent::handle_print_command(const json& cmd)
{
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        client = m_client;
    }
    if (!client)
        return -1;
    const std::string command = cmd.value("command", std::string());
    if (command == "ams_filament_setting") {
        MaterialSpec spec;
        int          ams_index = 0, slot_id = 0;
        const PrinterSnapshot snap = client->snapshot();
        if (!ams_setting_to_spec(cmd, presets(), snap.library, spec, ams_index, slot_id))
            return -1;
        const auto boxes = snap.cfs_boxes();
        if (ams_index < 0 || ams_index >= int(boxes.size())) {
            BOOST_LOG_TRIVIAL(warning) << "Creality CFS agent: ams_filament_setting for unknown AMS " << ams_index;
            return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
        }
        const int box_id = boxes[ams_index].id;
        std::weak_ptr<CrealityCfsAgent> weak = weak_from_this();
        std::thread([weak, client, box_id, slot_id, spec] {
            const WriteResult result = client->modify_material(box_id, slot_id, spec);
            BOOST_LOG_TRIVIAL(info) << "Creality CFS agent: slot " << box_id << "/" << slot_id << ": " << result.message;
            if (auto self = weak.lock()) {
                bool still_ours;
                {
                    std::lock_guard<std::recursive_mutex> lock(self->m_mutex);
                    still_ours = self->m_client == client;
                }
                if (still_ours)
                    self->publish_status(true);
            }
            if (!result.ok)
                notify("Creality CFS: " + result.message, true);
        }).detach();
        return BAMBU_NETWORK_SUCCESS;
    }
    if (command == "gcode_line") {
        const std::string gcode = cmd.value("param", std::string());
        if (gcode.find_first_not_of(" \t\r\n") == std::string::npos)
            return -1;
        return client->send_gcode(gcode) ? BAMBU_NETWORK_SUCCESS : -1;
    }
    if (command == "pause" || command == "resume" || command == "stop") {
        const json params = command == "pause" ? json{{"pause", 1}} : command == "resume" ? json{{"pause", 0}} : json{{"stop", 1}};
        return client->send("set", params) ? BAMBU_NETWORK_SUCCESS : -1;
    }
    if (command == "set_bed_temp" || command == "set_nozzle_temp") {
        int temp = 0;
        if (cmd.contains("temp") && cmd["temp"].is_number())
            temp = cmd["temp"].get<int>();
        else if (cmd.contains("temp") && cmd["temp"].is_string())
            temp = std::atoi(cmd["temp"].get<std::string>().c_str());
        const json params = command == "set_bed_temp" ? json{{"bedTempControl", {{"num", 0}, {"val", temp}}}} : json{{"nozzleTempControl", temp}};
        return client->send("set", params) ? BAMBU_NETWORK_SUCCESS : -1;
    }
    if (command == "push_status" || command == "get_version") {
        publish_status(true);
        return BAMBU_NETWORK_SUCCESS;
    }
    BOOST_LOG_TRIVIAL(info) << "Creality CFS agent: print command not supported: " << command;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int CrealityCfsAgent::upload_job(const PrintParams& params, const OnUpdateStatusFn& update_fn, const WasCancelledFn& cancel_fn, bool start)
{
    std::shared_ptr<Client> client;
    std::string             host;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        client = m_client;
        host   = m_host;
    }
    auto report = [&](int stage, int code, const std::string& text) {
        if (update_fn)
            update_fn(stage, code, text);
    };
    auto fail = [&](const std::string& text) {
        BOOST_LOG_TRIVIAL(error) << "Creality CFS agent: " << text;
        report(PrintingStageERROR, -1, text);
        return -1;
    };
    if (!client || host.empty())
        return fail("not connected to the printer");

    const json extra = json::parse(params.extra_options, nullptr, false);
    if (extra.is_discarded() || !extra.contains("files") || !extra["files"].is_array() || extra["files"].empty())
        return fail("no G-code file to send");

    // Every project filament -> the slot (CFS box/slot, or the external spool = box 0) the dialog mapped it to.
    std::map<std::string, std::string> info;
    info["enableSelfTest"] = extra.value("self_test", 0) ? "1" : "0";
    if (start) {
        const json            mapping = json::parse(params.ams_mapping_info, nullptr, false);
        const PrinterSnapshot snap    = client->snapshot();
        const auto            boxes   = snap.cfs_boxes();
        const CfsSlot*        ext     = nullptr;
        for (const CfsBox& box : snap.boxes)
            if (box.type == BOX_TYPE_EXTERNAL && !box.slots.empty())
                ext = &box.slots.front();
        int n = 0;
        if (mapping.is_array())
            for (size_t i = 0; i < mapping.size(); ++i) {
                const json& item = mapping[i];
                const int   tray = item.is_object() && item.contains("ams") && item["ams"].is_number_integer() ? item["ams"].get<int>() : -1;
                if (tray < 0)
                    continue; // unmapped
                int         box_id = 0, slot_id = 0;
                std::string type  = item.value("filamentType", std::string());
                std::string color = normalize_hex(item.value("sourceColor", std::string("#FFFFFF")));
                const CfsSlot* slot = nullptr;
                if (tray == 254 || tray == 255) {
                    slot = ext;
                } else if (size_t(tray / 4) < boxes.size()) {
                    box_id  = boxes[size_t(tray / 4)].id;
                    slot_id = tray % 4;
                    slot    = snap.find_slot(box_id, slot_id);
                } else {
                    return fail("filament " + std::to_string(i + 1) + " is mapped to a CFS slot that does not exist");
                }
                if (slot != nullptr) {
                    type = slot->type;
                    if (!slot->color.empty())
                        color = slot->color;
                }
                // The gcode tool of filament i is T<n>A..D; the firmware matches by it, not by the destination slot.
                const std::string tool = "T" + std::to_string(int(i) / 4 + 1) + std::string(1, char('A' + int(i) % 4));
                info["colorMatch_" + std::to_string(n++)] = tool + "\t" + type + "\t" + color + "\t" + std::to_string(box_id) + "\t" + std::to_string(slot_id);
            }
    }

    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host", new ConfigOptionString(host));
    for (const char* key : {"print_host_webui", "printhost_cafile", "printhost_port", "printhost_apikey"})
        cfg.set_key_value(key, new ConfigOptionString(""));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    const CrealityPrint printer(&cfg);

    const size_t total = extra["files"].size();
    report(PrintingStageCreate, 0, "");
    std::string last_error;
    for (size_t k = 0; k < total; ++k) {
        const json&       file = extra["files"][k];
        const std::string path = file.value("path", std::string());
        if (path.empty() || !boost::filesystem::exists(boost::filesystem::path(path)))
            return fail("G-code file not found: " + path);
        PrintHostUpload upload;
        upload.source_path = path;
        upload.upload_path = file.value("name", std::string());
        if (start && k == 0) { // with several plates only the first one is started
            upload.post_action   = PrintHostPostUploadAction::StartPrint;
            upload.extended_info = info;
        }
        bool       cancelled = false;
        const bool ok        = printer.upload(
            upload,
            [&](Http::Progress progress, bool& cancel) {
                if (cancel_fn && cancel_fn())
                    cancel = cancelled = true;
                const int pct = progress.ultotal > 0 ? std::min(100, int(100. * double(progress.ulnow) / double(progress.ultotal))) : 0;
                report(PrintingStageUpload, int((k * 100 + size_t(pct)) / total), std::to_string(pct) + "%");
            },
            [&](wxString error) { last_error = error.ToUTF8().data(); },
            [](wxString, wxString) {});
        if (cancelled || (cancel_fn && cancel_fn()))
            return BAMBU_NETWORK_ERR_CANCELED;
        if (!ok)
            return fail(last_error.empty() ? std::string("upload failed") : last_error);
    }
    report(PrintingStageFinished, 0, "3");
    return BAMBU_NETWORK_SUCCESS;
}

bool CrealityCfsAgent::start_discovery(bool start, bool /*sending*/)
{
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        client = m_client;
    }
    if (start && client) {
        const PrinterSnapshot snap = client->snapshot();
        if (snap.connected)
            announce(snap);
    }
    return true;
}

int CrealityCfsAgent::bind_detect(std::string dev_ip, std::string /*sec_link*/, detectResult& detect)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const PrinterSnapshot snap = m_client ? m_client->snapshot() : PrinterSnapshot();
    detect.dev_id              = m_dev_id.empty() ? dev_ip : m_dev_id;
    detect.model_id            = !m_model_id.empty() ? m_model_id : snap.model;
    detect.dev_name            = snap.hostname.empty() ? dev_ip : snap.hostname;
    detect.version             = snap.firmware;
    detect.connect_type        = "lan";
    detect.bind_state          = "free";
    return BAMBU_NETWORK_SUCCESS;
}

bool CrealityCfsAgent::fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/)
{
    // Subscription mode: the sidebar already has the data; publish again for good measure.
    publish_status(true);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_client != nullptr;
}

std::string CrealityCfsAgent::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_selected_machine.empty() ? m_dev_id : m_selected_machine;
}

int CrealityCfsAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

#define CFS_SETTER(method, member, type)                   \
    int CrealityCfsAgent::method(type fn)                  \
    {                                                      \
        std::lock_guard<std::recursive_mutex> lock(m_mutex); \
        member = std::move(fn);                            \
        return BAMBU_NETWORK_SUCCESS;                      \
    }

CFS_SETTER(set_server_callback, m_on_server_err, OnServerErrFn)
CFS_SETTER(set_on_ssdp_msg_fn, m_on_ssdp, OnMsgArrivedFn)
CFS_SETTER(set_on_printer_connected_fn, m_on_printer_connected, OnPrinterConnectedFn)
CFS_SETTER(set_on_subscribe_failure_fn, m_on_subscribe_failure, GetSubscribeFailureFn)
CFS_SETTER(set_on_message_fn, m_on_message, OnMessageFn)
CFS_SETTER(set_on_user_message_fn, m_on_user_message, OnMessageFn)
CFS_SETTER(set_on_local_connect_fn, m_on_local_connect, OnLocalConnectedFn)
CFS_SETTER(set_on_local_message_fn, m_on_local_message, OnMessageFn)

#undef CFS_SETTER

int CrealityCfsAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Orca clears the callbacks of an agent it switches away from; keep the last valid hop.
    if (fn)
        m_queue_on_main = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
