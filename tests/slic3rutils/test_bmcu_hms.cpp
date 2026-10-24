// BMCU vs. "firmware of AMS x does not match the printer": pure classification of HMS / print_error codes and the
// "is this unit a BMCU" lookup on synthetic data (src/slic3r/Utils/BmcuHms.hpp, GUI/DeviceCore/DevBmcu.h).
#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BmcuHms.hpp"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"

#include <map>
#include <string>

using namespace Slic3r;
using namespace Slic3r::BmcuHms;

namespace {
struct FakeVer { std::string sn; };

// attr/code from a 16-hex long HMS code "MMNNPPRR LLLLCCCC" as the printer sends it
void split(const char* hex16, unsigned& attr, unsigned& code)
{
    const unsigned long long v = std::stoull(hex16, nullptr, 16);
    attr = unsigned(v >> 32);
    code = unsigned(v & 0xFFFFFFFFu);
}
std::optional<int> hms_id(const char* hex16)
{
    unsigned a, c;
    split(hex16, a, c);
    return fw_mismatch_ams_id_from_hms(a, c);
}
} // namespace

TEST_CASE("BMCU serial rule", "[bmcu]")
{
    CHECK(is_bmcu_serial("0EA0123456"));
    CHECK(is_bmcu_serial("0ea01"));
    CHECK_FALSE(is_bmcu_serial("00M09A123"));
    CHECK_FALSE(is_bmcu_serial("0E"));
    CHECK_FALSE(is_bmcu_serial(""));
}

TEST_CASE("HMS firmware-mismatch codes yield the AMS unit id", "[bmcu]")
{
    // from resources/hms/hms_en_*.json
    CHECK(hms_id("0500040000010044") == 0); // AMS A
    CHECK(hms_id("0501040000010044") == 1);
    CHECK(hms_id("0502040000010044") == 2);
    CHECK(hms_id("0503040000010044") == 3); // AMS D
    CHECK(hms_id("0500050000010010") == 0);
    CHECK(hms_id("0507050000010010") == 7); // AMS H
    CHECK(hms_id("0580040000010045") == 128); // AMS-HT A
    CHECK(hms_id("0587050000010010") == 135); // AMS-HT H
    CHECK(hms_id("0500000000014" "09D") == 0);
    CHECK(hms_id("0582000000" "01409C") == 130);
}

TEST_CASE("Other HMS codes are not firmware-mismatch warnings", "[bmcu]")
{
    CHECK_FALSE(hms_id("0500040000010046")); // Laser Module firmware
    CHECK_FALSE(hms_id("0500040000010047")); // Air Pump firmware
    CHECK_FALSE(hms_id("0500040000010048")); // Cutting Module firmware
    CHECK_FALSE(hms_id("0700010000020001")); // AMS module, other error
    CHECK_FALSE(hms_id("0300040000010044")); // wrong module
    CHECK_FALSE(hms_id("0508040000010044")); // AMS id out of range for 0044
    CHECK_FALSE(hms_id("0500040000010045")); // 0045 is AMS-HT only
    CHECK_FALSE(hms_id("0500060000010044")); // wrong part
}

TEST_CASE("print_error firmware-mismatch codes", "[bmcu]")
{
    CHECK(fw_mismatch_ams_id_from_print_error(0x0500409D) == 0);
    CHECK(fw_mismatch_ams_id_from_print_error(0x0503409D) == 3);
    CHECK(fw_mismatch_ams_id_from_print_error(0x0580409C) == 128);
    CHECK(fw_mismatch_ams_id_from_print_error(0x05004033) == kAnyAms);
    CHECK_FALSE(fw_mismatch_ams_id_from_print_error(0x0500403C)); // nozzle mismatch
    CHECK_FALSE(fw_mismatch_ams_id_from_print_error(0x0508409D));
    CHECK_FALSE(fw_mismatch_ams_id_from_print_error(0));
}

TEST_CASE("BMCU lookup and filter decisions on synthetic data", "[bmcu]")
{
    std::map<int, FakeVer> mixed{{0, {"0EA0001"}}, {1, {"00M09A1"}}};
    CHECK(is_bmcu_unit(mixed, 0));
    CHECK_FALSE(is_bmcu_unit(mixed, 1));
    CHECK_FALSE(is_bmcu_unit(mixed, 2)); // unknown unit
    CHECK_FALSE(all_units_are_bmcu(mixed));

    std::map<int, FakeVer> only{{0, {"0EA0001"}}, {128, {"0eA0002"}}};
    CHECK(all_units_are_bmcu(only));
    CHECK_FALSE(all_units_are_bmcu(std::map<int, FakeVer>{}));

    unsigned a, c;
    split("0500040000010044", a, c); // AMS A -> BMCU
    CHECK(should_ignore_hms(mixed, true, a, c));
    CHECK_FALSE(should_ignore_hms(mixed, false, a, c)); // preference off
    split("0501040000010044", a, c); // AMS B -> real AMS
    CHECK_FALSE(should_ignore_hms(mixed, true, a, c));
    split("0500040000010046", a, c); // laser module
    CHECK_FALSE(should_ignore_hms(mixed, true, a, c));

    CHECK(should_ignore_print_error(mixed, true, 0x0500409D));
    CHECK_FALSE(should_ignore_print_error(mixed, true, 0x0501409D));
    CHECK_FALSE(should_ignore_print_error(mixed, true, 0x05004033)); // not all BMCU
    CHECK(should_ignore_print_error(only, true, 0x05004033));
    CHECK_FALSE(should_ignore_print_error(only, false, 0x05004033));
}

TEST_CASE("BMCU lookup works on DevFirmwareVersionInfo", "[bmcu]")
{
    std::map<int, DevFirmwareVersionInfo> vers;
    vers[0].sn = "0EAXYZ";
    vers[1].sn = "03C00A";
    CHECK(is_bmcu_unit(vers, 0));
    CHECK_FALSE(is_bmcu_unit(vers, 1));
}
