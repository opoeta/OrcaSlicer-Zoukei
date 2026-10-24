#include "DevBmcu.h"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace DevBmcu {

bool IgnoreFwMismatchEnabled()
{
    auto* app = GUI::wxGetApp().app_config;
    if (!app)
        return true;
    const std::string v = app->get("bmcu_ignore_fw_mismatch");
    return v.empty() || v == "true" || v == "1";
}

bool IsBmcu(MachineObject* obj, int ams_id)
{
    return obj && BmcuHms::is_bmcu_unit(obj->get_ams_version(), ams_id);
}

bool ShouldIgnoreHms(MachineObject* obj, unsigned attr, unsigned code)
{
    if (!obj || !BmcuHms::fw_mismatch_ams_id_from_hms(attr, code))
        return false;
    return BmcuHms::should_ignore_hms(obj->get_ams_version(), IgnoreFwMismatchEnabled(), attr, code);
}

bool ShouldIgnorePrintError(MachineObject* obj, int print_error)
{
    if (!obj || print_error <= 0 || !BmcuHms::fw_mismatch_ams_id_from_print_error(unsigned(print_error)))
        return false;
    return BmcuHms::should_ignore_print_error(obj->get_ams_version(), IgnoreFwMismatchEnabled(), unsigned(print_error));
}

} // namespace DevBmcu
} // namespace Slic3r
