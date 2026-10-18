// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef BEADING_STRATEGY_FACTORY_H
#define BEADING_STRATEGY_FACTORY_H

#include <math.h>
#include <cmath>

#include "BeadingStrategy.hpp"
#include "../../Point.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{

class BeadingStrategyFactory
{
public:
    static BeadingStrategyPtr makeStrategy
    (
        coord_t preferred_bead_width_outer = scaled<coord_t>(0.0005),
        coord_t preferred_bead_width_inner = scaled<coord_t>(0.0005),
        coord_t preferred_transition_length = scaled<coord_t>(0.0004),
        float transitioning_angle = M_PI / 4.0,
        bool print_thin_walls = false,
        coord_t min_bead_width = 0,
        coord_t min_feature_size = 0,
        double wall_split_middle_threshold = 0.5,
        double wall_add_middle_threshold = 0.5,
        coord_t max_bead_count = 0,
        coord_t outer_wall_offset = 0,
        int inward_distributed_center_wall_count = 2,
        double minimum_variable_line_width = 0.5,
        // NeoArachne Hybrid v2 (ported from OrcaFS-NeotkoCM, Neotko): wrap the chain (before the Limited
        // strategy) with NeoArachneBeadingStrategy. Defaults keep stock behaviour.
        bool   neoarachne_enabled = false,
        bool   neoarachne_pin_outer = false,
        bool   neoarachne_cap_widening = false,
        double neoarachne_hysteresis_pct = 0.0
    );
};

} // namespace Slic3r::Arachne
#endif // BEADING_STRATEGY_FACTORY_H
