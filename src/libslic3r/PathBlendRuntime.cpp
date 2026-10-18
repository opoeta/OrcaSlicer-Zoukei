// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// PathBlend runtime singletons. One instance per process; the GUI writes, the backend reads.
#include "PathBlendRuntime.hpp"

namespace Slic3r {

PathBlendSchedulerRuntime& PathBlendSchedulerRuntime::mut()
{
    static PathBlendSchedulerRuntime g;
    return g;
}
const PathBlendSchedulerRuntime& PathBlendSchedulerRuntime::get() { return mut(); }

PathBlendDispatcherRuntime& PathBlendDispatcherRuntime::mut()
{
    static PathBlendDispatcherRuntime g;
    return g;
}
const PathBlendDispatcherRuntime& PathBlendDispatcherRuntime::get() { return mut(); }

} // namespace Slic3r
