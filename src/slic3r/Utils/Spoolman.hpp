// Read-only client of a Spoolman server (https://donkie.github.io/Spoolman/) on the LAN.
// Orca Zoukei never creates or edits anything in Spoolman: only GET /api/v1/... is used.
#ifndef slic3r_Utils_Spoolman_hpp_
#define slic3r_Utils_Spoolman_hpp_

#include <string>
#include <vector>

namespace Slic3r {

class PresetBundle;

namespace Spoolman {

struct Spool
{
    int         id          = 0;
    int         filament_id = 0;
    int         vendor_id   = 0;
    std::string vendor;
    std::string name;     // filament name ("PLA SILK")
    std::string material; // "PLA", "PETG", "Silk PLA", ...
    std::string location;
    std::string comment;
    std::string color;                // "#RRGGBB": the first colour ("" when unknown)
    std::vector<std::string> colors;  // all colours "#RRGGBB" when the filament is multi-colour (>= 2 entries, colors[0] == color)
    bool        gradient    = false;  // multi_color_direction == "longitudinal" (else coaxial = side by side)
    double      remaining_weight = -1.; // g, < 0 = unknown
    double      initial_weight   = -1.;
    double      spool_weight     = -1.;
    int         nozzle_temp = 0;
    int         bed_temp    = 0;
    bool        archived    = false;

    bool is_multi() const { return colors.size() >= 2; }
    // "#12 Voolt3D PLA SILK (PLA)"
    std::string title() const;
};

// "10.10.1.254:7912", "http://host:7912/", "http://host:7912/api/v1" -> "http://10.10.1.254:7912" ("" for an empty input).
std::string normalize_url(const std::string& raw);
// The server configured in the preferences ("spoolman_url"), normalised. UI thread (reads the app config).
std::string configured_url();

// ---- network (blocking; call from a worker thread, never from the UI thread) ----
// GET /api/v1/info
bool fetch_info(const std::string& url, std::string& version, std::string& error);
// GET /api/v1/spool (archived spools are left out). Fills the cache.
bool fetch_spools(const std::string& url, std::vector<Spool>& out, std::string& error);
// GET /api/v1/spool/<id>. Updates the cache entry.
bool fetch_spool(const std::string& url, int id, Spool& out, std::string& error, int timeout = 8);
// GET <moonraker>/server/config -> spoolman.server, normalised ("" when the printer has no [spoolman] section).
std::string discover_from_moonraker(const std::string& moonraker_base_url, const std::string& api_key);

// Last spools fetched (any thread).
bool cached_spool(int id, Spool& out);
void cache_spools(const std::vector<Spool>& spools);

// ---- pure helpers (no I/O) ----
Spool parse_spool(const std::string& json_text, bool* ok = nullptr);
std::vector<Spool> parse_spools(const std::string& json_text, bool* ok = nullptr);
// "FF8000", "#ff8000", "0xFF8000" -> "#FF8000"; "" when it is not a colour.
std::string normalize_colour(const std::string& raw);
// The filament preset that matches the spool best (vendor + material + name, else the generic preset of the material).
// "" when no compatible preset has the spool's material type.
std::string best_preset(const PresetBundle& bundle, const Spool& spool);
// Orca filament_type of the spool ("PLA", "PETG", "PLA-CF", ...; "" unknown); silk is set for silk PLA variants.
std::string orca_type_of(const Spool& spool, bool* silk = nullptr);

} // namespace Spoolman
} // namespace Slic3r

#endif // slic3r_Utils_Spoolman_hpp_
