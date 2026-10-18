// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: minimal replacement for the NeoDebug channel
// used by the Gravity / InstanceContact modules. Read-only diagnostics: set the environment
// variable ORCA_DEBUG_GRAVITY=1 to log through BOOST_LOG_TRIVIAL(info). Never changes geometry.
#ifndef slic3r_GravityDebug_hpp_
#define slic3r_GravityDebug_hpp_

#include <cstdlib>
#include <string>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GravityDebug {

inline bool enabled()
{
    static const bool v = [] {
        const char *e = ::getenv("ORCA_DEBUG_GRAVITY");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return v;
}

inline void write(const std::string &msg)
{
    BOOST_LOG_TRIVIAL(info) << "[gravity] " << msg;
}

} // namespace GravityDebug
} // namespace Slic3r

#endif // slic3r_GravityDebug_hpp_
