// BMCU firmware flasher (WCH ISP protocol of the CH32V203 on the BMCU board) and the BMCU firmware catalogue
// (version file + manifest of jarczakpawel/BMCU-C-PJARCZAK).
//
// The flash sequence, packet layout, key derivation, AutoDI line search and the manifest format are ported from
// BMCU-Flasher by Pawel Jarczak, https://github.com/jarczakpawel/BMCU-Flasher (bmcu_flasher.py v1.4),
// MIT License, Copyright (c) 2026 Pawel Jarczak. The MIT notice is kept in docs/zoukei/bmcu-flasher.md.
//
// This file has no wx / boost / network dependency: the serial port is an injected Transport (see
// BmcuSerialPort.hpp for the real one) so the whole sequence can run against a mock bootloader in tests.
// Native USB (Toaiot boards without CH340, WCH ISP 4348:55E0) is not ported: it needs libusb or the WCH CH375 DLL.
#ifndef slic3r_Utils_BmcuFlasher_hpp_
#define slic3r_Utils_BmcuFlasher_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r { namespace Bmcu {

using Bytes = std::vector<uint8_t>;
using Key8  = std::array<uint8_t, 8>;

// ---- protocol constants (bmcu_flasher.py) ------------------------------------------------------------------------
constexpr uint8_t CMD_IDENTIFY  = 0xA1;
constexpr uint8_t CMD_ISP_END   = 0xA2;
constexpr uint8_t CMD_ISP_KEY   = 0xA3;
constexpr uint8_t CMD_ERASE     = 0xA4;
constexpr uint8_t CMD_PROGRAM   = 0xA5;
constexpr uint8_t CMD_VERIFY    = 0xA6;
constexpr uint8_t CMD_READ_CFG  = 0xA7;
constexpr uint8_t CMD_WRITE_CFG = 0xA8;
constexpr uint8_t CMD_SET_BAUD  = 0xC5;

constexpr uint8_t DEVICE_ID   = 0x31; // CH32V203
constexpr uint8_t DEVICE_TYPE = 0x19;
constexpr uint8_t CFG_MASK    = 0x1F;
constexpr uint8_t CFG_MASK_RDPR_USER_DATA_WPR = 0x07;

constexpr unsigned FLASH_KB    = 64;   // CH32V203C8T6
constexpr unsigned FLASH_BYTES = FLASH_KB * 1024;
constexpr unsigned CHUNK       = 56;   // bytes per program / verify packet
constexpr unsigned SEED_LEN    = 0x1E;

constexpr unsigned DEFAULT_BAUD      = 115200;
constexpr unsigned DEFAULT_FAST_BAUD = 1000000;

// USB-serial adapters known by BMCU-Flasher (name, vid, pid, fast baud).
struct Adapter { const char *name; uint16_t vid; uint16_t pid; unsigned fast_baud; };
const std::vector<Adapter> &known_adapters();
constexpr uint16_t CH340_VID = 0x1A86;
constexpr uint16_t CH340_PID = 0x7523;

// ---- packets -----------------------------------------------------------------------------------------------------
uint8_t checksum(const uint8_t *data, size_t n);
Bytes   pack_req(const Bytes &payload);                 // 57 AB <payload> <sum(payload)>
Bytes   build_identify(uint8_t device_id = DEVICE_ID, uint8_t device_type = DEVICE_TYPE);
Bytes   build_read_cfg(uint8_t mask = CFG_MASK);
Bytes   build_write_cfg(uint8_t mask, const Bytes &cfg12);
Bytes   build_isp_key(const Bytes &seed);
Bytes   build_erase(uint32_t sectors);
Bytes   build_set_baud(uint32_t baud);
Bytes   build_program(uint32_t address, uint8_t padding, const Bytes &data);
Bytes   build_verify(uint32_t address, uint8_t padding, const Bytes &data);
Bytes   build_isp_end(uint8_t reason);

Key8  key_from_seed(const Bytes &seed, uint8_t uid_chk, uint8_t chip_id); // throws if seed < 8 bytes
Key8  key_from_uid(const uint8_t *uid8, uint8_t chip_id);
Bytes xor_crypt(const uint8_t *data, size_t n, const Key8 &key);
uint8_t key_sum(const Key8 &key);

struct Frame { uint8_t cmd = 0; uint8_t code = 0; Bytes data; };

// Reassembles 55 AA <cmd> <code> <len16> <data> <sum> response frames from a byte stream. Frames with a bad
// checksum or another command are dropped, like WchIsp.recv() does.
class FrameReader
{
public:
    void push(const uint8_t *data, size_t n) { m_rx.insert(m_rx.end(), data, data + n); }
    bool next(uint8_t expect_cmd, Frame &out);
    void clear() { m_rx.clear(); }
private:
    Bytes m_rx;
};

// ---- transport ---------------------------------------------------------------------------------------------------
class Transport
{
public:
    virtual ~Transport() = default;
    virtual void   write(const uint8_t *data, size_t n) = 0;          // throws std::runtime_error
    virtual size_t read_available(uint8_t *buf, size_t cap) = 0;      // non blocking, 0 = nothing yet
    virtual void   purge() = 0;                                       // drop pending rx / tx
    virtual void   set_baud(unsigned baud) = 0;
    virtual void   set_dtr(bool on) = 0;
    virtual void   set_rts(bool on) = 0;
    virtual void   close() = 0;                                       // sets DTR and RTS, then closes
    virtual void     sleep_ms(unsigned ms);                           // overridable so a mock can use virtual time
    virtual uint64_t now_ms();
};

// Opens the port at `baud`. Called once per attempt (the TTL fallback reopens it at the normal baud).
using TransportFactory = std::function<std::unique_ptr<Transport>(unsigned baud)>;

enum class Mode { Usb, Ttl };
enum class LogLevel { Info, Warn, Action, Error };

struct FlashOptions
{
    Mode     mode         = Mode::Usb;
    unsigned baud         = DEFAULT_BAUD;
    unsigned fast_baud    = DEFAULT_FAST_BAUD;
    bool     no_fast      = false;
    bool     verify       = true;
    unsigned verify_every = 1;
    bool     verify_last  = false; // BMCU-Flasher does not verify the last (padded) block by default
    unsigned ttl_wait_ms  = 12000; // how long TTL mode waits for the user to enter the bootloader
};

enum class Stage { Connect, Unlock, Erase, Program, Verify, Finish, Done };

struct FlashCallbacks
{
    std::function<void(LogLevel, const std::string &)> log;
    std::function<void(int /*0..100*/)>               progress;
    std::function<void(Stage)>                        stage;
    // Polled before the erase only: once the flash is erased the sequence always runs to the end.
    std::function<bool()>                             cancelled;
};

class FlashError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class FlashCancelled : public FlashError { public: FlashCancelled() : FlashError("cancelled before erase") {} };

// Flashes `firmware` (raw .bin, <= 64 KiB). Throws FlashError / FlashCancelled. TTL retries once at `baud` when the
// fast baud transfer fails, like flash_firmware() in bmcu_flasher.py.
void flash_firmware(const Bytes &firmware, const TransportFactory &open, const FlashOptions &opt, const FlashCallbacks &cb);

// ---- hashes ------------------------------------------------------------------------------------------------------
std::string sha256_hex(const uint8_t *data, size_t n);
uint32_t    crc32(const uint8_t *data, size_t n, uint32_t crc = 0);
std::string crc32_hex(uint32_t crc); // 8 upper-case hex digits

// ---- firmware catalogue ------------------------------------------------------------------------------------------
constexpr const char *FW_REPO      = "jarczakpawel/BMCU-C-PJARCZAK";
constexpr const char *FW_REPO_URL  = "https://github.com/jarczakpawel/BMCU-C-PJARCZAK";
constexpr const char *FLASHER_URL  = "https://github.com/jarczakpawel/BMCU-Flasher";

std::string raw_base_url(const std::string &ref);     // https://raw.githubusercontent.com/<repo>/<ref>/
std::string version_url(const std::string &ref);      // <base>version
std::string manifest_url(const std::string &ref);     // <base>firmwares/manifest.txt
std::string firmware_url(const std::string &ref, const std::string &rel_path); // percent-encoded
std::string releases_api_url();                       // GitHub API releases list
bool        is_safe_ref(const std::string &ref);
bool        is_safe_rel_path(const std::string &rel);

// "11.00.00.00\n" -> "11.00.00.00" (first line, trimmed). Empty when the text is not x.x.x.x.
std::string normalize_version(const std::string &text);
bool        is_version_x4(const std::string &s);
int         compare_versions(const std::string &a, const std::string &b); // x.x.x.x numeric compare, -1/0/1
std::string display_version(const std::string &x4); // "11.00.00.00" -> "V11.0", "10.50.00.00" -> "V10.5"

struct ManifestEntry { std::string sha256; std::string crc32; size_t size = 0; };
using Manifest = std::map<std::string, ManifestEntry>;
Manifest parse_manifest(const std::string &text); // throws std::runtime_error("manifest empty")

// Checks size, CRC32 and SHA-256 against the manifest entry; `error` says which one failed.
bool verify_firmware(const Bytes &data, const ManifestEntry &e, std::string &error);

// One flashable variant: <force>/<AUTOLOAD|NO_AUTOLOAD>/<FILAMENT_RGB_ON|OFF>/<AMS_x>/ams_<x>_<retract>.bin
struct Variant
{
    std::string force;    // "standard(A1)", "soft_load(A1)", "high_force_load(P1S)", ...
    bool        autoload = true;
    bool        rgb      = true;
    std::string slot;     // "AMS_AUTO", "AMS_A".."AMS_D"
    std::string retract;  // "0.095f", "0.10f", ...
    std::string rel_path;
};
std::vector<Variant> variants_from_manifest(const Manifest &m);
std::string build_rel_path(const std::string &force, bool autoload, bool rgb, const std::string &slot, const std::string &retract);
std::string retract_display(const std::string &retract); // "0.095f" -> "9.5 cm", "0.60f" -> "60 cm"
double      retract_value(const std::string &retract);   // "0.095f" -> 0.095 (sort key)

struct Release { std::string tag; std::string name; std::string published_at; bool prerelease = false; };
// GitHub /releases JSON -> releases with a safe tag, drafts dropped, in the API order (newest first).
std::vector<Release> parse_releases(const std::string &json);

}} // namespace Slic3r::Bmcu

#endif
