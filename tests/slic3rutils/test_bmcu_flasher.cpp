// Tests for the BMCU flasher (src/slic3r/Utils/BmcuFlasher.*): packet bytes against golden vectors computed from
// the BMCU-Flasher protocol (bmcu_flasher.py v1.4), the key derivation, the frame reader, hashes, the firmware
// catalogue parsers (recorded manifest / GitHub releases JSON in tests/data/bmcu) and the whole flash sequence
// against a mock CH32V203 WCH ISP bootloader (USB AutoDI wiring and TTL with the fast baud fallback).
// No real serial port is opened.
#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BmcuFlasher.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r::Bmcu;

namespace {

std::string to_hex(const Bytes &b)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

std::string read_file(const std::string &name)
{
    std::ifstream f(std::string(TEST_DATA_DIR) + "/bmcu/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Bytes make_firmware(size_t n, uint32_t seed = 12345)
{
    Bytes fw(n);
    uint32_t x = seed;
    for (auto &b : fw) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    return fw;
}

// ---- mock CH32V203 board -----------------------------------------------------------------------------------------
struct MockBoard
{
    // USB board wiring (CH340 DTR/RTS -> BOOT0 / RESET); ignored when `ttl`
    bool boot_on_dtr      = false;
    bool boot_active      = true;   // boot line level that selects the bootloader
    bool reset_hold_level = false;  // reset line level that holds the MCU in reset
    bool wired            = true;   // false: the lines do nothing (wrong cable / no board)
    bool ttl              = false;  // external adapter: the "user" enters the bootloader by hand
    unsigned ttl_user_delay_ms = 700;

    uint8_t chip_id = DEVICE_ID, chip_type = DEVICE_TYPE;
    bool fast_broken       = false; // accepts C5 but cannot keep up at the fast baud
    bool protect_until_key = false; // WPR writes before A3 are ignored (exercises the unprotect branch)
    int  corrupt_program_at = -1;   // flips one byte in the block at this address

    enum State { App, Reset, Isp } state = App;
    bool dtr = true, rts = true;
    uint64_t clock = 0;
    unsigned host_baud = DEFAULT_BAUD, mcu_baud = DEFAULT_BAUD;
    uint64_t reenter_at = 0;

    Bytes flash = Bytes(FLASH_BYTES, 0x5A); // "old firmware"
    Bytes cfg12 = {0xA5, 0x5A, 0x3F, 0xC0, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00};
    Bytes uid   = {0xCD, 0xAB, 0x4E, 0x1F, 0x37, 0x02, 0x99, 0x10};
    bool  unlocked = false, erased = false;
    uint8_t key[8] = {};
    uint32_t next_prog_addr = 0;

    std::vector<uint8_t> isp_cmds;     // commands handled by the bootloader, in order
    std::vector<uint8_t> isp_end_reasons;
    std::vector<unsigned> opens;       // baud of every open
    int bad_frames = 0, garbled = 0, ignored = 0, isp_entries = 0;
    Bytes to_host;

    void eval_lines()
    {
        if (ttl || !wired)
            return;
        const bool reset_line = boot_on_dtr ? rts : dtr;
        const bool boot_line  = boot_on_dtr ? dtr : rts;
        if (reset_line == reset_hold_level)
            state = Reset;
        else if (state == Reset) {
            state = boot_line == boot_active ? Isp : App;
            if (state == Isp)
                enter_isp();
        }
    }
    void enter_isp()
    {
        state = Isp;
        mcu_baud = DEFAULT_BAUD;
        unlocked = false;
        ++isp_entries;
    }
    void tick()
    {
        if (ttl && reenter_at && clock >= reenter_at) {
            reenter_at = 0;
            enter_isp();
        }
    }
    void reply(uint8_t cmd, uint8_t code, const Bytes &data)
    {
        Bytes f = {0x55, 0xAA, cmd, code, uint8_t(data.size() & 0xFF), uint8_t(data.size() >> 8)};
        f.insert(f.end(), data.begin(), data.end());
        unsigned s = 0;
        for (size_t i = 2; i < f.size(); ++i) s += f[i];
        f.push_back(uint8_t(s));
        to_host.insert(to_host.end(), f.begin(), f.end());
    }
    // independent of the code under test: the bootloader key is sum(uid) repeated, last byte + chip id
    void make_key()
    {
        unsigned s = 0;
        for (uint8_t b : uid) s += b;
        for (auto &k : key) k = uint8_t(s);
        key[7] = uint8_t(key[7] + chip_id);
    }
    uint8_t key_sum8() const { unsigned s = 0; for (uint8_t k : key) s += k; return uint8_t(s); }

    void host_write(const uint8_t *p, size_t n)
    {
        tick();
        if (host_baud != mcu_baud) { ++garbled; return; }
        Bytes in(p, p + n);
        size_t i = 0;
        while (i + 6 <= in.size()) {
            if (in[i] != 0x57 || in[i + 1] != 0xAB) { ++bad_frames; return; }
            const size_t ln = size_t(in[i + 3]) | (size_t(in[i + 4]) << 8);
            if (i + 5 + ln + 1 > in.size()) { ++bad_frames; return; }
            unsigned s = 0;
            for (size_t k = i + 2; k < i + 5 + ln; ++k) s += in[k];
            if (uint8_t(s) != in[i + 5 + ln]) { ++bad_frames; return; }
            handle(in[i + 2], Bytes(in.begin() + i + 5, in.begin() + i + 5 + ln));
            i += 5 + ln + 1;
        }
        if (i != in.size()) ++bad_frames;
    }

    void handle(uint8_t cmd, const Bytes &d)
    {
        if (state != Isp) { ++ignored; return; }
        isp_cmds.push_back(cmd);
        switch (cmd) {
        case CMD_IDENTIFY: {
            static const char tag[] = "MCU ISP & WCH.CN";
            const bool ok = d.size() == 18 && d[0] == DEVICE_ID && d[1] == DEVICE_TYPE && std::memcmp(d.data() + 2, tag, 16) == 0;
            reply(cmd, ok ? 0x00 : 0xF1, Bytes{chip_id, chip_type});
            break;
        }
        case CMD_READ_CFG: {
            Bytes r = {d.empty() ? uint8_t(0) : d[0], 0x00};
            r.insert(r.end(), cfg12.begin(), cfg12.end());
            r.insert(r.end(), {0x00, 0x02, 0x09, 0x00}); // bootloader version
            r.insert(r.end(), uid.begin(), uid.end());
            reply(cmd, 0x00, r);
            break;
        }
        case CMD_WRITE_CFG: {
            if (d.size() != 14 || d[0] != CFG_MASK_RDPR_USER_DATA_WPR) { reply(cmd, 0xF2, Bytes{0, 0}); break; }
            for (int k = 0; k < 12; ++k) {
                if (k >= 8 && protect_until_key && !unlocked)
                    continue;
                cfg12[k] = d[2 + k];
            }
            reply(cmd, 0x00, Bytes{0, 0});
            break;
        }
        case CMD_ISP_END:
            reply(cmd, 0x00, Bytes{0, 0});
            isp_end_reasons.push_back(d.empty() ? 0xFF : d[0]);
            state = App;
            unlocked = false;
            if (ttl && !d.empty() && d[0] == 1)
                reenter_at = clock + ttl_user_delay_ms; // the user holds BOOT and taps RESET
            break;
        case CMD_ISP_KEY:
            if (d.size() != SEED_LEN) { reply(cmd, 0xF3, Bytes{0}); break; }
            make_key();
            unlocked = true;
            reply(cmd, 0x00, Bytes{key_sum8()});
            break;
        case CMD_ERASE: {
            const uint32_t sectors = d.size() == 4 ? uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24) : 0;
            if (!unlocked || sectors != FLASH_KB) { reply(cmd, 0xF4, Bytes{0, 0}); break; }
            std::fill(flash.begin(), flash.end(), 0xFF);
            erased = true;
            next_prog_addr = 0;
            reply(cmd, 0x00, Bytes{0, 0});
            break;
        }
        case CMD_PROGRAM:
        case CMD_VERIFY: {
            if (!unlocked || d.size() < 5) { reply(cmd, 0xF5, Bytes{0, 0}); break; }
            const uint32_t addr = uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
            const size_t n = d.size() - 5;
            if (addr + n > FLASH_BYTES || d[4] != 0x00) { reply(cmd, 0xF6, Bytes{0, 0}); break; }
            Bytes plain(n);
            for (size_t k = 0; k < n; ++k) plain[k] = d[5 + k] ^ key[k & 7];
            if (cmd == CMD_PROGRAM) {
                if (!erased || addr != next_prog_addr) { reply(cmd, 0xF7, Bytes{0, 0}); break; }
                if (int(addr) == corrupt_program_at && n > 0) plain[3] ^= 0x01;
                std::copy(plain.begin(), plain.end(), flash.begin() + addr);
                next_prog_addr = addr + uint32_t(n);
                reply(cmd, 0x00, Bytes{0, 0});
            } else {
                const bool same = std::equal(plain.begin(), plain.end(), flash.begin() + addr);
                reply(cmd, same ? 0x00 : 0xF8, Bytes{0, 0});
            }
            break;
        }
        case CMD_SET_BAUD: {
            const uint32_t b = uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
            reply(cmd, 0x00, Bytes{0, 0});
            mcu_baud = fast_broken ? b + 1 : b;
            break;
        }
        default: reply(cmd, 0xFF, Bytes{0, 0});
        }
    }
};

class MockTransport : public Transport
{
public:
    MockTransport(MockBoard &b, unsigned baud) : m_b(b) { m_b.host_baud = baud; }
    void write(const uint8_t *data, size_t n) override
    {
        if (m_closed) throw std::runtime_error("port closed");
        m_b.host_write(data, n);
    }
    size_t read_available(uint8_t *buf, size_t cap) override
    {
        m_b.tick();
        const size_t n = std::min(cap, m_b.to_host.size());
        std::copy(m_b.to_host.begin(), m_b.to_host.begin() + n, buf);
        m_b.to_host.erase(m_b.to_host.begin(), m_b.to_host.begin() + n);
        return n;
    }
    void purge() override { m_b.to_host.clear(); }
    void set_baud(unsigned baud) override { m_b.host_baud = baud; }
    void set_dtr(bool on) override { m_b.dtr = on; m_b.eval_lines(); }
    void set_rts(bool on) override { m_b.rts = on; m_b.eval_lines(); }
    void close() override
    {
        // the lines are released with the port: the board keeps its state
        m_b.dtr = m_b.rts = true;
        m_closed = true;
    }
    void sleep_ms(unsigned ms) override { m_b.clock += ms; }
    uint64_t now_ms() override { return m_b.clock; }
private:
    MockBoard &m_b;
    bool m_closed = false;
};

TransportFactory factory_for(MockBoard &b)
{
    return [&b](unsigned baud) -> std::unique_ptr<Transport> {
        b.opens.push_back(baud);
        if (b.ttl) b.reenter_at = b.clock + b.ttl_user_delay_ms; // the user enters the bootloader by hand
        return std::make_unique<MockTransport>(b, baud);
    };
}

struct Recorder
{
    std::vector<std::string> lines;
    std::vector<int>         progress;
    std::vector<Stage>       stages;
    FlashCallbacks cb()
    {
        FlashCallbacks c;
        c.log      = [this](LogLevel, const std::string &m) { lines.push_back(m); };
        c.progress = [this](int p) { progress.push_back(p); };
        c.stage    = [this](Stage s) { stages.push_back(s); };
        return c;
    }
    bool has(const std::string &needle) const
    {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) { return l.find(needle) != std::string::npos; });
    }
};

// Run-length string of the bootloader command log, e.g. "a1 a7 a8 ... a5x960 ...".
std::string rle(const std::vector<uint8_t> &cmds)
{
    std::string out;
    for (size_t i = 0; i < cmds.size();) {
        size_t j = i;
        while (j < cmds.size() && cmds[j] == cmds[i]) ++j;
        char buf[16];
        if (j - i > 2) std::snprintf(buf, sizeof(buf), "%02x*%zu", cmds[i], j - i);
        else           std::snprintf(buf, sizeof(buf), "%02x", cmds[i]);
        if (!out.empty()) out += ' ';
        out += buf;
        if (j - i == 2) { out += ' '; out += buf; }
        i = j;
    }
    return out;
}

bool flash_holds(const MockBoard &b, const Bytes &fw)
{
    if (!std::equal(fw.begin(), fw.end(), b.flash.begin())) return false;
    const size_t padded = (fw.size() + CHUNK - 1) / CHUNK * CHUNK;
    return std::all_of(b.flash.begin() + fw.size(), b.flash.end(), [](uint8_t c) { return c == 0xFF; }) && padded <= FLASH_BYTES;
}

} // namespace

TEST_CASE("BMCU packets match the BMCU-Flasher byte layout", "[bmcu]")
{
    CHECK(to_hex(build_identify()) == "57aba1120031194d4355204953502026205743482e434ef5");
    CHECK(to_hex(build_read_cfg(0x1F)) == "57aba702001f00c8");
    CHECK(to_hex(build_erase(64)) == "57aba4040040000000e8");
    CHECK(to_hex(build_set_baud(1000000)) == "57abc5040040420f005a");
    CHECK(to_hex(build_isp_end(1)) == "57aba2010001a4");
    CHECK(to_hex(build_program(0x38, 0, Bytes{1, 2, 3})) == "57aba508003800000000010203eb");
    CHECK(to_hex(build_isp_key(Bytes(30, 0))) == "57aba31e00" + std::string(60, '0') + "c1");
}

TEST_CASE("BMCU key derivation", "[bmcu]")
{
    Bytes seed(30);
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = uint8_t(i);
    const Key8 ks = key_from_seed(seed, 0x5A, 0x31);
    CHECK(to_hex(Bytes(ks.begin(), ks.end())) == "4a5c5e4256484e7b");
    const uint8_t uid[8] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    const Key8 ku = key_from_uid(uid, 0x31);
    CHECK(to_hex(Bytes(ku.begin(), ku.end())) == "3838383838383869");
    const Bytes plain = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const Bytes enc = xor_crypt(plain.data(), plain.size(), ku);
    CHECK(xor_crypt(enc.data(), enc.size(), ku) == plain);
    CHECK(enc[8] == (9 ^ 0x38)); // the key wraps every 8 bytes
    CHECK_THROWS(key_from_seed(Bytes(7, 0), 0, 0));
}

TEST_CASE("BMCU frame reader", "[bmcu]")
{
    FrameReader r;
    Frame f;
    // garbage, a frame with a bad checksum, a frame of another command, then the wanted one split in two pushes
    const Bytes junk = {0x00, 0x13, 0x55};
    const Bytes bad  = {0x55, 0xAA, 0xA1, 0x00, 0x02, 0x00, 0x31, 0x19, 0x00};
    const Bytes other = {0x55, 0xAA, 0xA7, 0x00, 0x00, 0x00, 0xA7};
    const Bytes good = {0x55, 0xAA, 0xA1, 0x00, 0x02, 0x00, 0x31, 0x19, uint8_t(0xA1 + 0x02 + 0x31 + 0x19)};
    r.push(junk.data(), junk.size());
    r.push(bad.data(), bad.size());
    r.push(other.data(), other.size());
    r.push(good.data(), 4);
    CHECK_FALSE(r.next(CMD_IDENTIFY, f));
    r.push(good.data() + 4, good.size() - 4);
    REQUIRE(r.next(CMD_IDENTIFY, f));
    CHECK(f.code == 0x00);
    CHECK(f.data == Bytes{0x31, 0x19});
    CHECK_FALSE(r.next(CMD_IDENTIFY, f));
}

TEST_CASE("BMCU hashes", "[bmcu]")
{
    const std::string abc = "abc";
    CHECK(sha256_hex((const uint8_t *) abc.data(), abc.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string digits = "123456789";
    CHECK(crc32_hex(crc32((const uint8_t *) digits.data(), digits.size())) == "CBF43926");
    Bytes d(1000);
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i * 7 + 3);
    CHECK(sha256_hex(d.data(), d.size()) == "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371");
    CHECK(crc32_hex(crc32(d.data(), d.size())) == "17BC2A46");
    // incremental CRC, as the download loop of BMCU-Flasher does it
    CHECK(crc32(d.data() + 500, 500, crc32(d.data(), 500)) == crc32(d.data(), d.size()));
}

TEST_CASE("BMCU versions", "[bmcu]")
{
    CHECK(normalize_version(" 11.00.00.00\r\n") == "11.00.00.00");
    CHECK(normalize_version("11.00.00.00\nextra") == "11.00.00.00");
    CHECK(normalize_version("V11") == "");
    CHECK(normalize_version("1.2.3") == "");
    CHECK(normalize_version("1.2.3.4.5") == "");
    CHECK(is_version_x4("10.50.00.00"));
    CHECK(compare_versions("11.00.00.00", "10.50.00.00") == 1);
    CHECK(compare_versions("10.05.00.00", "10.50.00.00") == -1);
    CHECK(compare_versions("11.00.00.00", "11.00.00.00") == 0);
    CHECK(display_version("11.00.00.00") == "V11.0");
    CHECK(display_version("10.50.00.00") == "V10.5");
    CHECK(display_version("10.55.00.00") == "V10.55");
}

TEST_CASE("BMCU manifest and variants from the recorded manifest", "[bmcu]")
{
    const Manifest m = parse_manifest(read_file("manifest_sample.txt"));
    CHECK(m.size() == 37); // 36 variants + the "../escape" row, which parses like in bmcu_flasher.py
    CHECK(m.count("../escape/ams_auto_0.10f.bin") == 1);
    CHECK(m.count("bad/sha.bin") == 0);
    CHECK(m.count("bad/crc.bin") == 0);
    const auto it = m.find("standard(A1)/AUTOLOAD/FILAMENT_RGB_ON/AMS_AUTO/ams_auto_0.095f.bin");
    REQUIRE(it != m.end());
    CHECK(it->second.sha256 == "79c19e1cd6fd2e6d6019bdf1582d86d1b87156b18c04df5329cb67ffca80eab0");
    CHECK(it->second.crc32 == "A2F36575");
    CHECK(it->second.size == 55964);

    const auto vars = variants_from_manifest(m);
    CHECK(vars.size() == 36); // the "../escape" row is never offered as a variant
    for (const auto &v : vars) {
        CHECK(is_safe_rel_path(v.rel_path));
        CHECK(build_rel_path(v.force, v.autoload, v.rgb, v.slot, v.retract) == v.rel_path);
    }
    CHECK(build_rel_path("standard(A1)", true, true, "AMS_AUTO", "0.095f") == "standard(A1)/AUTOLOAD/FILAMENT_RGB_ON/AMS_AUTO/ams_auto_0.095f.bin");
    CHECK(build_rel_path("high_force_load(P1S)", false, false, "AMS_C", "0.60f") ==
          "high_force_load(P1S)/NO_AUTOLOAD/FILAMENT_RGB_OFF/AMS_C/ams_c_0.60f.bin");
    CHECK(retract_display("0.095f") == "9.5 cm");
    CHECK(retract_display("0.60f") == "60 cm");
    CHECK_FALSE(is_safe_rel_path("../escape/ams_auto_0.10f.bin"));
    CHECK_FALSE(is_safe_rel_path("/abs.bin"));
    CHECK_FALSE(is_safe_rel_path("C:/x.bin"));
    CHECK_THROWS(parse_manifest("# only a comment\n"));

    CHECK(firmware_url("main", "standard(A1)/AUTOLOAD/FILAMENT_RGB_ON/AMS_AUTO/ams_auto_0.095f.bin") ==
          "https://raw.githubusercontent.com/jarczakpawel/BMCU-C-PJARCZAK/main/firmwares/standard%28A1%29/AUTOLOAD/FILAMENT_RGB_ON/AMS_AUTO/ams_auto_0.095f.bin");
    CHECK(manifest_url("v11") == "https://raw.githubusercontent.com/jarczakpawel/BMCU-C-PJARCZAK/v11/firmwares/manifest.txt");
}

TEST_CASE("BMCU firmware verification against the manifest", "[bmcu]")
{
    const Bytes fw = make_firmware(4000);
    ManifestEntry e{sha256_hex(fw.data(), fw.size()), crc32_hex(crc32(fw.data(), fw.size())), fw.size()};
    std::string err;
    CHECK(verify_firmware(fw, e, err));
    Bytes bad = fw;
    bad[100] ^= 1;
    CHECK_FALSE(verify_firmware(bad, e, err));
    CHECK(err.find("verify failed") != std::string::npos);
    Bytes shorter(fw.begin(), fw.end() - 1);
    CHECK_FALSE(verify_firmware(shorter, e, err));
}

TEST_CASE("BMCU releases from the recorded GitHub JSON", "[bmcu]")
{
    const auto rel = parse_releases(read_file("releases_sample.json"));
    REQUIRE(rel.size() >= 3);
    CHECK(rel[0].tag == "v12-beta"); // the draft is dropped
    CHECK(rel[0].prerelease);
    CHECK(rel[1].tag == "v11");
    CHECK_FALSE(rel[1].prerelease);
    CHECK(rel[1].published_at.rfind("2026-10-04", 0) == 0);
    CHECK(std::none_of(rel.begin(), rel.end(), [](const Release &r) { return r.tag == "../evil" || r.tag == "draft-x"; }));
    CHECK(parse_releases("{not json").empty());
    CHECK(parse_releases("{\"message\":\"API rate limit exceeded\"}").empty());
    CHECK_FALSE(is_safe_ref("../x"));
    CHECK_FALSE(is_safe_ref("a/b"));
    CHECK(is_safe_ref("V10.5"));
}

TEST_CASE("BMCU USB flash against the mock bootloader", "[bmcu]")
{
    MockBoard b; // boot on RTS (high), reset on DTR (low holds): AutoDI finds it on the 5th combination
    const Bytes fw = make_firmware(53656);
    const size_t blocks = (fw.size() + CHUNK - 1) / CHUNK;
    Recorder rec;
    flash_firmware(fw, factory_for(b), FlashOptions{}, rec.cb());

    CHECK(flash_holds(b, fw));
    CHECK(b.bad_frames == 0);
    CHECK(b.garbled == 0);
    CHECK(b.state == MockBoard::App);         // rebooted into the new firmware
    CHECK(b.isp_end_reasons == std::vector<uint8_t>{1, 0});
    CHECK(b.opens == std::vector<unsigned>{DEFAULT_BAUD});
    CHECK(b.host_baud == DEFAULT_FAST_BAUD);
    // WCHTool sequence of bmcu_flasher.py: identify (AutoDI + again), cfg dance, isp_end(1), re-enter, key, erase,
    // tail check, fast baud, program + empty flush, key, verify all but the last block, isp_end(0)
    const std::string expected = "a1 a1 a7 a8 a7 a2 a1 a1 a7 a1 a7 a8 a7 a3 a4 a6 c5 a5*" + std::to_string(blocks + 1) + " a3 a6*" +
                                 std::to_string(blocks - 1) + " a2";
    CHECK(rle(b.isp_cmds) == expected);
    CHECK(rec.has("autodi ok boot_is_dtr=0 boot_assert=1 reset_assert=1"));
    CHECK(rec.has("isp_key ok (src=uid"));
    CHECK(rec.has("verify ok"));
    REQUIRE_FALSE(rec.progress.empty());
    CHECK(std::is_sorted(rec.progress.begin(), rec.progress.end()));
    CHECK(rec.progress.back() == 100);
    CHECK(rec.stages.back() == Stage::Done);
}

TEST_CASE("BMCU USB flash with the other wiring and a protected flash", "[bmcu]")
{
    MockBoard b;
    b.boot_on_dtr = true;          // boot on DTR, reset on RTS (high holds)
    b.boot_active = false;
    b.reset_hold_level = true;
    b.protect_until_key = true;    // WPR stays set until unlocked: the unprotect branch runs
    const Bytes fw = make_firmware(56 * 10); // exact multiple of the chunk
    Recorder rec;
    flash_firmware(fw, factory_for(b), FlashOptions{}, rec.cb());
    CHECK(flash_holds(b, fw));
    CHECK(rec.has("code flash protected"));
    CHECK(rec.has("unprotect ok"));
    CHECK(rle(b.isp_cmds).find("a3 a8 a7 a4") != std::string::npos);
    CHECK(b.state == MockBoard::App);
}

TEST_CASE("BMCU TTL flash falls back to the normal baud", "[bmcu]")
{
    MockBoard b;
    b.ttl = true;
    b.fast_broken = true;
    const Bytes fw = make_firmware(3000, 7);
    Recorder rec;
    FlashOptions o;
    o.mode = Mode::Ttl;
    flash_firmware(fw, factory_for(b), o, rec.cb());
    CHECK(flash_holds(b, fw));
    CHECK(b.opens == std::vector<unsigned>{DEFAULT_BAUD, DEFAULT_BAUD});
    CHECK(rec.has("Retrying the full flash at 115200 baud"));
    CHECK(b.host_baud == DEFAULT_BAUD);
    // second run: no C5
    const auto last_a4 = std::find(b.isp_cmds.rbegin(), b.isp_cmds.rend(), CMD_ERASE);
    CHECK(std::find(b.isp_cmds.rbegin(), last_a4, CMD_SET_BAUD) == last_a4);
}

TEST_CASE("BMCU cancel before erase keeps the old firmware", "[bmcu]")
{
    MockBoard b;
    Recorder rec;
    FlashCallbacks cb = rec.cb();
    cb.cancelled = [] { return true; };
    CHECK_THROWS_AS(flash_firmware(make_firmware(1000), factory_for(b), FlashOptions{}, cb), FlashCancelled);
    CHECK(std::all_of(b.flash.begin(), b.flash.end(), [](uint8_t c) { return c == 0x5A; }));
    CHECK(std::find(b.isp_cmds.begin(), b.isp_cmds.end(), CMD_ERASE) == b.isp_cmds.end());
    CHECK(b.state == MockBoard::App);
}

TEST_CASE("BMCU errors", "[bmcu]")
{
    SECTION("no board answers") {
        MockBoard b;
        b.wired = false;
        CHECK_THROWS_AS(flash_firmware(make_firmware(1000), factory_for(b), FlashOptions{}, FlashCallbacks{}), FlashError);
        CHECK(b.clock < 20000); // gives up in bounded (virtual) time
    }
    SECTION("another chip") {
        MockBoard b;
        b.chip_id = 0x30;
        CHECK_THROWS_AS(flash_firmware(make_firmware(1000), factory_for(b), FlashOptions{}, FlashCallbacks{}), FlashError);
        CHECK(std::find(b.isp_cmds.begin(), b.isp_cmds.end(), CMD_ERASE) == b.isp_cmds.end());
    }
    SECTION("verify catches a bad block") {
        MockBoard b;
        b.corrupt_program_at = 56 * 3;
        try {
            flash_firmware(make_firmware(2000), factory_for(b), FlashOptions{}, FlashCallbacks{});
            FAIL("no error");
        } catch (const FlashError &e) {
            CHECK(std::string(e.what()).find("verify failed at 0x000000a8") != std::string::npos);
        }
    }
    SECTION("firmware too large or empty") {
        MockBoard b;
        CHECK_THROWS_AS(flash_firmware(Bytes(FLASH_BYTES + 1, 0), factory_for(b), FlashOptions{}, FlashCallbacks{}), FlashError);
        CHECK_THROWS_AS(flash_firmware(Bytes{}, factory_for(b), FlashOptions{}, FlashCallbacks{}), FlashError);
        CHECK(b.opens.empty());
    }
    SECTION("TTL user never enters the bootloader") {
        MockBoard b;
        b.ttl = true;
        b.ttl_user_delay_ms = 1000000;
        FlashOptions o;
        o.mode = Mode::Ttl;
        CHECK_THROWS_AS(flash_firmware(make_firmware(1000), factory_for(b), o, FlashCallbacks{}), FlashError);
    }
}
