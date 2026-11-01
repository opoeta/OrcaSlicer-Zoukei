#ifndef slic3r_RetractWhenCrossingPerimeters_hpp_
#define slic3r_RetractWhenCrossingPerimeters_hpp_

#include <vector>

#include "../AABBTreeIndirect.hpp"
#include "../AABBTreeLines.hpp"
#include "../Line.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

// Forward declarations.
class ExPolygon;
class Layer;
class Polyline;

class RetractWhenCrossingPerimeters
{
public:
    bool    travel_inside_internal_regions(const Layer &layer, const Polyline &travel);
    // Bambu Studio: a long travel hugging a wall still retracts, it would ooze onto the wall.
    bool    travel_near_perimeters_too_long(const Layer &layer, const Polyline &travel, coord_t wall_distance, coord_t max_near_wall_travel);

private:
    // Last object layer visited, for which a cache of internal islands was created.
    const Layer                        *m_layer = nullptr;
    // Last object layer visited, for which the wall lines were collected (built on first need).
    const Layer                        *m_walls_layer = nullptr;
    AABBTreeLines::LinesDistancer<Line> m_walls;
    // Internal islands only, referencing data owned by m_layer->regions()->surfaces().
    std::vector<const ExPolygon*>       m_internal_islands;
    // Search structure over internal islands.
    using AABBTree = AABBTreeIndirect::Tree<2, coord_t>;
    AABBTree                            m_aabbtree_internal_islands;
};

} // namespace Slic3r

#endif // slic3r_RetractWhenCrossingPerimeters_hpp_
