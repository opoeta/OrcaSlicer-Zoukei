// Orca Zoukei: print markers. Where a sliced job will stop (pause), ask for a new colour (M600), switch tool or
// run custom G-code, with the layer and the time at which it happens. Built from a GCodeProcessorResult
// (custom_gcode_per_print_z + the time estimator) and used by the slice summary timeline, the Bambu status
// panel progress bar and the Creality / Flashforge / Snapmaker device panels (via a small job registry).
#ifndef slic3r_GUI_Zoukei_PrintMarkers_hpp_
#define slic3r_GUI_Zoukei_PrintMarkers_hpp_

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/colour.h>
#include <wx/string.h>

namespace Slic3r {

struct GCodeProcessorResult;

namespace GUI {
namespace Zoukei {
namespace PrintMarkers {

enum class Kind { Pause, ColorChange, ToolChange, Custom };

struct Marker
{
    Kind        kind{Kind::Pause};
    double      print_z{0.};
    int         layer{1};      // 1-based layer number
    float       time_s{0.f};   // print time elapsed when the marker is reached
    float       frac{0.f};     // time_s / total time, 0..1 (layer based when has_time is false)
    int         extruder{0};
    std::string color;         // "#RRGGBB" for colour changes, else empty
    std::string text;          // pause message / custom G-code (first line)
};

struct MarkerSet
{
    std::vector<Marker> markers;
    float               total_time{0.f};
    int                 total_layers{0};
    bool                has_time{false};

    bool empty() const { return markers.empty(); }
};

// Preferences checkbox "zoukei_print_markers" (default on).
bool enabled();

// Markers of a sliced plate (normal time mode). Empty set when there are none.
MarkerSet build(const GCodeProcessorResult& result);

// Job registry, keyed by the file name the printer reports (path and .gcode/.3mf suffixes are ignored).
// Remembered sets are kept in data_dir()/zoukei_print_markers.json (last 24 jobs) so a print that outlives
// the session still shows its markers.
void remember_job(const std::string& job_name, const MarkerSet& set);
void remember_job(const std::string& job_name, const GCodeProcessorResult* result);
bool find_job(const std::string& job_name, MarkerSet& out);

nlohmann::json to_json(const MarkerSet& set);

// Answer of the "print_markers" web command of the device panels: {requestId, ok, enabled, name, total, layers, markers:[...]}.
nlohmann::json web_answer(const std::string& request_id, const std::string& job_name);

// Colours, labels and text.
wxColour colour(const Marker& m);
wxColour outline_colour(bool dark);
wxString kind_label(Kind k);
wxString describe(const Marker& m);                // "Pause - layer 12 - 1h 20m"
wxString format_duration(float seconds);           // "1h 20m"

// The first marker still ahead of progress (0..1, time based). left_s is the printer's remaining time (<= 0 if
// unknown); the returned seconds is the time until that marker (< 0 if it cannot be estimated).
const Marker* next_marker(const MarkerSet& set, double progress, double left_s, double* seconds_until);

} // namespace PrintMarkers
} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_PrintMarkers_hpp_
