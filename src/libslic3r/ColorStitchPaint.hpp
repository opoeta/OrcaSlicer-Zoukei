#ifndef slic3r_ColorStitchPaint_hpp_
#define slic3r_ColorStitchPaint_hpp_

// ColorStitch Painter, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// The painter stores, per ModelVolume, a paint slot per triangle (colorstitch_paint_facets, state
// 1..32, 0 = unpainted) and a slot -> SurfaceEffectProfile id table. The recipes themselves (pass
// stacks and payloads) live in SurfaceEffectProfileManager, which the 3MF stores as one project
// level blob. At slice time each painted top surface is cut along the painted footprint and every
// piece is filled with its own recipe through the Sandwich engine (FillSandwich.cpp); the rest of
// the surface prints as before.
//
// Differences from NeotkoCM (kept on purpose, see the WIKI section 6):
//  - 32 slots per volume (the TriangleSelector state range of this fork) instead of 254;
//  - the Penultimate zone (the solid layer under a top surface, the area covered by that top) and the
//    Bottom zone (supported bottom surfaces, painted from below) are printed by the same engine as the
//    Top zone, in a reduced form: see SurfaceZone and FillSandwich.cpp;
//  - SVG stickers (ColorStitchSticker.hpp) are a rough port: a flat colour region per SVG colour;
//  - no MixedFilament Object mode, no preview outside the gizmo.
// Everything here is inert while nothing is painted: no index is built and no surface is split.

#include "ColorStitch.hpp"
#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "TriangleSelector.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Model;
class ModelObject;
class ModelVolume;
class PrintObject;

namespace ColorStitchPaint {

// Slot 0 = unpainted, slots 1..kSlotCount-1 carry a recipe.
constexpr int kSlotCount = int(EnforcerBlockerType::ExtruderMax) + 1;

// 3MF names, the same NeotkoCM writes, so painted projects open in both.
constexpr const char *PaintAttr    = "paint_colormix";
constexpr const char *SlotTableKey = "colormix_slot_to_profile_id";
constexpr const char *LibraryKey   = "colormix_profiles_b64";

// Profile id painted with `slot` on this volume, 0 when the slot is unmapped.
int  profile_for_slot(const ModelVolume &mv, int slot);
// True when the volume has painted facets and at least one mapped slot.
bool volume_has_paint(const ModelVolume &mv);
bool object_has_paint(const ModelObject &mo);
// Content hash of the recipes the volume references (0 without paint). The recipes live outside
// the model, so this is what tells Print::apply that a painted recipe was edited.
uint64_t profiles_fingerprint(const ModelVolume &mv);

// Slot table <-> "0,3,0,7" (index 0 first). An all zero table serializes to "".
std::string      serialize_slot_table(const std::vector<int> &table);
std::vector<int> parse_slot_table(const std::string &csv);

// 3MF load: `mask` holds the per triangle attribute strings of the whole object geometry (empty or
// shorter when the file has no paint); the volume uses [first_triangle, first_triangle + count).
void load_volume_paint(ModelVolume &mv, const std::vector<std::string> &mask, size_t first_triangle, size_t count);

// The recipe library as the 3MF metadata value (base64 JSON), "" when the library is empty.
std::string library_to_metadata();
// Merges a library read from a 3MF into the manager. A loaded recipe keeps its id when the id is
// free or already holds the same recipe; otherwise it gets a new id and the slot tables of `model`
// are remapped accordingly.
void library_from_metadata(Model &model, const std::string &b64);

} // namespace ColorStitchPaint

// Print::apply helper: painted facets, slot tables or the content of a referenced recipe differ
// between the two objects (model parts only). Also refreshes the fingerprint cached on mo_new.
bool model_colorstitch_paint_data_changed(const ModelObject &mo, const ModelObject &mo_new);

// Snapshot of the painted recipes of one PrintObject, built by PrintObject::infill() before the
// fills and read by make_fills (FillSandwich.cpp). Recipes are copied here, so the fill threads
// never touch SurfaceEffectProfileManager.
struct ColorStitchPaintIndex
{
    struct Recipe
    {
        int               profile_id = 0;
        PrintObjectConfig config; // the object config with the recipe payload keys applied
        SurfacePassStack  top;    // the Top zone stack (may be empty: then the Top zone is not printed)
        SurfacePassStack  penu;   // the Penultimate zone stack
        SurfacePassStack  bottom; // the Bottom zone stack

        const SurfacePassStack &stack(SurfaceZone zone) const
        {
            return zone == SurfaceZone::Penultimate ? penu : (zone == SurfaceZone::Bottom ? bottom : top);
        }
    };
    struct Facet
    {
        double  z      = 0.;      // top of an upward facet / bottom of a downward one, slicing frame (mm)
        int     recipe = -1;      // index into recipes
        bool    down   = false;   // facing down (Bottom zone) instead of up (Top and Penultimate zones)
        Polygon poly;             // XY projection, slicing frame, scaled, CCW
    };
    // An SVG sticker resolved at build time: one footprint per recipe (ColorStitchSticker.hpp).
    struct StickerArea
    {
        double     z    = 0.;     // anchor of the sticker in the slicing frame (mm)
        bool       down = false;
        int        recipe = -1;   // index into recipes
        ExPolygons area;          // slicing frame, scaled
    };

    std::vector<Recipe>      recipes;
    std::vector<Facet>       facets;           // sorted by z
    std::vector<StickerArea> sticker_areas;    // topmost sticker first
    size_t                   orphan_facets = 0; // painted with a slot whose recipe is missing

    // Null when the object has no usable paint and no sticker.
    static std::shared_ptr<const ColorStitchPaintIndex> build(const PrintObject &po);

    const Recipe *recipe(int profile_id) const;
    // Footprint of every recipe that prints something in `zone`, whose upward facets top out (Top and
    // Penultimate: facets facing up, z = their top) or whose downward facets start (Bottom: facets facing
    // down, z = their bottom) in [z_lo, z_hi] (slicing frame). Sticker areas come first, topmost sticker
    // first, with the part hidden by the stickers above it removed; then the painted facets.
    std::vector<std::pair<int, ExPolygons>> footprints(double z_lo, double z_hi, SurfaceZone zone = SurfaceZone::Top) const;
};

} // namespace Slic3r

#endif // slic3r_ColorStitchPaint_hpp_
