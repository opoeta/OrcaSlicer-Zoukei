// Ported from CrealityPrinterClient in the "Creality CFS Sync" plugin by Heyde Moura.
#include "CrealityCfsClient.hpp"

#include "Http.hpp"
#include "DeviceActivityLog.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace Slic3r { namespace CrealityCfs {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = asio::ip::tcp;
using json          = nlohmann::json;
using Clock         = std::chrono::steady_clock;

namespace {
constexpr const char* WS_PORT = "9999";
}

struct Client::Impl
{
    explicit Impl(std::string host)
        : host(std::move(host)), work(asio::make_work_guard(io)), resolver(io), poll_timer(io), reconnect_timer(io)
    {
        snapshot.host = this->host;
    }

    // ---- shared state (any thread) ----
    std::string             host;
    mutable std::mutex      mutex;
    std::condition_variable cv;
    PrinterSnapshot         snapshot;
    std::map<std::string, json>        latest;
    std::map<std::string, std::uint64_t> key_seq;
    std::map<int, Listener> listeners;
    int                     next_listener{1};
    std::atomic<bool>       connected{false};
    std::atomic<bool>       stopping{false};
    std::atomic<double>     poll_interval{15.0};

    // ---- I/O thread ----
    asio::io_context                                           io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread                                                thread;
    std::promise<void>                                         thread_done; // set when io.run() has returned
    tcp::resolver                                              resolver;
    asio::steady_timer                                         poll_timer;
    asio::steady_timer                                         reconnect_timer;
    std::unique_ptr<websocket::stream<beast::tcp_stream>>      ws;
    beast::flat_buffer                                         buffer;
    std::deque<std::string>                                    outbox;
    bool                                                       writing{false};
    std::uint64_t                                              generation{0};
    Clock::time_point                                          last_rx{Clock::now()};

    // The thread keeps its own reference, so a link released from inside one of its own
    // callbacks (stop() then detaches) stays alive until the thread has finished.
    void start(std::shared_ptr<Impl> self)
    {
        thread = std::thread([self] {
            try {
                self->io.run();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Creality CFS link " << self->host << ": I/O thread stopped: " << e.what();
            }
            self->thread_done.set_value();
        });
        asio::post(io, [this] { connect(); });
    }

    // Joins the I/O thread, at most `max_wait` when given (the thread owns a reference, so leaving it running is safe:
    // a /info request in connect() can take up to 4 s).
    void stop(std::chrono::milliseconds max_wait = std::chrono::milliseconds::max())
    {
        if (stopping.exchange(true))
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
        }
        cv.notify_all(); // blocked request() / modify_material() callers return now
        asio::post(io, [this] {
            ++generation;
            boost::system::error_code ec;
            poll_timer.cancel();
            reconnect_timer.cancel();
            resolver.cancel();
            if (ws)
                beast::get_lowest_layer(*ws).socket().close(ec);
        });
        work.reset();
        if (thread.joinable()) {
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
            } else if (max_wait == std::chrono::milliseconds::max() ||
                       thread_done.get_future().wait_for(max_wait) == std::future_status::ready) {
                thread.join();
            } else {
                BOOST_LOG_TRIVIAL(warning) << "Creality CFS link " << host << ": I/O thread still busy after " << max_wait.count()
                                           << " ms, detached";
                thread.detach();
            }
        }
    }

    void notify(const json& msg)
    {
        if (stopping)
            return;
        std::vector<Listener> fns;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& [id, fn] : listeners)
                fns.push_back(fn);
        }
        for (auto& fn : fns) {
            try {
                fn(msg);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "Creality CFS listener failed: " << e.what();
            }
        }
    }

    void connect()
    {
        if (stopping)
            return;
        const std::uint64_t gen = ++generation;
        // GET /info gives the model and MAC; optional for the link itself.
        json info;
        try {
            Http::get("http://" + host + "/info")
                .timeout_connect(3)
                .timeout_max(4)
                .on_complete([&](std::string body, unsigned) { info = json::parse(body, nullptr, false); })
                .perform_sync();
        } catch (...) {}
        if (stopping || gen != generation)
            return;
        if (info.is_object()) {
            std::lock_guard<std::mutex> lock(mutex);
            if (info.contains("model") && info["model"].is_string())
                snapshot.model = info["model"].get<std::string>();
            if (info.contains("mac") && info["mac"].is_string())
                snapshot.mac = info["mac"].get<std::string>();
        }

        ws = std::make_unique<websocket::stream<beast::tcp_stream>>(io);
        buffer.consume(buffer.size());
        outbox.clear();
        writing = false;
        resolver.async_resolve(host, WS_PORT, [this, gen](beast::error_code ec, tcp::resolver::results_type results) {
            if (gen != generation)
                return;
            if (ec)
                return fail(gen, "resolve failed: " + ec.message());
            beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(5));
            beast::get_lowest_layer(*ws).async_connect(results, [this, gen](beast::error_code ec, const tcp::endpoint&) {
                if (gen != generation)
                    return;
                if (ec)
                    return fail(gen, "connect failed: " + ec.message());
                beast::get_lowest_layer(*ws).expires_never();
                ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
                ws->set_option(websocket::stream_base::decorator(
                    [](websocket::request_type& req) { req.set(beast::http::field::user_agent, "OrcaSlicer-creality-cfs"); }));
                ws->read_message_max(8 * 1024 * 1024);
                ws->async_handshake(host + ":" + WS_PORT, "/", [this, gen](beast::error_code ec) {
                    if (gen != generation)
                        return;
                    if (ec)
                        return fail(gen, "handshake failed: " + ec.message());
                    on_open(gen);
                });
            });
        });
    }

    void on_open(std::uint64_t gen)
    {
        ws->text(true);
        connected = true;
        last_rx   = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.connected  = true;
            snapshot.last_error.clear();
        }
        BOOST_LOG_TRIVIAL(info) << "Creality CFS link connected to " << host;
        DeviceActivity::ws_event(host, "WebSocket connected (ws://" + host + ":" + WS_PORT + ")");
        notify({{"__connected__", true}});
        queue_write(json({{"method", "get"}, {"params", {{"boxsInfo", 1}, {"boxConfig", 1}, {"reqMaterials", 1}}}}).dump());
        arm_poll(gen);
        do_read(gen);
    }

    void arm_poll(std::uint64_t gen)
    {
        const double interval = std::max(2.0, poll_interval.load());
        poll_timer.expires_after(std::chrono::milliseconds(int(interval * 1000)));
        poll_timer.async_wait([this, gen, interval](beast::error_code ec) {
            if (ec || gen != generation)
                return;
            if (Clock::now() - last_rx > std::chrono::milliseconds(int((interval * 3 + 10) * 1000)))
                return fail(gen, "no traffic from printer, reconnecting");
            queue_write(json({{"method", "get"}, {"params", {{"boxsInfo", 1}}}}).dump());
            arm_poll(gen);
        });
    }

    void do_read(std::uint64_t gen)
    {
        ws->async_read(buffer, [this, gen](beast::error_code ec, std::size_t) {
            if (gen != generation)
                return;
            if (ec)
                return fail(gen, "read failed: " + ec.message());
            std::string text = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            last_rx = Clock::now();
            on_text(text);
            if (gen == generation)
                do_read(gen);
        });
    }

    void on_text(const std::string& text)
    {
        DeviceActivity::ws_message(host, false, text);
        const auto first = text.find_first_not_of(" \t\r\n");
        const bool is_object = first != std::string::npos && text[first] == '{';
        if (text.find("heart_beat") != std::string::npos && !is_object) {
            queue_write("ok");
            return;
        }
        json msg = json::parse(text, nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) {
            if (text.find("heart_beat") != std::string::npos)
                queue_write("ok");
            return;
        }
        if (msg.contains("heart_beat"))
            queue_write("ok");
        {
            std::lock_guard<std::mutex> lock(mutex);
            apply_status(snapshot, msg);
            for (auto it = msg.begin(); it != msg.end(); ++it) {
                latest[it.key()] = it.value();
                ++key_seq[it.key()];
            }
        }
        cv.notify_all();
        notify(msg);
    }

    void queue_write(std::string text)
    {
        DeviceActivity::ws_message(host, true, text);
        outbox.push_back(std::move(text));
        pump();
    }

    void pump()
    {
        if (writing || outbox.empty() || !ws || !connected)
            return;
        writing                 = true;
        const std::uint64_t gen = generation;
        ws->async_write(asio::buffer(outbox.front()), [this, gen](beast::error_code ec, std::size_t) {
            writing = false;
            if (gen != generation)
                return;
            if (ec)
                return fail(gen, "write failed: " + ec.message());
            outbox.pop_front();
            pump();
        });
    }

    void fail(std::uint64_t gen, const std::string& reason)
    {
        if (gen != generation || stopping)
            return;
        ++generation;
        const bool was_connected = connected.exchange(false);
        boost::system::error_code ec;
        poll_timer.cancel();
        if (ws)
            beast::get_lowest_layer(*ws).socket().close(ec);
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.connected  = false;
            snapshot.last_error = reason;
        }
        cv.notify_all();
        if (was_connected)
            BOOST_LOG_TRIVIAL(warning) << "Creality CFS link " << host << " dropped: " << reason;
        DeviceActivity::ws_event(host, (was_connected ? "WebSocket dropped: " : "WebSocket failed: ") + reason + " (retry in 3 s)", true);
        notify({{"__disconnected__", reason}});
        reconnect_timer.expires_after(std::chrono::seconds(3));
        reconnect_timer.async_wait([this](beast::error_code ec) {
            if (!ec && !stopping)
                connect();
        });
    }
};

// ---------------------------------------------------------------------------------------------

namespace {
std::mutex                                     s_registry_mutex;
std::map<std::string, std::weak_ptr<Client>>   s_registry;
bool                                           s_shut_down{false}; // guarded by s_registry_mutex
}

std::string Client::strip_host(const std::string& print_host)
{
    std::string host = print_host;
    const auto  scheme = host.find("://");
    if (scheme != std::string::npos)
        host = host.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    // Keep IPv6 literals intact; drop a :port otherwise.
    if (!host.empty() && host[0] != '[') {
        const auto colon = host.find(':');
        if (colon != std::string::npos)
            host = host.substr(0, colon);
    }
    while (!host.empty() && std::isspace((unsigned char) host.back()))
        host.pop_back();
    while (!host.empty() && std::isspace((unsigned char) host.front()))
        host.erase(host.begin());
    return host;
}

std::shared_ptr<Client> Client::get(const std::string& print_host)
{
    const std::string host = strip_host(print_host);
    if (host.empty())
        return nullptr;
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    if (s_shut_down)
        return nullptr;
    if (auto existing = s_registry[host].lock())
        return existing;
    std::shared_ptr<Client> client(new Client(host));
    s_registry[host] = client;
    client->m_impl->start(client->m_impl);
    return client;
}

void Client::shutdown_all(std::chrono::milliseconds max_wait)
{
    std::vector<std::shared_ptr<Impl>> impls;
    {
        std::lock_guard<std::mutex> lock(s_registry_mutex);
        s_shut_down = true;
        for (auto& [host, weak] : s_registry)
            if (auto client = weak.lock())
                impls.push_back(client->m_impl);
    }
    for (const std::shared_ptr<Impl>& impl : impls)
        impl->stop(max_wait);
    if (!impls.empty())
        BOOST_LOG_TRIVIAL(info) << "Creality CFS: " << impls.size() << " link(s) stopped for shutdown";
}

Client::Client(std::string host) : m_impl(std::make_shared<Impl>(std::move(host))) {}

Client::~Client()
{
    m_impl->stop(std::chrono::seconds(5));
}

const std::string& Client::host() const { return m_impl->host; }

PrinterSnapshot Client::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->snapshot;
}

int Client::add_listener(Listener fn)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const int id = m_impl->next_listener++;
    m_impl->listeners.emplace(id, std::move(fn));
    return id;
}

void Client::remove_listener(int id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->listeners.erase(id);
}

bool Client::send(const std::string& method, const json& params)
{
    m_impl->poll_interval = poll_interval_s;
    if (!m_impl->connected || m_impl->stopping)
        return false;
    std::string text = json({{"method", method}, {"params", params}}).dump();
    Impl*       impl = m_impl.get();
    asio::post(impl->io, [impl, text = std::move(text)]() mutable { impl->queue_write(std::move(text)); });
    return true;
}

json Client::request(const json& params, const std::vector<std::string>& expect, double timeout_s)
{
    std::map<std::string, std::uint64_t> before;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (const auto& key : expect)
            before[key] = m_impl->key_seq[key];
    }
    json result = json::object();
    if (!send("get", params))
        return result;
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->cv.wait_for(lock, std::chrono::milliseconds(int(timeout_s * 1000)), [&] {
        if (!m_impl->connected || m_impl->stopping)
            return true;
        for (const auto& key : expect)
            if (m_impl->key_seq[key] == before[key])
                return false;
        return true;
    });
    for (const auto& key : expect)
        if (m_impl->key_seq[key] != before[key])
            result[key] = m_impl->latest[key];
    return result;
}

WriteResult Client::modify_material(int box_id, int slot_id, const MaterialSpec& spec, bool force, double verify_timeout_s)
{
    WriteResult r = modify_material_impl(box_id, slot_id, spec, force, verify_timeout_s);
    DeviceActivity::add(m_impl->host, r.ok ? DeviceActivity::Source::Agent : DeviceActivity::Source::Error,
                        "modify_material box" + std::to_string(box_id) + "/slot" + std::to_string(slot_id) + ": " + (r.ok ? "OK - " : "FAILED - ") + r.message);
    return r;
}

WriteResult Client::modify_material_impl(int box_id, int slot_id, const MaterialSpec& spec, bool force, double verify_timeout_s)
{
    WriteResult result;
    result.box_id  = box_id;
    result.slot_id = slot_id;
    const std::string     label  = "box" + std::to_string(box_id) + "/slot" + std::to_string(slot_id);
    const PrinterSnapshot snap   = snapshot();
    const CfsSlot*        before = snap.find_slot(box_id, slot_id);
    if (before == nullptr) {
        result.message = "slot " + std::to_string(box_id) + "/" + std::to_string(slot_id) + " does not exist on this printer";
        return result;
    }
    if (before->is_rfid_locked() && !spec.is_clear()) {
        result.message = "slot holds a Creality RFID spool and is read-only";
        return result;
    }
    if (!force) {
        const std::string reason = check_writable(snap);
        if (!reason.empty()) {
            result.message = "refused: " + reason;
            return result;
        }
    }
    DeviceActivity::agent(m_impl->host, "modify_material " + label + ": writing " + spec.vendor + " " + spec.name + " (" + spec.type + ") " + spec.color +
                                            (spec.is_clear() ? " [clear]" : "") + (force ? " [force]" : ""));
    std::uint64_t ack_before;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        ack_before = m_impl->key_seq["modifyMaterial"];
    }
    if (!send("set", spec.to_params(box_id, slot_id))) {
        result.message = "send failed: printer not connected";
        return result;
    }
    json ack;
    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        m_impl->cv.wait_for(lock, std::chrono::milliseconds(2500), [&] { return m_impl->key_seq["modifyMaterial"] != ack_before || !m_impl->connected; });
        if (m_impl->key_seq["modifyMaterial"] != ack_before)
            ack = m_impl->latest["modifyMaterial"];
    }
    // Some firmwares push boxsInfo themselves, others need the request.
    const auto deadline = Clock::now() + std::chrono::milliseconds(int(verify_timeout_s * 1000));
    while (Clock::now() < deadline) {
        const double remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
        request({{"boxsInfo", 1}}, {"boxsInfo"}, std::max(0.5, remaining));
        const PrinterSnapshot after = snapshot();
        if (const CfsSlot* slot = after.find_slot(box_id, slot_id); slot != nullptr && slot_matches(*slot, spec)) {
            result.ok      = true;
            result.message = spec.is_clear() ? std::string("slot cleared") :
                                               "slot set to " + spec.vendor + " " + spec.name + " (" + spec.type + ") " + spec.color;
            return result;
        }
        if (!m_impl->connected)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    result.message = "printer did not apply the change" +
                     (ack.is_null() ? std::string(" (no modifyMaterial acknowledgement)") : " (printer answered modifyMaterial=" + ack.dump() + ")");
    return result;
}

bool Client::send_gcode(const std::string& gcode)
{
    bool        ok = true;
    std::string line;
    for (std::size_t i = 0; i <= gcode.size(); ++i) {
        const char c = i < gcode.size() ? gcode[i] : '\n';
        if (c == '\r')
            continue;
        if (c == '\n') {
            const auto b = line.find_first_not_of(" \t");
            if (b != std::string::npos) {
                const auto e = line.find_last_not_of(" \t");
                ok           = send("set", {{"gcodeCmd", line.substr(b, e - b + 1)}}) && ok;
            }
            line.clear();
        } else
            line.push_back(c);
    }
    return ok;
}

}} // namespace Slic3r::CrealityCfs
