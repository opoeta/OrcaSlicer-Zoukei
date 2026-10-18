// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0. See NeoTower.hpp.
#include "NeoTower.hpp"

#include "GCode/ToolOrdering.hpp"
#include "Layer.hpp"

namespace Slic3r {

bool NeoTower::active(const ToolOrdering &ordering)
{
    for (const LayerTools &lt : ordering)
        if (!lt.sandwich_sublayers.empty())
            return true;
    return false;
}

std::vector<NeoTowerEvent> NeoTower::collect_events(const ToolOrdering &ordering, const std::vector<std::vector<float>> &wipe_volumes,
                                                    float prime_floor, unsigned int first_tool)
{
    std::vector<NeoTowerEvent> events;
    unsigned int current = first_tool;
    for (const LayerTools &lt : ordering) {
        auto emit = [&](unsigned int tool, float z_actual, bool sub) {
            if (current != (unsigned int) -1 && tool != current) {
                NeoTowerEvent ev;
                ev.z_nominal    = float(lt.print_z);
                ev.z_actual     = z_actual;
                ev.layer_height = float(lt.wipe_tower_layer_height);
                ev.old_tool     = current;
                ev.new_tool     = tool;
                ev.is_sublayer  = sub;
                ev.wipe_volume  = NeoTowerPure::resolve_wipe_volume(wipe_volumes, int(current), int(tool), sub, prime_floor);
                events.push_back(ev);
            }
            current = tool;
        };
        for (unsigned int e : lt.extruders)
            emit(e, float(lt.print_z), false);
        for (const LayerTools::SandwichSublayer &ss : lt.sandwich_sublayers) {
            const float z = (ss.layer != nullptr && ss.index < ss.layer->sandwich_sublayers.size()) ?
                                float(ss.layer->sandwich_sublayers[ss.index].print_z) : float(lt.print_z);
            emit(ss.tool, z, true);
        }
    }
    return events;
}

} // namespace Slic3r
