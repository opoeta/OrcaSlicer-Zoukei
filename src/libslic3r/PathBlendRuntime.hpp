#ifndef slic3r_PathBlendRuntime_hpp_
#define slic3r_PathBlendRuntime_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// PathBlend runtime toggles: process-wide singletons read by the Sandwich sub-layer scheduler
// (MultiPassScheduler / NeoTower) and the G-code emitter. They live in libslic3r so the GUI can
// write them without pulling GUI headers into the slicer core.

#include <cstdlib> // std::getenv for the runtime override
#include <string>  // std::stod

namespace Slic3r {

// Scheduler side: when chain_atomic is true, the cross-object scheduler drains every consecutive
// same-tool sub-layer of one PathBlend chain before moving to the next chain, so the ramp of one
// object completes before the ramp of the next starts (no cross-object micro travels).
struct PathBlendSchedulerRuntime {
    bool chain_atomic = true;

    static PathBlendSchedulerRuntime&       mut();
    static const PathBlendSchedulerRuntime& get();
};

// XY threshold (mm) below which two same-tool PathBlend sub-layers count as one chain.
// ORCA_PB_CHAIN_XY overrides it at runtime (NeotkoCM measured 2.0 mm as the safe default).
inline double pb_chain_max_xy_default()
{
    static const double v = [] {
        if (const char* e = std::getenv("ORCA_PB_CHAIN_XY")) {
            try {
                const double d = std::stod(e);
                if (d >= 0.0 && d < 100.0)
                    return d;
            } catch (...) {}
        }
        return 2.0;
    }();
    return v;
}

// Dispatcher side (G-code emitter).
struct PathBlendDispatcherRuntime {
    bool   chain_continuous = true;
    double chain_max_xy_mm  = pb_chain_max_xy_default();

    static PathBlendDispatcherRuntime&       mut();
    static const PathBlendDispatcherRuntime& get();
};

} // namespace Slic3r

#endif // slic3r_PathBlendRuntime_hpp_
