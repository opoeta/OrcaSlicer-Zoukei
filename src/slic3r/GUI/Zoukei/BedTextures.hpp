// Orca Zoukei: per plate type 3D bed textures (and the holographic flag for them).
//
// A printer can declare one texture per plate type with the printer option "bed_texture_per_type"
// (or, for every variant of a model at once, the same key in the machine_model JSON):
//     "Cool Plate=pei_smooth.svg;Textured PEI Plate=pei_textured.svg;Textured Cool Plate=holographic.svg"
// Plate type names are the BedType names ("Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate",
// "Textured Cool Plate", "Supertack Plate"); "*" or "Default Plate" is the fallback for the types without an entry.
// Files are looked up in the vendor profile folder (user data dir first), then in resources/images. A file whose name
// contains "holo" is a holographic plate: it is drawn with the iridescent printbed_holo shader unless the app setting
// "bed_holographic_effect" (Preferences > Graphics) is off.
// Printers without the key behave exactly as before.
#ifndef slic3r_GUI_Zoukei_BedTextures_hpp_
#define slic3r_GUI_Zoukei_BedTextures_hpp_

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

class GLTexture;

namespace Zoukei {
namespace BedTextures {

struct Entry
{
    std::string path;               // absolute path of the texture file
    bool        holographic = false;
};

// Texture declared for `type` by the edited printer preset; false when none (keep the default bed texture).
// Cheap enough to call every frame (the parsed spec is cached until the printer preset or its spec changes).
bool resolve(BedType type, Entry &out);

// Loads (once) and returns the texture, or nullptr while it is not ready / failed to load. Needs the GL context.
GLTexture *get_texture(const std::string &path);

// App setting "bed_holographic_effect" (default on).
bool holographic_effect_enabled();

// Forgets the parsed specs and the loaded textures (e.g. after the GL context is recreated).
void reset();

// Pure helpers, exposed for tests.
// Parses "Plate=file;Plate=file" into (plate type name, file) pairs; ignores malformed pieces.
bool parse_spec(const std::string &spec, std::vector<std::pair<std::string, std::string>> &out);
// True for a file name that asks for the holographic shader.
bool is_holographic_file(const std::string &file);

} // namespace BedTextures
} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_BedTextures_hpp_
