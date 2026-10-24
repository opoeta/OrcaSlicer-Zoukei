#include <catch2/catch_all.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/DeviceActivityLog.hpp"

using namespace Slic3r;
namespace DA = Slic3r::DeviceActivity;
using json   = nlohmann::json;

namespace {
std::size_t count_source(const std::string& host, DA::Source s)
{
    std::size_t n = 0;
    for (const auto& e : DA::entries(host))
        n += e.source == s;
    return n;
}
std::string all_text(const std::string& host)
{
    std::string out;
    for (const auto& e : DA::entries(host))
        out += e.text + "\n" + e.raw + "\n";
    return out;
}
} // namespace

TEST_CASE("DeviceActivity: host key drops scheme, port and credentials", "[DeviceActivity]")
{
    CHECK(DA::key_of("http://10.1.2.3:7125/x") == "10.1.2.3");
    CHECK(DA::key_of("HTTPS://user:pass@Printer.local:443/") == "printer.local");
    CHECK(DA::key_of("10.1.2.3") == "10.1.2.3");
}

TEST_CASE("DeviceActivity: secrets are never recorded", "[DeviceActivity]")
{
    const std::string host = "t-redact";
    DA::clear(host);
    DA::add(host, DA::Source::Agent, "login token=abc123SECRET ok", json{{"token", "abc123SECRET"}, {"nested", {{"Password", "hunter2"}, {"cookie", "sid=1"}, {"keep", "visible"}}}});
    DA::add(host, DA::Source::Agent, "Authorization: Bearer abcdefghijklmnop");
    DA::ws_message(host, true, R"({"method":"set","params":{"remote_api_token":"TOPSECRET","gcodeCmd":"G28"}})");
    const std::string text = all_text(host);
    CHECK(text.find("abc123SECRET") == std::string::npos);
    CHECK(text.find("hunter2") == std::string::npos);
    CHECK(text.find("TOPSECRET") == std::string::npos);
    CHECK(text.find("abcdefghijklmnop") == std::string::npos);
    CHECK(text.find("sid=1") == std::string::npos);
    CHECK(text.find("visible") != std::string::npos);
    CHECK(text.find("G28") != std::string::npos);
}

TEST_CASE("DeviceActivity: websocket traffic keeps changes only", "[DeviceActivity]")
{
    const std::string host = "t-ws";
    DA::clear(host);
    DA::set_show_all(false);
    // heartbeat and the periodic poll are skipped
    DA::ws_message(host, false, "heart_beat");
    DA::ws_message(host, true, "ok");
    DA::ws_message(host, true, R"({"method":"get","params":{"boxsInfo":1}})");
    CHECK(DA::entries(host).empty());
    // the first frame, then only changes
    const std::string frame1 = R"({"deviceState":1,"nozzleTemp":"139.8","printProgress":42,"boxsInfo":{"materialBoxs":[{"id":1,"materials":[{"id":0,"vendor":"Creality","type":"PLA","name":"Hyper PLA","color":"#0ff0000","state":1}]}]}})";
    DA::ws_message(host, false, frame1);
    const std::size_t n1 = DA::entries(host).size();
    CHECK(n1 >= 1);
    DA::ws_message(host, false, frame1); // identical: nothing new
    CHECK(DA::entries(host).size() == n1);
    // telemetry that moves every second is not logged
    DA::ws_message(host, false, R"({"nozzleTemp":"140.1"})");
    CHECK(DA::entries(host).size() == n1);
    // a slot change is logged as a diff
    DA::ws_message(host, false, R"({"boxsInfo":{"materialBoxs":[{"id":1,"materials":[{"id":0,"vendor":"Creality","type":"PETG","name":"Hyper PETG","color":"#000ff00","state":1}]}]}})");
    const std::string text = all_text(host);
    CHECK(text.find("box1/slot0") != std::string::npos);
    CHECK(text.find("->") != std::string::npos);
    // outgoing writes are logged
    DA::ws_message(host, true, R"({"method":"set","params":{"modifyMaterial":{"boxId":1,"id":0}}})");
    CHECK(count_source(host, DA::Source::WsOut) == 1);
    // "show all" keeps the noise too
    DA::set_show_all(true);
    DA::ws_message(host, false, R"({"nozzleTemp":"140.2"})");
    DA::ws_message(host, false, "heart_beat");
    DA::set_show_all(false);
    CHECK(all_text(host).find("nozzleTemp=") != std::string::npos);
}

TEST_CASE("DeviceActivity: ring buffer, since / limit and filters", "[DeviceActivity]")
{
    const std::string host = "t-ring";
    DA::clear(host);
    for (int i = 0; i < int(DA::CAPACITY) + 50; ++i)
        DA::add(host, i % 2 ? DA::Source::Sync : DA::Source::Error, "entry " + std::to_string(i));
    const auto all = DA::entries(host);
    CHECK(all.size() == DA::CAPACITY);
    CHECK(all.front().text == "entry 50");
    json q = DA::query(host, 0, 10);
    CHECK(q["entries"].size() == 10);
    CHECK(q["entries"].back()["text"] == "entry " + std::to_string(DA::CAPACITY + 49));
    CHECK(q["dropped"] == 50);
    const std::uint64_t next = q["next"].get<std::uint64_t>();
    CHECK(DA::query(host, next, 10)["entries"].empty());
    DA::add(host, DA::Source::Agent, "later");
    json more = DA::query(host, next, 10);
    REQUIRE(more["entries"].size() == 1);
    CHECK(more["entries"][0]["text"] == "later");
    CHECK(DA::query(host, 0, 2000, DA::source_mask("error"))["entries"].size() == count_source(host, DA::Source::Error));
    CHECK(DA::query(host, 0, 5, ~0u, "ENTRY 2049")["entries"].size() == 1);
    DA::clear(host);
    CHECK(DA::entries(host).empty());
}

TEST_CASE("DeviceActivity: klippy.log line filter", "[DeviceActivity]")
{
    CHECK(DA::klipper_line_relevant("Timer too close"));
    CHECK(DA::klipper_line_relevant("mcu 'mcu': Unable to connect"));
    CHECK(DA::klipper_line_relevant("Transition to shutdown state: Lost communication with MCU"));
    CHECK(DA::klipper_line_relevant("DXC2 key code 0x20"));
    CHECK_FALSE(DA::klipper_line_relevant("Stats 1234.5: gcodein=0 mcu: mcu_awake=0.002"));
}
