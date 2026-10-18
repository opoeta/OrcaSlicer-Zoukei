// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: G-code emission of the Sandwich sub-layers (the
// MultiPass sub-layer dispatcher of NeotkoCM's GCode.cpp).
//
// The sub-layers of a print_z are printed after the layer body, in the order ToolOrdering stored in
// LayerTools::sandwich_sublayers (MultiPassScheduler). The wipe tower planned exactly that tool
// sequence (NeoTower planning in Print::_make_wipe_tower), so its tool change results are consumed
// here in order; the tower layer is finished after the last sub-layer.
//
// Each sub-layer is extruded at its own nozzle Z (m_nominal_z); its paths already carry the band
// height and the scaled flow, so no global flow scaling is involved.
#include "../GCode.hpp"

#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Layer.hpp"
#include "../MultiPassSubLayer.hpp"
#include "../Print.hpp"
#include "../ShortestPath.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

std::string GCode::process_sandwich_sublayers(const Print                     &print,
                                              const std::vector<LayerToPrint> &layers,
                                              const LayerTools                &layer_tools,
                                              size_t                           single_object_instance_idx,
                                              coordf_t                         print_z,
                                              bool                             has_wipe_tower)
{
    std::string gcode;
    if (layer_tools.sandwich_sublayers.empty())
        return gcode;

    for (const LayerTools::SandwichSublayer &ss : layer_tools.sandwich_sublayers) {
        if (ss.layer == nullptr || ss.index >= ss.layer->sandwich_sublayers.size())
            continue;
        const MultiPassSubLayer &sub = ss.layer->sandwich_sublayers[ss.index];

        // Tool change first and unconditionally: the tower planned it, so it must be consumed even
        // if no instance ends up printing this sub-layer.
        if (has_wipe_tower) {
            if (m_writer.need_toolchange(ss.tool)) {
                gcode += m_wipe_tower->tool_change(*this, int(ss.tool), false);
                m_last_processor_extrusion_role = erWipeTower;
            }
        } else
            gcode += this->set_extruder(ss.tool, print_z);

        if (sub.fills.entities.empty())
            continue;

        for (const LayerToPrint &ltp : layers) {
            if (ltp.object_layer != ss.layer)
                continue;
            const PrintObject *obj = ltp.original_object != nullptr ? ltp.original_object : ltp.object_layer->object();
            if (obj == nullptr)
                continue;
            const size_t i_begin = single_object_instance_idx == size_t(-1) ? 0 : single_object_instance_idx;
            const size_t i_end   = single_object_instance_idx == size_t(-1) ? obj->instances().size() :
                                                                               std::min(obj->instances().size(), single_object_instance_idx + 1);
            for (size_t instance_id = i_begin; instance_id < i_end; ++instance_id) {
                const PrintInstance &inst = obj->instances()[instance_id];

                // Same per-instance preamble as the main instance loop of process_layer.
                m_config.apply(print.default_region_config());
                m_config.apply(obj->config(), true);
                if (sub.region != nullptr)
                    m_config.apply(sub.region->config());
                m_layer                  = ss.layer;
                m_object_layer_over_raft = false;
                if (m_config.reduce_crossing_wall)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);

                if (this->config().gcode_label_objects)
                    gcode += std::string("; printing object ") + obj->model_object()->name + " id:" + std::to_string(obj->get_id()) +
                             " copy " + std::to_string(inst.id) + "\n";
                if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_start_str(std::string("; start printing object, unique label id: ") +
                                                      std::to_string(inst.model_instance->get_labeled_id()) + "\n" + "M624 " +
                                                      _encode_label_ids_to_base64({ inst.model_instance->get_labeled_id() }) + "\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper)
                            m_writer.set_object_start_str(std::string("EXCLUDE_OBJECT_START NAME=") + instance_name(inst) + "\n");
                        else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware)
                            m_writer.set_object_start_str(std::string("M486 S") + std::to_string(inst.unique_id) + "\n");
                    }
                }

                m_extrusion_quality_estimator.set_current_object(obj);
                const Point &offset = inst.shift;
                std::pair<const PrintObject *, Point> this_object_copy(obj, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                this->on_set_origin(obj, offset);

                // The sub-layer's own nozzle Z. The lazy layer-change flag makes the first travel
                // reach it even when the nozzle already sits on the first point.
                m_nominal_z                = sub.print_z;
                m_need_change_layer_lift_z = true;

                // Extrude a private copy, chained from the current position.
                ExtrusionEntityCollection          chained(sub.fills);
                std::vector<ExtrusionEntity *>     leaves;
                std::function<void(ExtrusionEntity *)> collect = [&leaves, &collect](ExtrusionEntity *e) {
                    if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(e)) {
                        for (ExtrusionEntity *c : coll->entities)
                            collect(c);
                    } else if (e != nullptr)
                        leaves.push_back(e);
                };
                for (ExtrusionEntity *e : chained.entities)
                    collect(e);
                chain_and_reorder_extrusion_entities(leaves, m_last_pos.to_point());
                for (ExtrusionEntity *ee : leaves)
                    gcode += this->extrude_entity(*ee, "infill");

                // Same per-instance footer as the main instance loop.
                if (!m_writer.is_object_start_str_empty()) {
                    m_writer.set_object_start_str("");
                } else if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_end_str(std::string("; stop printing object, unique label id: ") +
                                                    std::to_string(inst.model_instance->get_labeled_id()) + "\n" + "M625\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper)
                            m_writer.set_object_end_str(std::string("EXCLUDE_OBJECT_END NAME=") + instance_name(inst) + "\n");
                        else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware)
                            m_writer.set_object_end_str(std::string("M486 S-1\n"));
                    }
                }
            }
        }
    }

    // The tower layer still holds its finish when no tool change consumed it (all sub-layers in the
    // filament the body ended with): print it now, as the last body extruder would have.
    if (has_wipe_tower && m_wipe_tower->has_pending_tool_change()) {
        const Extruder *cur = m_writer.filament();
        const int       tool = cur != nullptr ? int(cur->id()) : int(layer_tools.sandwich_sublayers.back().tool);
        gcode += m_wipe_tower->tool_change(*this, tool, true);
        m_last_processor_extrusion_role = erWipeTower;
    }

    // Flush a pending object end label and return to the layer Z, like the mixed sub-layer block.
    m_writer.add_object_end_labels(gcode);
    m_nominal_z                = print_z;
    m_need_change_layer_lift_z = true;
    return gcode;
}

} // namespace Slic3r
