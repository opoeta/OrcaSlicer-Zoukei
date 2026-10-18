// Orca Zoukei: Libre Mode, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// ONE opt-in master switch (Preferences > Developer > Experimental Features > "Libre Mode"), default OFF. With it off the
// slicer behaves exactly as before: nothing here is consulted and slicing / G-code stay byte-identical. With it on:
//  - True Objects (Gravity): the slicer measures what is really under every surface (bed, another object, air), so a face
//    resting on another object prints as a contact surface and only the part over air is a bridge (libslic3r Gravity*).
//  - free Z: objects may sit at any height (per object "Auto drop" off, set by Align & Stack and by Snap & Drag).
//  - Snap & Drag: dragging an object lets it land on the real surface below.
//  - Assembled Boolean mode, per-part XY compensation and Copy / Paste Process Settings in the object list.
#ifndef slic3r_GUI_Zoukei_LibreMode_hpp_
#define slic3r_GUI_Zoukei_LibreMode_hpp_

#include <string>

namespace Slic3r {
namespace GUI {
namespace LibreMode {

// Master switch (app_config "zoukei_libre_mode"). Cheap; safe to call from render paths.
bool enabled();
// Stores the switch, mirrors it into the preset bundle (slice-time key neotko_true_objects) and schedules a new slice.
void set_enabled(bool on);
// Copies the stored switch into the preset bundle. Call once at start-up and whenever the stored value changes.
void sync_to_bundle();

// Snap & Drag sub-options (only meaningful while enabled()). Defaults: snap on, to bed on, as block off.
bool snap_drag();
void set_snap_drag(bool on);
bool snap_to_bed();
void set_snap_to_bed(bool on);
bool move_as_block();
void set_move_as_block(bool on);

// The Libre Mode checkboxes as an ImGui block (used by the Align & Stack panel). Call between ImGui::Begin / End.
void render_imgui_options();

// --- Object list actions (ported from OrcaFS-NeotkoCM, Neotko) ---------------------------------------------------------
// Assembled Boolean mode (per object, ModelObject config key neotko_assemble_boolean, default true = stock union of the
// parts). When off the parts of an assembled object are sliced WITHOUT union, so overlapping parts keep their own perimeters.
bool assemble_boolean_available(); // exactly the situation where the menu item makes sense: Libre Mode on, a multi-part object selected
bool assemble_boolean_checked();   // true = union on (also when the key is absent)
void toggle_assemble_boolean();

// Copy / Paste Process Settings: the per-object overrides of the selected object, by block ("Speed", "Quality", "Strength")
// or all ("" = replaces the target settings on paste; a block merges and leaves the other overrides untouched).
void copy_process_settings(const std::string &category);
void paste_process_settings();
bool has_process_settings_clipboard();

} // namespace LibreMode
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_LibreMode_hpp_
