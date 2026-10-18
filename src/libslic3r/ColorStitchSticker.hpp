#ifndef slic3r_ColorStitchSticker_hpp_
#define slic3r_ColorStitchSticker_hpp_

// SVG stickers of the ColorStitch Painter, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// NeotkoCM marks its stickers "rough"; this is a reduced port of the same idea.
//
// A sticker is not geometry. The SVG becomes a flat mask on a horizontal face (the top of a part, or
// the bottom of one: the face on the bed or the underside of an overhang) and every colour region of
// the drawing carries a painted recipe (SurfaceEffectProfile). At slice time the regions are cut out
// of the top / bottom surfaces exactly like painted facets (ColorStitchPaintIndex), and the topmost
// sticker wins where several overlap. Nothing is added to the model.
//
// Differences from NeotkoCM (kept on purpose):
//  - flat colour regions only: a gradient fill becomes the mean colour of its stops (NeotkoCM prints
//    it as bands or as one ColorStitch ramp); CSS classes are not read, <mask> is ignored, <clipPath>
//    is applied (our nanosvg supports it);
//  - horizontal faces only (top and bottom), no stickers on walls;
//  - no rendered preview outside the painter gizmo.
// The 3MF key and the JSON layout are NeotkoCM's (colormix_stickers_b64), so the pile loads there too.

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "libslic3r.h"

#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;

struct ColorStitchStickerColor
{
    uint32_t rgb        = 0;     // 0xRRGGBB as read from the SVG
    int      profile_id = 0;     // SurfaceEffectProfile id (0 = unassigned: the region is not printed)
    bool     hidden     = false; // eye off: not printed, shows what is under it

    template<class Archive> void serialize(Archive &ar) { ar(rgb, profile_id, hidden); }
    bool operator==(const ColorStitchStickerColor &o) const { return rgb == o.rgb && profile_id == o.profile_id && hidden == o.hidden; }
    bool operator!=(const ColorStitchStickerColor &o) const { return !(*this == o); }
};

struct ColorStitchSticker
{
    std::string name;                                 // display label (the SVG file name)
    std::string svg_data;                             // raw SVG text, the persisted source of truth
    // Sticker-local mm (the SVG plane, z = 0, drawing centred on the origin, +Y up) to the object frame
    // (the frame ModelVolume::get_matrix() maps into). A negative z axis means the sticker faces down.
    Transform3d transform{ Transform3d::Identity() };
    int         profile_id = 0;                       // whole sticker recipe, used when `colors` is empty
    std::vector<ColorStitchStickerColor> colors;      // one per distinct SVG colour
    bool        hidden = false;                       // whole sticker off

    bool faces_down() const { return transform.linear().col(2).z() < 0.0; }

    template<class Archive> void serialize(Archive &ar) { ar(name, svg_data, transform, profile_id, colors, hidden); }
    bool operator==(const ColorStitchSticker &o) const
    {
        return profile_id == o.profile_id && transform.matrix() == o.transform.matrix() && name == o.name && svg_data == o.svg_data &&
               colors == o.colors && hidden == o.hidden;
    }
    bool operator!=(const ColorStitchSticker &o) const { return !(*this == o); }
};

namespace ColorStitchStickerTools {

// 3MF key (NeotkoCM's) and its value codec: base64 of a JSON array, one entry per sticker.
constexpr const char *MetadataKey = "colormix_stickers_b64";
std::string           serialize(const std::vector<ColorStitchSticker> &stickers);
void                  parse(const std::string &b64, std::vector<ColorStitchSticker> &stickers);

// Distinct fill / stroke colours of the drawing (0xRRGGBB), first appearance first. A background (the
// first shape, when it covers the whole drawing and there are other shapes) is dropped, here and in
// color_masks().
std::vector<uint32_t> svg_colors(const std::string &svg);
// Width and height of the drawing in mm (0, 0 when the SVG cannot be read).
Vec2d svg_size_mm(const std::string &svg);

// The visible area of every colour of the sticker in the target frame (scaled coordinates, XY of
// `to_target` applied to the sticker-local mm). Shapes drawn later cover earlier ones. `color_index`
// indexes sticker.colors (the nearest colour of the list), -1 for a sticker without colour list.
struct ColorMask
{
    int        color_index = -1;
    ExPolygons area;
};
std::vector<ColorMask> color_masks(const ColorStitchSticker &sticker, const Transform3d &to_target);

// Nearest entry of `colors` to `rgb` (RGB distance), -1 when the list is empty.
int nearest_color(const std::vector<ColorStitchStickerColor> &colors, uint32_t rgb);

// Content hash of the stickers (geometry and recipe ids), 0 without stickers. Print::apply compares it.
uint64_t fingerprint(const ModelObject &mo);

} // namespace ColorStitchStickerTools

// Print::apply helper: the sticker pile of the two objects differs (order, pose, SVG, recipes).
bool model_colorstitch_sticker_data_changed(const ModelObject &mo, const ModelObject &mo_new);

} // namespace Slic3r

#endif // slic3r_ColorStitchSticker_hpp_
