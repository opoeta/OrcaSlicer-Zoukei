#ifndef slic3r_Utils_BmcuHms_hpp_
#define slic3r_Utils_BmcuHms_hpp_

// BMCU (open-source AMS-lite clone) vs. the Bambu "AMS firmware does not match the printer" warnings.
//
// A BMCU reports a firmware the printer does not know, so Bambu printers (A1 / A1 mini ...) raise
// "The firmware of AMS A does not match the printer. Please upgrade it on the "Firmware" page.".
// For a BMCU that warning is meaningless (it cannot be upgraded from Bambu's firmware page), so Orca Zoukei
// hides it. This header only holds pure functions (no wx, no MachineObject) so they can be unit tested:
//   - the BMCU serial rule ("0EA..."),
//   - classification of an HMS item (attr, code) or a print_error as an "AMS firmware mismatch" and the
//     AMS unit id it refers to (derived from the code itself),
//   - the "is this unit a BMCU" lookup over any map<int, {sn}> (ams id -> module version info).
//
// Codes covered (checked against resources/hms/hms_en_*.json):
//   HMS  05 <id> 04 00 | 0001 0044   "The firmware of AMS A..D does not match the printer"      id 0..7
//   HMS  05 <id> 05 00 | 0001 0010   same text (newer firmware), AMS A..H and AMS-HT A..H          id 0..7 / 128..135
//   HMS  05 <id> 04 00 | 0001 0045   "The firmware of AMS-HT A..H does not match the printer"     id 128..135
//   print_error 05<id>409D            "... does not match the printer; the device cannot continue working" id 0..7
//   print_error 05<id>409C            same for AMS-HT                                                id 128..135
//   print_error 05004033              "The AMS firmware does not match the printer" (no unit id -> any)
//
// Rule credited to OrcaStudio (https://github.com/jarczakpawel/OrcaStudio, AGPL-3.0).

#include <optional>
#include <string>

namespace Slic3r { namespace BmcuHms {

// "Any AMS unit" (the code does not name one).
constexpr int kAnyAms = -1;

// BMCU serials start with "0EA".
inline bool is_bmcu_serial(const std::string& sn)
{
    return sn.size() >= 3 && sn[0] == '0' && (sn[1] == 'E' || sn[1] == 'e') && (sn[2] == 'A' || sn[2] == 'a');
}

// AMS ("ams/N", N 0..7) and single-tray AMS-HT / AMS-lite ("n3s/N", N 128..135) unit ids.
inline bool is_ams_unit_id(int id) { return (id >= 0 && id < 8) || (id >= 128 && id < 136); }

namespace detail {
inline bool is_fw_mismatch_short(unsigned module, unsigned num, unsigned msg)
{
    if (module != 0x05)
        return false;
    if (msg == 0x409D)
        return num < 8;
    if (msg == 0x409C)
        return num >= 128 && num < 136;
    return false;
}
} // namespace detail

// HMS item (attr, code as sent by the printer) -> AMS unit id when it is an "AMS firmware does not match the
// printer" warning, std::nullopt otherwise.
inline std::optional<int> fw_mismatch_ams_id_from_hms(unsigned attr, unsigned code)
{
    const unsigned module = (attr >> 24) & 0xFF;
    const unsigned num    = (attr >> 16) & 0xFF;
    const unsigned part   = (attr >> 8) & 0xFF;
    const unsigned msg    = code & 0xFFFF;
    if (module != 0x05)
        return std::nullopt;
    if (detail::is_fw_mismatch_short(module, num, msg))
        return int(num);
    if (part == 0x04 && msg == 0x0044 && num < 8)
        return int(num);
    if (part == 0x04 && msg == 0x0045 && num >= 128 && num < 136)
        return int(num);
    if (part == 0x05 && msg == 0x0010 && is_ams_unit_id(int(num)))
        return int(num);
    return std::nullopt;
}

// print_error (32-bit) -> AMS unit id (kAnyAms for 05004033), std::nullopt when it is not such a warning.
inline std::optional<int> fw_mismatch_ams_id_from_print_error(unsigned err)
{
    if (err == 0x05004033u)
        return kAnyAms;
    if (detail::is_fw_mismatch_short((err >> 24) & 0xFF, (err >> 16) & 0xFF, err & 0xFFFF))
        return int((err >> 16) & 0xFF);
    return std::nullopt;
}

// `versions`: ams id -> object with a `sn` member (MachineObject::get_ams_version() map, or a synthetic one).
template<class Versions> bool is_bmcu_unit(const Versions& versions, int ams_id)
{
    auto it = versions.find(ams_id);
    return it != versions.end() && is_bmcu_serial(it->second.sn);
}

// True when at least one AMS unit is known and all of them are BMCU.
template<class Versions> bool all_units_are_bmcu(const Versions& versions)
{
    bool any = false;
    for (const auto& kv : versions) {
        if (!is_ams_unit_id(kv.first))
            continue;
        if (!is_bmcu_serial(kv.second.sn))
            return false;
        any = true;
    }
    return any;
}

// The warning refers to unit `ams_id` (or any, kAnyAms) and that unit is a BMCU -> it can be ignored.
template<class Versions> bool is_ignorable_for(const Versions& versions, int ams_id)
{
    return ams_id == kAnyAms ? all_units_are_bmcu(versions) : is_bmcu_unit(versions, ams_id);
}

// Combined predicates used by the UI filters. `enabled` is the preference.
template<class Versions> bool should_ignore_hms(const Versions& versions, bool enabled, unsigned attr, unsigned code)
{
    if (!enabled)
        return false;
    const auto id = fw_mismatch_ams_id_from_hms(attr, code);
    return id && is_ignorable_for(versions, *id);
}

template<class Versions> bool should_ignore_print_error(const Versions& versions, bool enabled, unsigned err)
{
    if (!enabled)
        return false;
    const auto id = fw_mismatch_ams_id_from_print_error(err);
    return id && is_ignorable_for(versions, *id);
}

}} // namespace Slic3r::BmcuHms

#endif // slic3r_Utils_BmcuHms_hpp_
