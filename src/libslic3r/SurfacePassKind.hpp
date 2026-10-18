#ifndef slic3r_SurfacePassKind_hpp_
#define slic3r_SurfacePassKind_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// SurfacePassKind - effect kind of one Sandwich sub-layer pass. Kept in a lightweight
// header so consumers that only need the enum (the sub-layer record, NeoTower, the G-code
// emitter) do not pull in ColorStitch.hpp.
//
// The enum is `SurfacePassKind`, not `SurfaceEffectKind`: the latter lives in
// SurfaceEffectProfile.hpp with different members.

namespace Slic3r {

enum class SurfacePassKind : int {
    None        = 0, // passthrough: the natural surface, printed with the object's own filament
    Solid       = 1, // flat colour (a classic MultiPass pass)
    ColorStitch = 2, // per-line filament choice (dither, stripes, weave, custom pattern)
    PathBlend   = 3, // geometric gradient: a Z ramp in one filament, capped by another
    // NeotkoCM reserves 4 for ZBump (top-surface height map). Not ported; the value stays reserved
    // so stacks saved by NeotkoCM keep their numbering.
    ZBump       = 4,
};

// The zone of a surface a Sandwich stack belongs to (NeotkoCM: Top, Penultimate, Bottom).
//  Top         : the visible top surface (stTop).
//  Penultimate : the solid layer right under a top surface (stSubTop), the area covered by that top.
//  Bottom      : the supported bottom surface (on the bed or on supports); bridges are left alone.
enum class SurfaceZone : int {
    Top         = 0,
    Penultimate = 1,
    Bottom      = 2,
};

} // namespace Slic3r

#endif // slic3r_SurfacePassKind_hpp_
