#pragma once
// BMCU (open-source AMS-lite) helpers on top of MachineObject. Pure classification lives in slic3r/Utils/BmcuHms.hpp.
#include "slic3r/Utils/BmcuHms.hpp"

namespace Slic3r {
class MachineObject;

namespace DevBmcu {

// Preference "bmcu_ignore_fw_mismatch" (default on): ignore the AMS firmware-mismatch warning when the unit is a BMCU.
bool IgnoreFwMismatchEnabled();

// Is the AMS unit `ams_id` (0..7, AMS-HT 128..135) of this printer a BMCU? (serial starts with "0EA")
bool IsBmcu(MachineObject* obj, int ams_id);

// HMS item / print_error is an AMS firmware-mismatch warning for a BMCU unit and the preference is on.
bool ShouldIgnoreHms(MachineObject* obj, unsigned attr, unsigned code);
bool ShouldIgnorePrintError(MachineObject* obj, int print_error);

} // namespace DevBmcu
} // namespace Slic3r
