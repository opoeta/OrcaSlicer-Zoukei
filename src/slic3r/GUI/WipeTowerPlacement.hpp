#ifndef slic3r_GUI_WipeTowerPlacement_hpp_
#define slic3r_GUI_WipeTowerPlacement_hpp_

// "Automatically optimize wipe tower placement", ported from Bambu Studio 2.8 (AGPL-3.0).
// Instead of leaving the wipe tower wherever the printer default put it, the tower is seated right
// next to the arranged objects, on the side of the bed the printer's default tower position points to.
// That keeps the nozzle's trips between the objects and the tower short and never overlaps an object.

#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {
class DynamicPrintConfig;

namespace GUI {

class PartPlate;
class PartPlateList;

// Preferences > General (User) > "auto_optimize_wipe_tower_placement" (default on).
bool wipe_tower_optimal_pos_enabled();

// Extra clearance on top of the brim and the arrange gap, so the tower does not touch the objects' brims.
static constexpr double WIPE_TOWER_OPTIMAL_POS_EXTRA_CLEARANCE = 10.0;
// Matches the gap arrange keeps around the wipe tower.
static constexpr double WIPE_TOWER_ARRANGE_GAP = 1.0;

// Slides the tower along the ray from the parts' hull centroid towards the printer's default tower
// direction until it clears the (inflated) hull. All coordinates are world coordinates in mm; on success
// the tower's min (bottom-left) corner is written to out_wt_min.
bool compute_optimal_wipe_tower_min(const Polygons &inflated_parts,
                                    const Vec2d    &plate_center,
                                    const Vec2d    &default_tower_center,
                                    const Vec2d    &tower_size,
                                    double          tower_clearance,
                                    Vec2d          &out_wt_min);

// Optimal tower position of one plate, in the plate's own frame (the frame wipe_tower_x/y use).
// Returns false when the plate prints no tower, has no objects, or the spot would be out of the bed,
// inside an exclusion zone or off the printable area; the caller then keeps the current position.
// `config` is a full config (project options included). No GUI application state is used.
bool compute_plate_optimal_wipe_tower_pos(PartPlate &plate, int plate_idx, const DynamicPrintConfig &config, Vec2d &out_local_pos);

// Re-seats the tower of every unlocked plate (or just plate `only_plate_idx` when >= 0) next to its
// objects and stores the result in the project config. Returns the number of towers that moved.
int optimize_wipe_towers(PartPlateList &plate_list, int only_plate_idx = -1);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_WipeTowerPlacement_hpp_
