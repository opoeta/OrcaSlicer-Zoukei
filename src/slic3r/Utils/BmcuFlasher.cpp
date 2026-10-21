// BMCU firmware flasher. See BmcuFlasher.hpp.
// Flash sequence ported from BMCU-Flasher (https://github.com/jarczakpawel/BMCU-Flasher, bmcu_flasher.py v1.4),
// MIT License, Copyright (c) 2026 Pawel Jarczak.
#include "BmcuFlasher.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace Bmcu {

// ---- adapters ----------------------------------------------------------------------------------------------------
const std::vector<Adapter> &known_adapters()
{
    // bmcu_flasher_gui.py ADAPTERS (name, vid, pid, fast baud)
    static const std::vector<Adapter> list = {
        {"CH340/CH341 (WCH)", 0x1A86, 0x7523, 1000000},
        {"CP2102/CP210x (Silabs)", 0x10C4, 0xEA60, 921600},
        {"FT232R (FTDI)", 0x0403, 0x6001, 1000000},
        {"PL2303 (Prolific)", 0x067B, 0x2303, 460800},
        {"CH9102 (WCH)", 0x1A86, 0x55D4, 1000000},
        {"CH343 (WCH)", 0x1A86, 0x55D3, 1000000},
    };
    return list;
}

// ---- packets -----------------------------------------------------------------------------------------------------
static void put_u16(Bytes &b, uint32_t v) { b.push_back(uint8_t(v & 0xFF)); b.push_back(uint8_t((v >> 8) & 0xFF)); }
static void put_u32(Bytes &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

uint8_t checksum(const uint8_t *data, size_t n)
{
    unsigned s = 0;
    for (size_t i = 0; i < n; ++i)
        s += data[i];
    return uint8_t(s & 0xFF);
}

Bytes pack_req(const Bytes &payload)
{
    Bytes out = {0x57, 0xAB};
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(checksum(payload.data(), payload.size()));
    return out;
}

static Bytes cmd_payload(uint8_t cmd, const Bytes &data)
{
    Bytes p = {cmd};
    put_u16(p, uint32_t(data.size()));
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

Bytes build_identify(uint8_t device_id, uint8_t device_type)
{
    static const char tag[] = "MCU ISP & WCH.CN";
    Bytes d = {device_id, device_type};
    d.insert(d.end(), tag, tag + sizeof(tag) - 1);
    return pack_req(cmd_payload(CMD_IDENTIFY, d));
}

Bytes build_read_cfg(uint8_t mask) { return pack_req(cmd_payload(CMD_READ_CFG, Bytes{mask, 0x00})); }

Bytes build_write_cfg(uint8_t mask, const Bytes &cfg12)
{
    Bytes d = {mask, 0x00};
    d.insert(d.end(), cfg12.begin(), cfg12.end());
    return pack_req(cmd_payload(CMD_WRITE_CFG, d));
}

Bytes build_isp_key(const Bytes &seed) { return pack_req(cmd_payload(CMD_ISP_KEY, seed)); }

Bytes build_erase(uint32_t sectors)
{
    Bytes d;
    put_u32(d, sectors);
    return pack_req(cmd_payload(CMD_ERASE, d));
}

Bytes build_set_baud(uint32_t baud)
{
    Bytes d;
    put_u32(d, baud);
    return pack_req(cmd_payload(CMD_SET_BAUD, d));
}

static Bytes addr_packet(uint8_t cmd, uint32_t address, uint8_t padding, const Bytes &data)
{
    Bytes d;
    put_u32(d, address);
    d.push_back(padding);
    d.insert(d.end(), data.begin(), data.end());
    return pack_req(cmd_payload(cmd, d));
}

Bytes build_program(uint32_t address, uint8_t padding, const Bytes &data) { return addr_packet(CMD_PROGRAM, address, padding, data); }
Bytes build_verify(uint32_t address, uint8_t padding, const Bytes &data) { return addr_packet(CMD_VERIFY, address, padding, data); }
Bytes build_isp_end(uint8_t reason) { return pack_req(cmd_payload(CMD_ISP_END, Bytes{reason})); }

Key8 key_from_seed(const Bytes &seed, uint8_t uid_chk, uint8_t chip_id)
{
    if (seed.size() < 8)
        throw std::runtime_error("seed too short");
    const size_t a = seed.size() / 5;
    const size_t b = seed.size() / 7;
    Key8 k;
    k[0] = seed[b * 4] ^ uid_chk;
    k[1] = seed[a] ^ uid_chk;
    k[2] = seed[b] ^ uid_chk;
    k[3] = seed[b * 6] ^ uid_chk;
    k[4] = seed[b * 3] ^ uid_chk;
    k[5] = seed[a * 3] ^ uid_chk;
    k[6] = seed[b * 5] ^ uid_chk;
    k[7] = uint8_t(k[0] + chip_id);
    return k;
}

Key8 key_from_uid(const uint8_t *uid8, uint8_t chip_id)
{
    const uint8_t s = checksum(uid8, 8);
    Key8 k;
    k.fill(s);
    k[7] = uint8_t(k[7] + chip_id);
    return k;
}

Bytes xor_crypt(const uint8_t *data, size_t n, const Key8 &key)
{
    Bytes out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = data[i] ^ key[i & 7];
    return out;
}

uint8_t key_sum(const Key8 &key) { return checksum(key.data(), key.size()); }

bool FrameReader::next(uint8_t expect_cmd, Frame &out)
{
    for (;;) {
        size_t i = 0;
        while (i + 1 < m_rx.size() && !(m_rx[i] == 0x55 && m_rx[i + 1] == 0xAA))
            ++i;
        if (i + 1 >= m_rx.size()) {
            // keep a trailing 0x55, it may be the start of the next magic
            if (!m_rx.empty() && m_rx.back() == 0x55)
                m_rx.erase(m_rx.begin(), m_rx.end() - 1);
            else
                m_rx.clear();
            return false;
        }
        if (i > 0)
            m_rx.erase(m_rx.begin(), m_rx.begin() + i);
        if (m_rx.size() < 2 + 4 + 1)
            return false;
        const uint8_t cmd   = m_rx[2];
        const size_t  ln    = size_t(m_rx[4]) | (size_t(m_rx[5]) << 8);
        const size_t  total = 2 + 4 + ln + 1;
        if (m_rx.size() < total)
            return false;
        Bytes frame(m_rx.begin(), m_rx.begin() + total);
        m_rx.erase(m_rx.begin(), m_rx.begin() + total);
        if (checksum(frame.data() + 2, total - 3) != frame.back())
            continue;
        if (cmd != expect_cmd)
            continue;
        out.cmd  = cmd;
        out.code = frame[3];
        out.data.assign(frame.begin() + 6, frame.end() - 1);
        return true;
    }
}

// ---- transport defaults ------------------------------------------------------------------------------------------
void Transport::sleep_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint64_t Transport::now_ms()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- flash sequence ----------------------------------------------------------------------------------------------
namespace {

class TimeoutError : public FlashError { public: using FlashError::FlashError; };
class FastBaudFallback : public FlashError { public: using FlashError::FlashError; };

std::string hex(const uint8_t *d, size_t n, char sep = 0)
{
    static const char *digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (sep && i)
            s += sep;
        s += digits[d[i] >> 4];
        s += digits[d[i] & 15];
    }
    return s;
}
std::string hex(const Bytes &b, char sep = 0) { return hex(b.data(), b.size(), sep); }
std::string hex32(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", unsigned(v));
    return buf;
}

class Isp
{
public:
    Isp(Transport &t, unsigned baud) : baud(baud), m_t(t) {}

    void flush()
    {
        m_t.purge();
        m_reader.clear();
    }
    void set_baud(unsigned b)
    {
        baud = b;
        m_t.set_baud(b);
    }

    Frame recv(uint8_t expect_cmd, unsigned timeout_ms)
    {
        const uint64_t end = m_t.now_ms() + timeout_ms;
        uint8_t buf[512];
        for (;;) {
            if (m_t.now_ms() >= end) {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "timeout waiting for cmd=0x%02x", unsigned(expect_cmd));
                throw TimeoutError(msg);
            }
            for (;;) {
                const size_t n = m_t.read_available(buf, sizeof(buf));
                if (n == 0)
                    break;
                m_reader.push(buf, n);
            }
            Frame f;
            if (m_reader.next(expect_cmd, f))
                return f;
            m_t.sleep_ms(2);
        }
    }

    Frame txrx(const Bytes &pkt, uint8_t expect_cmd, unsigned timeout_ms)
    {
        m_t.write(pkt.data(), pkt.size());
        return recv(expect_cmd, timeout_ms);
    }

    // BMCU board: BOOT0 and RESET of the CH32V203 hang on DTR / RTS of the CH340 (which one is which differs).
    void set_lines(bool boot_is_dtr, bool boot_val, bool reset_val)
    {
        if (boot_is_dtr) {
            m_t.set_dtr(boot_val);
            m_t.set_rts(reset_val);
        } else {
            m_t.set_rts(boot_val);
            m_t.set_dtr(reset_val);
        }
    }
    void pulse_reset(bool boot_is_dtr, bool reset_assert)
    {
        if (boot_is_dtr) {
            m_t.set_rts(!reset_assert);
            m_t.sleep_ms(20);
            m_t.set_rts(reset_assert);
        } else {
            m_t.set_dtr(!reset_assert);
            m_t.sleep_ms(20);
            m_t.set_dtr(reset_assert);
        }
    }

    Transport &t() { return m_t; }

    unsigned baud;

private:
    Transport  &m_t;
    FrameReader m_reader;
};

struct AutoDi { bool boot_is_dtr = false, boot_assert = false, reset_assert = false; };

bool autodi_try(Isp &isp, const Bytes &identify, AutoDi &out)
{
    for (bool boot_is_dtr : {true, false})
        for (bool boot_assert : {true, false})
            for (bool reset_assert : {true, false}) {
                try {
                    isp.flush();
                    isp.set_lines(boot_is_dtr, boot_assert, reset_assert);
                    isp.t().sleep_ms(20);
                    isp.pulse_reset(boot_is_dtr, reset_assert);
                    isp.t().sleep_ms(60);
                    isp.flush();
                    Frame f = isp.txrx(identify, CMD_IDENTIFY, 600);
                    if (f.code == 0x00 && f.data.size() >= 2) {
                        out = {boot_is_dtr, boot_assert, reset_assert};
                        return true;
                    }
                } catch (const std::exception &) {
                }
            }
    return false;
}

struct Ctx
{
    const FlashOptions   &opt;
    const FlashCallbacks &cb;
    void log(LogLevel l, const std::string &m) const { if (cb.log) cb.log(l, m); }
    void progress(int p) const { if (cb.progress) cb.progress(p); }
    void stage(Stage s) const { if (cb.stage) cb.stage(s); }
    bool cancelled() const { return cb.cancelled && cb.cancelled(); }
};

void leave_bootloader(Isp &isp, const FlashOptions &opt, const AutoDi *autodi)
{
    try {
        isp.txrx(build_isp_end(0), CMD_ISP_END, 1200);
    } catch (const std::exception &) {
    }
    if (opt.mode == Mode::Usb && autodi) {
        isp.set_lines(autodi->boot_is_dtr, !autodi->boot_assert, autodi->reset_assert);
        isp.t().sleep_ms(20);
        isp.pulse_reset(autodi->boot_is_dtr, autodi->reset_assert);
    }
}

// Waits (polling identify) until the bootloader answers; `before` runs before each try (AutoDI line toggling).
bool wait_identify(Isp &isp, const Bytes &identify, unsigned total_ms, unsigned try_ms, unsigned gap_ms,
                   const std::function<void()> &before, std::string &last_err, Frame *out = nullptr)
{
    const uint64_t end = isp.t().now_ms() + total_ms;
    for (;;) {
        try {
            if (before)
                before();
            isp.flush();
            Frame f = isp.txrx(identify, CMD_IDENTIFY, try_ms);
            if (f.code == 0x00 && f.data.size() >= 2) {
                if (out)
                    *out = std::move(f);
                return true;
            }
            last_err = "identify bad response";
        } catch (const std::exception &e) {
            last_err = e.what();
        }
        if (isp.t().now_ms() >= end)
            return false;
        isp.t().sleep_ms(gap_ms);
    }
}

void flash_once(const Bytes &fw, const TransportFactory &open, const FlashOptions &opt, const Ctx &ctx)
{
    const bool ttl = opt.mode == Mode::Ttl;
    const size_t blocks = (fw.size() + CHUNK - 1) / CHUNK;
    Bytes fw_pad = fw;
    fw_pad.resize(blocks * CHUNK, 0xFF);
    const unsigned verify_every = std::max(1u, opt.verify_every);

    std::unique_ptr<Transport> port = open(opt.baud);
    if (!port)
        throw FlashError("could not open the serial port");
    Isp isp(*port, opt.baud);
    bool fast_active = false;

    auto fail = [&](const std::string &msg) -> void {
        try { port->close(); } catch (...) {}
        if (ttl && fast_active)
            throw FastBaudFallback(msg);
        throw FlashError(msg);
    };
    auto fast_txrx = [&](const Bytes &pkt, uint8_t cmd, unsigned to) -> Frame {
        try {
            return isp.txrx(pkt, cmd, to);
        } catch (const std::exception &e) {
            if (ttl && fast_active) {
                try { port->close(); } catch (...) {}
                throw FastBaudFallback(e.what());
            }
            throw;
        }
    };
    auto plain_fail = [&](const std::string &msg) -> void {
        try { port->close(); } catch (...) {}
        throw FlashError(msg);
    };

    const uint64_t t0 = port->now_ms();
    const Bytes identify = build_identify(DEVICE_ID, DEVICE_TYPE);
    ctx.stage(Stage::Connect);

    AutoDi autodi;
    Frame  f;
    std::string last_err;
    if (!ttl) {
        ctx.log(LogLevel::Info, "autodi...");
        if (!autodi_try(isp, identify, autodi))
            plain_fail("autodi failed (usb mode): the BMCU did not answer on any DTR/RTS combination");
        ctx.log(LogLevel::Info, std::string("autodi ok boot_is_dtr=") + (autodi.boot_is_dtr ? "1" : "0") +
                                    " boot_assert=" + (autodi.boot_assert ? "1" : "0") +
                                    " reset_assert=" + (autodi.reset_assert ? "1" : "0"));
        ctx.log(LogLevel::Info, "stage identify @ host_baud=" + std::to_string(isp.baud));
        f = isp.txrx(identify, CMD_IDENTIFY, 1000);
    } else {
        ctx.log(LogLevel::Action, "ttl mode: enter bootloader now: disconnect USB, hold BOOT, connect USB, release BOOT when the BMCU board LED lights. waiting for ISP...");
        ctx.log(LogLevel::Info, "stage identify @ host_baud=" + std::to_string(isp.baud));
        if (!wait_identify(isp, identify, opt.ttl_wait_ms, 800, 150, nullptr, last_err, &f))
            plain_fail("identify failed (ttl). enter bootloader first. last=" + last_err);
        ctx.log(LogLevel::Info, "bootloader detected (ISP active)");
    }
    if (f.code != 0x00 || f.data.size() < 2)
        plain_fail("identify failed");
    const uint8_t chip_id = f.data[0], chip_type = f.data[1];
    if (chip_id != DEVICE_ID || chip_type != DEVICE_TYPE) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "unexpected chip_id/type: 0x%02x/0x%02x (expected CH32V203 0x31/0x19)", chip_id, chip_type);
        plain_fail(msg);
    }
    char idmsg[64];
    std::snprintf(idmsg, sizeof(idmsg), "identify ok chip_id=0x%02x chip_type=0x%02x", chip_id, chip_type);
    ctx.log(LogLevel::Info, idmsg);
    if (ctx.cancelled()) {
        leave_bootloader(isp, opt, ttl ? nullptr : &autodi);
        try { port->close(); } catch (...) {}
        throw FlashCancelled();
    }

    auto read_cfg = [&](const char *what) -> Bytes {
        Frame r = isp.txrx(build_read_cfg(CFG_MASK), CMD_READ_CFG, 1200);
        if (r.code != 0x00 || r.data.size() < 14)
            plain_fail(std::string("read_cfg failed") + what);
        return r.data;
    };
    auto log_cfg = [&](const char *tag, const Bytes &cfg) {
        if (cfg.size() >= 8)
            ctx.log(LogLevel::Info, "uid=" + hex(cfg.data() + cfg.size() - 8, 8, '-'));
        ctx.log(LogLevel::Info, std::string(tag) + " rdpr_user=" + hex(cfg.data() + 2, 4) + " cfg_data=" + hex(cfg.data() + 6, 4) +
                                    " cfg_wpr=" + hex(cfg.data() + 10, 4));
    };

    ctx.stage(Stage::Unlock);
    ctx.log(LogLevel::Info, "stage read_cfg (A7)");
    Bytes cfg = read_cfg("");
    log_cfg("cfg", cfg);

    // WCHTool preamble: A8(step1) -> A7 -> A2(01) -> re-enter ISP -> (A1+A7)x2 -> A8(step2) -> A7
    ctx.log(LogLevel::Info, "wchtool: stage write_cfg step1 (A8)");
    Bytes cfg12(cfg.begin() + 2, cfg.begin() + 14);
    {
        Bytes a = cfg12;
        const uint8_t s1[12] = {0xA5, 0x5A, 0x3F, 0xC0, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::copy(s1, s1 + 12, a.begin());
        Frame r = isp.txrx(build_write_cfg(CFG_MASK_RDPR_USER_DATA_WPR, a), CMD_WRITE_CFG, 2000);
        if (r.code != 0x00)
            plain_fail("write_cfg (wchtool step1) failed");
    }
    cfg = read_cfg(" after write_cfg (wchtool step1)");
    log_cfg("cfg_after_step1", cfg);

    ctx.log(LogLevel::Info, "wchtool: stage isp_end reason=0x01 (apply option bytes)");
    try {
        isp.txrx(build_isp_end(1), CMD_ISP_END, 1200);
    } catch (const std::exception &) {
    }

    if (!ttl) {
        ctx.log(LogLevel::Info, "usb: re-enter bootloader (autodi)");
        auto toggle = [&]() {
            isp.flush();
            isp.set_lines(autodi.boot_is_dtr, autodi.boot_assert, autodi.reset_assert);
            port->sleep_ms(20);
            isp.pulse_reset(autodi.boot_is_dtr, autodi.reset_assert);
            port->sleep_ms(80);
        };
        if (!wait_identify(isp, identify, 2500, 800, 100, toggle, last_err))
            plain_fail("identify failed (usb) after isp_end(01). last=" + last_err);
    } else {
        ctx.log(LogLevel::Action, "ttl: re-enter bootloader now (hold BOOT, tap RESET). waiting for ISP...");
        if (!wait_identify(isp, identify, opt.ttl_wait_ms, 800, 150, nullptr, last_err))
            plain_fail("identify failed (ttl) after isp_end(01). last=" + last_err);
    }
    ctx.log(LogLevel::Info, "bootloader detected again (after isp_end(01))");

    for (int i = 0; i < 2; ++i) {
        Frame r = isp.txrx(identify, CMD_IDENTIFY, 1000);
        if (r.code != 0x00 || r.data.size() < 2)
            plain_fail("identify failed after re-enter (wchtool)");
        cfg = read_cfg(" after re-enter (wchtool)");
    }
    log_cfg("cfg_after_reenter", cfg);

    ctx.log(LogLevel::Info, "wchtool: stage write_cfg step2 (A8)");
    cfg12.assign(cfg.begin() + 2, cfg.begin() + 14);
    {
        Bytes b = cfg12;
        const uint8_t s2[12] = {0xFF, 0xFF, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
        std::copy(s2, s2 + 12, b.begin());
        Frame r = isp.txrx(build_write_cfg(CFG_MASK_RDPR_USER_DATA_WPR, b), CMD_WRITE_CFG, 2000);
        if (r.code != 0x00)
            plain_fail("write_cfg (wchtool step2) failed");
    }
    cfg = read_cfg(" after write_cfg (wchtool step2)");
    log_cfg("cfg_after_step2", cfg);
    cfg12.assign(cfg.begin() + 2, cfg.begin() + 14);
    Bytes wpr(cfg12.begin() + 8, cfg12.begin() + 12);

    ctx.log(LogLevel::Info, "stage isp_key (A3)");
    const Bytes seed(SEED_LEN, 0x00);
    Frame kr = isp.txrx(build_isp_key(seed), CMD_ISP_KEY, 1200);
    if (kr.code != 0x00 || kr.data.empty())
        plain_fail("isp_key failed");
    const uint8_t boot_sum = kr.data[0];

    const uint8_t uid_chk = cfg[2];
    struct Cand { const char *name; Key8 key; };
    std::vector<Cand> cands;
    if (cfg.size() >= 8)
        cands.push_back({"uid", key_from_uid(cfg.data() + cfg.size() - 8, chip_id)});
    cands.push_back({"seed", key_from_seed(seed, uid_chk, chip_id)});
    const Cand *picked = nullptr;
    for (const Cand &c : cands)
        if (key_sum(c.key) == boot_sum) { picked = &c; break; } // "uid" first, as BMCU-Flasher prefers it
    if (!picked) {
        char msg[128];
        int  n = std::snprintf(msg, sizeof(msg), "isp_key checksum mismatch: boot=0x%02x", boot_sum);
        for (const Cand &c : cands)
            n += std::snprintf(msg + n, sizeof(msg) - size_t(n), " %s=0x%02x", c.name, key_sum(c.key));
        plain_fail(msg);
    }
    const Key8 key = picked->key;
    char kmsg[64];
    std::snprintf(kmsg, sizeof(kmsg), "isp_key ok (src=%s key_sum=0x%02x)", picked->name, boot_sum);
    ctx.log(LogLevel::Info, kmsg);
    ctx.log(LogLevel::Info, "unlock ok");

    if (wpr != Bytes{0xFF, 0xFF, 0xFF, 0xFF}) {
        ctx.log(LogLevel::Warn, "code flash protected (WPR=" + hex(wpr) + ") -> clearing WPR + RDPR");
        Bytes c = cfg12;
        c[0] = 0xA5;
        c[1] = 0x5A;
        std::fill(c.begin() + 8, c.begin() + 12, 0xFF);
        Frame r = isp.txrx(build_write_cfg(CFG_MASK_RDPR_USER_DATA_WPR, c), CMD_WRITE_CFG, 2000);
        if (r.code != 0x00)
            plain_fail("write_cfg (unprotect) failed");
        port->sleep_ms(80);
        Bytes cfg2 = read_cfg(" after unprotect");
        Bytes wpr2(cfg2.begin() + 10, cfg2.begin() + 14);
        ctx.log(LogLevel::Info, "cfg_wpr(after)=" + hex(wpr2));
        if (wpr2 != Bytes{0xFF, 0xFF, 0xFF, 0xFF})
            plain_fail("WPR still not cleared (needs reset/power-cycle to apply option bytes). Re-enter bootloader and retry.");
        ctx.log(LogLevel::Info, "unprotect ok");
    }

    // Last point where stopping leaves the old firmware in place.
    if (ctx.cancelled()) {
        ctx.log(LogLevel::Warn, "cancelled before erase: leaving bootloader, the old firmware is untouched");
        leave_bootloader(isp, opt, ttl ? nullptr : &autodi);
        try { port->close(); } catch (...) {}
        throw FlashCancelled();
    }

    ctx.stage(Stage::Erase);
    ctx.log(LogLevel::Info, "stage erase sectors=" + std::to_string(FLASH_KB) + " (full erase)");
    {
        Frame r = isp.txrx(build_erase(FLASH_KB), CMD_ERASE, 12000);
        if (r.code != 0x00)
            plain_fail("erase failed");
    }
    ctx.log(LogLevel::Info, "erase ok");

    const uint32_t tail_addr = ((FLASH_BYTES - CHUNK) / CHUNK) * CHUNK;
    {
        const Bytes ff(CHUNK, 0xFF);
        Frame r = isp.txrx(build_verify(tail_addr, 0x00, xor_crypt(ff.data(), ff.size(), key)), CMD_VERIFY, 1800);
        if (r.code != 0x00)
            plain_fail("erase incomplete (tail not erased) addr=" + hex32(tail_addr));
    }

    if (!opt.no_fast) {
        ctx.log(LogLevel::Info, "stage set_baud mcu=" + std::to_string(opt.fast_baud));
        const bool can_fallback = ttl && opt.fast_baud != opt.baud;
        Frame r;
        try {
            r = isp.txrx(build_set_baud(opt.fast_baud), CMD_SET_BAUD, 1200);
        } catch (const std::exception &e) {
            if (can_fallback) {
                try { port->close(); } catch (...) {}
                throw FastBaudFallback(std::string("set_baud: ") + e.what());
            }
            throw;
        }
        if (r.code != 0x00) {
            try { port->close(); } catch (...) {}
            if (can_fallback)
                throw FastBaudFallback("set_baud failed");
            throw FlashError("set_baud failed");
        }
        port->sleep_ms(30);
        try {
            isp.set_baud(opt.fast_baud);
            isp.flush();
        } catch (const std::exception &e) {
            if (can_fallback) {
                try { port->close(); } catch (...) {}
                throw FastBaudFallback(std::string("host set_baud: ") + e.what());
            }
            throw;
        }
        fast_active = can_fallback;
        ctx.log(LogLevel::Info, "set_baud ok host_baud=" + std::to_string(isp.baud));
    }

    ctx.stage(Stage::Program);
    const uint8_t pad_byte = 0x00;
    ctx.log(LogLevel::Info, "stage program addr=0x00000000.." + hex32(uint32_t(blocks * CHUNK)) + " blocks=" + std::to_string(blocks) +
                                " chunk=" + std::to_string(CHUNK) + " verify=" + (opt.verify ? "on" : "off"));
    const uint64_t tprog = port->now_ms();
    ctx.progress(0);
    int last_ui = -1, last_log = -1;
    for (size_t i = 0; i < blocks; ++i) {
        const uint32_t addr = uint32_t(i * CHUNK);
        Frame r = fast_txrx(build_program(addr, pad_byte, xor_crypt(fw_pad.data() + addr, CHUNK, key)), CMD_PROGRAM, 5000);
        if (r.code != 0x00)
            fail("program failed at " + hex32(addr));
        const int stage_pct = int((i + 1) * 100 / blocks);
        const int ui_pct    = int((i + 1) * 50 / blocks);
        if (ui_pct != last_ui) { last_ui = ui_pct; ctx.progress(ui_pct); }
        if (stage_pct % 10 == 0 && stage_pct != last_log) {
            last_log = stage_pct;
            ctx.log(LogLevel::Info, "program " + std::to_string(stage_pct) + "% addr=" + hex32(uint32_t((i + 1) * CHUNK)));
        }
    }
    const uint32_t flush_addr = uint32_t(blocks * CHUNK);
    ctx.log(LogLevel::Info, "stage program_flush addr=" + hex32(flush_addr) + " (A5 empty)");
    {
        Frame r = fast_txrx(build_program(flush_addr, pad_byte, Bytes{}), CMD_PROGRAM, 5000);
        if (r.code != 0x00)
            fail("program_flush failed");
    }
    ctx.progress(50);
    {
        const double dt = std::max(double(port->now_ms() - tprog) / 1000.0, 1e-3);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "program done in %.3fs (%.1f KiB/s)", dt, fw.size() / 1024.0 / dt);
        ctx.log(LogLevel::Info, msg);
    }

    if (opt.verify) {
        ctx.stage(Stage::Verify);
        ctx.log(LogLevel::Info, "stage isp_key (A3) before verify");
        Frame k2 = fast_txrx(build_isp_key(seed), CMD_ISP_KEY, 1200);
        if (k2.code != 0x00 || k2.data.empty() || k2.data[0] != boot_sum)
            fail("isp_key before verify failed");

        std::vector<size_t> idx;
        for (size_t i = 0; i < blocks; ++i) {
            const bool is_last = i == blocks - 1;
            if ((i % verify_every == 0 && !is_last) || (opt.verify_last && is_last))
                idx.push_back(i);
        }
        if (idx.empty()) {
            ctx.log(LogLevel::Info, "stage verify skipped (no blocks selected)");
        } else {
            ctx.log(LogLevel::Info, "stage verify blocks=" + std::to_string(idx.size()) + "/" + std::to_string(blocks));
            last_ui = -1;
            last_log = -1;
            for (size_t j = 0; j < idx.size(); ++j) {
                const uint32_t addr = uint32_t(idx[j] * CHUNK);
                Frame r = fast_txrx(build_verify(addr, pad_byte, xor_crypt(fw_pad.data() + addr, CHUNK, key)), CMD_VERIFY, 1800);
                if (r.code != 0x00)
                    fail("verify failed at " + hex32(addr));
                const int stage_pct = int((j + 1) * 100 / idx.size());
                const int ui_pct    = 50 + int((j + 1) * 50 / idx.size());
                if (ui_pct != last_ui) { last_ui = ui_pct; ctx.progress(ui_pct); }
                if (stage_pct % 10 == 0 && stage_pct != last_log) {
                    last_log = stage_pct;
                    ctx.log(LogLevel::Info, "verify " + std::to_string(stage_pct) + "% addr=" + hex32(addr));
                }
            }
            ctx.log(LogLevel::Info, "verify ok");
        }
    }

    ctx.stage(Stage::Finish);
    ctx.log(LogLevel::Info, "stage isp_end");
    leave_bootloader(isp, opt, ttl ? nullptr : &autodi);
    const uint64_t total = port->now_ms() - t0;
    port->close();
    ctx.progress(100);
    char okmsg[48];
    std::snprintf(okmsg, sizeof(okmsg), "OK total=%.3fs", double(total) / 1000.0);
    ctx.log(LogLevel::Info, okmsg);
    ctx.stage(Stage::Done);
}

} // namespace

void flash_firmware(const Bytes &firmware, const TransportFactory &open, const FlashOptions &opt, const FlashCallbacks &cb)
{
    if (firmware.empty())
        throw FlashError("firmware is empty");
    if (firmware.size() > FLASH_BYTES)
        throw FlashError("firmware is larger than the 64 KiB flash of the CH32V203");
    Ctx ctx{opt, cb};
    try {
        flash_once(firmware, open, opt, ctx);
    } catch (const FastBaudFallback &e) {
        if (opt.mode != Mode::Ttl || opt.no_fast || opt.fast_baud == opt.baud)
            throw FlashError(e.what());
        ctx.log(LogLevel::Warn, "TTL fast baud " + std::to_string(opt.fast_baud) + " failed (" + e.what() + "). Retrying the full flash at " +
                                    std::to_string(opt.baud) + " baud.");
        ctx.log(LogLevel::Action, "TTL fallback: keep/put BMCU in bootloader mode when prompted. The flash will restart from the beginning at normal speed.");
        ctx.progress(0);
        FlashOptions o2 = opt;
        o2.no_fast      = true;
        Ctx ctx2{o2, cb};
        flash_once(firmware, open, o2, ctx2);
    }
}

// ---- SHA-256 / CRC32 ---------------------------------------------------------------------------------------------
namespace {
struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t  buf[64];
    size_t   len = 0;
    uint64_t bits = 0;

    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t *p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t *p, size_t n)
    {
        bits += uint64_t(n) * 8;
        while (n > 0) {
            const size_t take = std::min(n, size_t(64) - len);
            std::memcpy(buf + len, p, take);
            len += take; p += take; n -= take;
            if (len == 64) { block(buf); len = 0; }
        }
    }
    std::string final_hex()
    {
        const uint64_t total = bits;
        const uint8_t one = 0x80, zero = 0x00;
        update(&one, 1);
        while (len != 56)
            update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i)
            lenb[i] = uint8_t(total >> (56 - 8 * i));
        update(lenb, 8);
        uint8_t out[32];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j)
                out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
        return hex(out, 32);
    }
};
} // namespace

std::string sha256_hex(const uint8_t *data, size_t n)
{
    Sha256 s;
    s.update(data, n);
    return s.final_hex();
}

uint32_t crc32(const uint8_t *data, size_t n, uint32_t crc)
{
    static uint32_t table[256];
    static bool     init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void) init;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::string crc32_hex(uint32_t crc)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", unsigned(crc));
    return buf;
}

// ---- catalogue ---------------------------------------------------------------------------------------------------
bool is_safe_ref(const std::string &ref)
{
    if (ref.empty() || ref.size() > 64 || ref[0] == '.' || ref[0] == '-')
        return false;
    for (char c : ref)
        if (!(std::isalnum((unsigned char) c) || c == '.' || c == '_' || c == '-'))
            return false;
    return ref.find("..") == std::string::npos;
}

bool is_safe_rel_path(const std::string &rel)
{
    if (rel.empty() || rel.size() > 256 || rel[0] == '/' || rel[0] == '\\' || rel.find("..") != std::string::npos ||
        rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos)
        return false;
    for (char c : rel)
        if ((unsigned char) c < 0x20 || (unsigned char) c >= 0x7F)
            return false;
    return true;
}

std::string raw_base_url(const std::string &ref) { return std::string("https://raw.githubusercontent.com/") + FW_REPO + "/" + ref + "/"; }
std::string version_url(const std::string &ref) { return raw_base_url(ref) + "version"; }
std::string manifest_url(const std::string &ref) { return raw_base_url(ref) + "firmwares/manifest.txt"; }
std::string releases_api_url() { return std::string("https://api.github.com/repos/") + FW_REPO + "/releases?per_page=30"; }

std::string firmware_url(const std::string &ref, const std::string &rel_path)
{
    // urllib.parse.quote(rel_path, safe="/")
    std::string out = raw_base_url(ref) + "firmwares/";
    static const char *digits = "0123456789ABCDEF";
    for (unsigned char c : rel_path) {
        if (std::isalnum(c) || c == '_' || c == '.' || c == '-' || c == '~' || c == '/')
            out += char(c);
        else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

static std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b])) ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool is_version_x4(const std::string &s)
{
    int parts = 0;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = i;
        while (j < s.size() && std::isdigit((unsigned char) s[j])) ++j;
        if (j == i || j - i > 6)
            return false;
        ++parts;
        if (j == s.size())
            break;
        if (s[j] != '.')
            return false;
        i = j + 1;
        if (i == s.size())
            return false;
    }
    return parts == 4;
}

std::string normalize_version(const std::string &text)
{
    std::string s = trim(text);
    const size_t nl = s.find_first_of("\r\n");
    if (nl != std::string::npos)
        s = trim(s.substr(0, nl));
    return is_version_x4(s) ? s : std::string();
}

static std::vector<long> version_parts(const std::string &s)
{
    std::vector<long> v;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, '.'))
        v.push_back(std::strtol(part.c_str(), nullptr, 10));
    return v;
}

int compare_versions(const std::string &a, const std::string &b)
{
    const auto va = version_parts(a), vb = version_parts(b);
    for (size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
        const long x = i < va.size() ? va[i] : 0, y = i < vb.size() ? vb[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

std::string display_version(const std::string &x4)
{
    // remote_get_version(): "11.00.00.00" -> V11.0, "10.50.00.00" -> V10.5, "10.55..." -> V10.55
    const auto v = version_parts(x4);
    if (v.size() < 2)
        return x4;
    const long major = v[0], minor = v[1];
    if (minor == 0)
        return "V" + std::to_string(major) + ".0";
    if (minor % 10 == 0)
        return "V" + std::to_string(major) + "." + std::to_string(minor / 10);
    return "V" + std::to_string(major) + "." + std::to_string(minor);
}

Manifest parse_manifest(const std::string &text)
{
    Manifest m;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        // split(None, 3): sha crc size rel_path (the path may hold spaces)
        std::stringstream ls(line);
        std::string sha, crc, size_s;
        if (!(ls >> sha >> crc >> size_s))
            continue;
        std::string rel;
        std::getline(ls, rel);
        rel = trim(rel);
        if (rel.empty())
            continue;
        std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::transform(crc.begin(), crc.end(), crc.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (sha.size() != 64 || crc.size() != 8 || !std::all_of(sha.begin(), sha.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
            !std::all_of(crc.begin(), crc.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
            !std::all_of(size_s.begin(), size_s.end(), [](unsigned char c) { return std::isdigit(c); }) || size_s.size() > 9)
            continue;
        m[rel] = ManifestEntry{sha, crc, size_t(std::stoul(size_s))};
    }
    if (m.empty())
        throw std::runtime_error("manifest empty");
    return m;
}

bool verify_firmware(const Bytes &data, const ManifestEntry &e, std::string &error)
{
    const std::string crc = crc32_hex(crc32(data.data(), data.size()));
    const std::string sha = sha256_hex(data.data(), data.size());
    if (data.size() != e.size || crc != e.crc32 || sha != e.sha256) {
        error = "verify failed size=" + std::to_string(data.size()) + "/" + std::to_string(e.size) + " crc=" + crc + "/" + e.crc32 +
                " sha=" + sha.substr(0, 12) + ".../" + e.sha256.substr(0, 12) + "...";
        return false;
    }
    return true;
}

std::string build_rel_path(const std::string &force, bool autoload, bool rgb, const std::string &slot, const std::string &retract)
{
    // FirmwareSelector.build() / _online_fw()
    std::string suffix = "auto";
    if (slot != "AMS_AUTO") {
        const size_t us = slot.find('_');
        suffix = us == std::string::npos ? slot : slot.substr(us + 1);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    }
    return force + "/" + (autoload ? "AUTOLOAD" : "NO_AUTOLOAD") + "/" + (rgb ? "FILAMENT_RGB_ON" : "FILAMENT_RGB_OFF") + "/" + slot +
           "/ams_" + suffix + "_" + retract + ".bin";
}

std::vector<Variant> variants_from_manifest(const Manifest &m)
{
    std::vector<Variant> out;
    for (const auto &kv : m) {
        const std::string &rel = kv.first;
        std::vector<std::string> parts;
        std::stringstream ss(rel);
        std::string p;
        while (std::getline(ss, p, '/'))
            parts.push_back(p);
        if (parts.size() != 5 || !is_safe_rel_path(rel))
            continue;
        Variant v;
        v.force = parts[0];
        if (parts[1] == "AUTOLOAD") v.autoload = true;
        else if (parts[1] == "NO_AUTOLOAD") v.autoload = false;
        else continue;
        if (parts[2] == "FILAMENT_RGB_ON") v.rgb = true;
        else if (parts[2] == "FILAMENT_RGB_OFF") v.rgb = false;
        else continue;
        v.slot = parts[3];
        if (v.slot.rfind("AMS_", 0) != 0)
            continue;
        const std::string &file = parts[4];
        const size_t last_us = file.rfind('_');
        if (last_us == std::string::npos || file.size() < 4 || file.compare(file.size() - 4, 4, ".bin") != 0)
            continue;
        v.retract  = file.substr(last_us + 1, file.size() - 4 - last_us - 1);
        v.rel_path = rel;
        if (build_rel_path(v.force, v.autoload, v.rgb, v.slot, v.retract) != rel)
            continue;
        out.push_back(std::move(v));
    }
    return out;
}

double retract_value(const std::string &retract)
{
    std::string s = retract;
    if (!s.empty() && (s.back() == 'f' || s.back() == 'F'))
        s.pop_back();
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

std::string retract_display(const std::string &retract)
{
    // metres -> cm as BMCU-Flasher shows them ("0.095f" -> "9.5cm")
    const double cm = retract_value(retract) * 100.0;
    char buf[32];
    if (std::abs(cm - double(long(cm + 0.5))) < 1e-6)
        std::snprintf(buf, sizeof(buf), "%ld cm", long(cm + 0.5));
    else
        std::snprintf(buf, sizeof(buf), "%.1f cm", cm);
    return buf;
}

std::vector<Release> parse_releases(const std::string &json)
{
    std::vector<Release> out;
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_array())
        return out;
    for (const auto &r : j) {
        if (!r.is_object())
            continue;
        auto str = [&](const char *k) { return r.contains(k) && r[k].is_string() ? r[k].get<std::string>() : std::string(); };
        if (r.contains("draft") && r["draft"].is_boolean() && r["draft"].get<bool>())
            continue;
        Release rel;
        rel.tag          = str("tag_name");
        rel.name         = str("name");
        rel.published_at = str("published_at");
        rel.prerelease   = r.contains("prerelease") && r["prerelease"].is_boolean() && r["prerelease"].get<bool>();
        if (!is_safe_ref(rel.tag))
            continue;
        out.push_back(std::move(rel));
    }
    return out;
}

}} // namespace Slic3r::Bmcu
