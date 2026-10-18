// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoTower, the post-slice wipe tower planner.
//
// NeotkoCM's problem statement holds here too: the classic tower is planned layer by layer from
// ToolOrdering and cannot see the tool changes a Sandwich inserts INSIDE a layer (one per pass and
// filament bucket). NeoTower runs after slicing, when every tool change is known, real layers and
// sub-layers alike, and plans the tower from that complete list.
//
// What is ported: the event model (NeoTowerEvent), the pure kernels (purge resolution, Z epsilons,
// slot heights), the canonical sub-layer order (MultiPassScheduler, shared with the G-code
// emitter) and the planning of every sub-layer tool change into the tower.
// What is not: NeotkoCM's own copy of Snapmaker's WipeTower2 with its Local-Z machinery, the
// identity (z, old, new) dispatch, Zigurat taper, purge compaction and variable layer heights.
// Those are built on Snapmaker Orca's Local-Z dithering infrastructure, which this fork does not
// have. Here the sub-layer tool changes are planned into Orca's own tower (WipeTower / WipeTower2)
// in the exact order GCode emits them, so the classic positional consumption stays valid.
//
// Opt-in: NeoTower engages only when an object uses the Sandwich (sub-layers exist). Without
// sub-layers nothing here is called and the tower is exactly the classic one.
#ifndef slic3r_NeoTower_hpp_
#define slic3r_NeoTower_hpp_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "NeoTowerZ.hpp"

namespace Slic3r {

class ToolOrdering;

// One tool change anywhere in the print.
struct NeoTowerEvent
{
    float  z_nominal    = 0.f;   // print_z of the real layer the event belongs to
    float  z_actual     = 0.f;   // nozzle Z when the change happens (sub-layer: inside the layer)
    float  layer_height = 0.f;   // nominal height of the real layer
    size_t old_tool     = 0;     // 0-based filaments
    size_t new_tool     = 0;
    float  wipe_volume  = 0.f;   // mm3 to purge
    bool   is_sublayer  = false; // Sandwich sub-layer tool change
};

namespace NeoTowerPure {

// Quantize z to 1 um and pack it with the tool ids (up to 99 tools).
inline uint64_t make_key(float z_actual, size_t old_tool, size_t new_tool)
{
    const uint64_t z_um = static_cast<uint64_t>(std::llround(z_actual * 1000.f));
    return z_um * 10000ULL + old_tool * 100ULL + new_tool;
}

// One purge rule for every tool change (NeotkoCM s158):
//   body change                    -> physical (flush matrix)
//   sandwich sub-layer, same tool  -> prime_floor
//   sandwich sub-layer, new colour -> max(prime_floor, physical)
// prime_floor doubles as the fallback when the matrix is empty or out of range.
inline float resolve_wipe_volume(const std::vector<std::vector<float>> &wipe_volumes, int old_tool, int new_tool, bool sandwich_ctx,
                                 float prime_floor)
{
    float physical = prime_floor;
    if (old_tool >= 0 && size_t(old_tool) < wipe_volumes.size() && new_tool >= 0 && size_t(new_tool) < wipe_volumes[size_t(old_tool)].size())
        physical = wipe_volumes[size_t(old_tool)][size_t(new_tool)];
    if (!sandwich_ctx)
        return physical;
    if (old_tool == new_tool)
        return prime_floor;
    return std::max(prime_floor, physical);
}

// A sub-layer slot gets at most 40% of the nominal layer height, never below 0.04 mm.
inline float sublayer_slot_height(float min_layer_height, float nominal_layer_height)
{
    return std::max(0.04f, std::min(min_layer_height, nominal_layer_height * 0.4f));
}

// Height of a tower plane sitting `z - last_emitting_plane_z` above the previous one: never more
// than the nominal, never below the printable minimum.
inline float eff_layer_height(float z, float nominal_h, float last_emitting_plane_z)
{
    const float delta = z - last_emitting_plane_z;
    if (delta > NeoTowerZ::Z_EPS_PLAN && delta < nominal_h - NeoTowerZ::Z_EPS_PLAN)
        return std::max(delta, NeoTowerZ::NOMINAL_LH_MIN);
    return std::max(nominal_h, NeoTowerZ::NOMINAL_LH_MIN);
}

} // namespace NeoTowerPure

class NeoTower
{
public:
    // True when any layer of the ordering carries Sandwich sub-layers.
    static bool active(const ToolOrdering &ordering);

    // Purge (mm3) of a sub-layer tool change given the purge the classic tower would use for the
    // same filament pair: the Sandwich wipe reserve (multipass_prime_volume) is a floor.
    static float sublayer_wipe_volume(float physical, float prime_floor)
    {
        return std::max(std::max(0.f, prime_floor), std::max(0.f, physical));
    }

    // Every tool change of the print in emission order: per real layer the body changes
    // (LayerTools::extruders) then the Sandwich tail (LayerTools::sandwich_sublayers), with the
    // running tool chained across layers. `wipe_volumes` is the flush matrix (may be empty).
    static std::vector<NeoTowerEvent> collect_events(const ToolOrdering &ordering, const std::vector<std::vector<float>> &wipe_volumes,
                                                     float prime_floor, unsigned int first_tool);
};

} // namespace Slic3r

#endif // slic3r_NeoTower_hpp_
