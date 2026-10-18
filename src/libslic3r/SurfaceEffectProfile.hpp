#ifndef slic3r_SurfaceEffectProfile_hpp_
#define slic3r_SurfaceEffectProfile_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// Surface Effect Profiles: a named snapshot of a Sandwich recipe (ColorStitch / PathBlend /
// MultiPass payloads plus the resolved pass stacks). In NeotkoCM the ColorStitch Painter paints
// these profiles onto faces and the 3MF stores the library. The Painter is not part of this port
// yet: the data model, the manager and the JSON round trip are, so the painter batch can build on
// them. The slicer itself reads the region keys (see SurfacePassStack::resolve).
//
// Left out on purpose: the NeotkoCM-only project migrations (move_sandwich_editor_recipes,
// migrate_legacy_colorstitch), which need ModelConfig plumbing of the painter.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
struct SurfacePassStack;

enum class SurfaceEffectKind : uint8_t {
    ColorStitch = 1,
    PathBlend   = 2,
    MultiPass   = 3,
};

// Serialized config key -> serialized value. `present` distinguishes "empty default" from
// "user saved snapshot".
struct SurfaceEffectPayload {
    bool                               present = false;
    std::map<std::string, std::string> kv;
};

struct SurfaceEffectProfile {
    int         id = 0;           // assigned by the manager, 1-based
    std::string name;
    uint32_t    preview_argb = 0; // swatch hint
    // A working colour created on the fly by the painter (garbage collected when unused), as
    // opposed to a deliberately saved palette entry.
    bool        auto_generated = false;

    SurfaceEffectPayload colorstitch;
    SurfaceEffectPayload pathblend;
    SurfaceEffectPayload multipass;

    // The resolved Sandwich, one SurfacePassStack::to_json() blob per zone.
    std::string stack_top_json;
    std::string stack_penu_json;
    std::string stack_bottom_json;
};

class SurfaceEffectProfileManager
{
public:
    static SurfaceEffectProfileManager& get();

    // `add` assigns a new id and returns it.
    int  add(SurfaceEffectProfile profile);
    // Inserts keeping the profile's own id (painted data already references it). Refuses an
    // existing or invalid id.
    bool adopt(SurfaceEffectProfile profile);
    bool remove(int id);
    bool rename(int id, const std::string& new_name);

    const SurfaceEffectProfile*              find(int id) const;
    SurfaceEffectProfile*                    find_mut(int id);
    const std::vector<SurfaceEffectProfile>& list() const { return m_profiles; }
    size_t                                   size() const { return m_profiles.size(); }
    void                                     clear();
    int                                      next_id() const { return m_next_id; }
    void                                     restore(std::vector<SurfaceEffectProfile> profiles, int next_id);

    // Snapshot the given keys of `cfg` (only those present).
    static SurfaceEffectPayload snapshot_keys(const DynamicPrintConfig& cfg, const std::vector<std::string>& keys);
    // Lift the ColorStitch / PathBlend payloads of a resolved sandwich up to the profile level.
    static void payload_from_stacks(const SurfacePassStack& top, const SurfacePassStack& penu, SurfaceEffectProfile& p);
    // Write a payload back into a config (keys unknown to `cfg` are skipped).
    static void restore_keys(DynamicPrintConfig& cfg, const SurfaceEffectPayload& payload);

    // Canonical key lists of each payload kind (the keys this port defines).
    static const std::vector<std::string>& colorstitch_keys();
    static const std::vector<std::string>& multipass_keys();
    static const std::vector<std::string>& pathblend_keys();

    // JSON round trip of the whole library (project-level storage).
    std::string to_json() const;
    bool        from_json(const std::string& text);

private:
    SurfaceEffectProfileManager() = default;
    std::vector<SurfaceEffectProfile> m_profiles;
    int                               m_next_id = 1;
};

} // namespace Slic3r

#endif // slic3r_SurfaceEffectProfile_hpp_
