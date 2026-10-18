// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// Single source of truth for the Z epsilons shared by the Sandwich sub-layers and NeoTower.
//
// Invariants (validated by static_assert below):
//   NOMINAL_LH_MIN  > SUBLAYER_GAP                  (real layers cannot fuse)
//   SUBLAYER_GAP    > Z_EPS_PLAN  > Z_EPS_GROUP     (sub-layer and real layer stay separate)
//   Z_EPS_FUTURE_TC < NOMINAL_LH_MIN
#ifndef slic3r_NeoTowerZ_hpp_
#define slic3r_NeoTowerZ_hpp_

#include <cmath>
#include <cstdint>

namespace Slic3r {
namespace NeoTowerZ {

// Thinnest real layer / thinnest printable Sandwich band (mm).
constexpr float NOMINAL_LH_MIN = 0.04f;
// Distance between a sub-layer's scheduling Z and its parent nominal Z.
constexpr float SUBLAYER_GAP = 2e-4f;
// Tower plan merge epsilon. Must be < SUBLAYER_GAP.
constexpr float Z_EPS_PLAN = 1e-4f;
// Same-z grouping epsilon. Must be < Z_EPS_PLAN.
constexpr float Z_EPS_GROUP = 1e-5f;
// Threshold to tell a future real tool change from a sub-layer one.
constexpr float Z_EPS_FUTURE_TC = 5e-4f;
// Below this offset from the nominal Z a sub-layer event is a pass on the real layer's plane;
// at or above it, it is a separate staircase plane.
constexpr float SAME_PLANE_MAX_OFF = 0.02f;

inline int64_t to_key_um(float z) { return static_cast<int64_t>(std::llround(static_cast<double>(z) * 1000.0)); }
inline int64_t to_nm(float z) { return static_cast<int64_t>(std::llround(static_cast<double>(z) * 1e6)); }

static_assert(NOMINAL_LH_MIN > SUBLAYER_GAP, "Real LH minimum must be larger than sublayer gap");
static_assert(SUBLAYER_GAP > Z_EPS_PLAN, "Sublayer gap must exceed plan epsilon");
static_assert(Z_EPS_PLAN > Z_EPS_GROUP, "Plan epsilon must exceed group epsilon");
static_assert(Z_EPS_FUTURE_TC < NOMINAL_LH_MIN, "Future-TC threshold cannot exceed minimum real LH");
static_assert(Z_EPS_FUTURE_TC > SUBLAYER_GAP, "Future-TC threshold must distinguish sublayer and real gap");
static_assert(SAME_PLANE_MAX_OFF < NOMINAL_LH_MIN, "Same-plane threshold must not absorb staircase planes");
static_assert(SAME_PLANE_MAX_OFF > 5.f * SUBLAYER_GAP, "Same-plane threshold must cover stacked lamina plane offsets");

} // namespace NeoTowerZ
} // namespace Slic3r

#endif // slic3r_NeoTowerZ_hpp_
