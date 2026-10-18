#ifndef slic3r_MultiPassScheduler_hpp_
#define slic3r_MultiPassScheduler_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// Canonical tool-grouping order for Sandwich sub-layers. ONE deterministic ordering function,
// called identically by the tower planner (ToolOrdering / NeoTower) and by the G-code emitter, so
// plan == emission by construction.
//
// It groups sub-layers by tool while respecting per-chain Z stacking: within one chain (one stack
// of passes at the same XY) pass N must print before pass N+1. Across chains (disjoint XY) work of
// the same tool is merged behind a single tool change.
//
// Pure algorithm, header-only, no heavy dependencies.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Slic3r {
namespace MultiPassScheduler {

// ORCA_PB_ISLAND_CHAINS=0 forces the old single-chain-per-layer behaviour at runtime (A/B testing).
inline bool island_chains_enabled()
{
    static const bool v = [] {
        if (const char* e = std::getenv("ORCA_PB_ISLAND_CHAINS"))
            return !(e[0] == '0' && e[1] == '\0');
        return true;
    }();
    return v;
}

// THE chain identity. A chain is a hard sequential dependency (the cap must print on top of its
// own ramp). island_id separates disjoint XY islands of the same object and layer, so they can be
// reordered freely; passing 0 everywhere reproduces the single-chain behaviour.
inline uint64_t make_chain_key(const void* obj, int layer_id, int island_id)
{
    uint64_t h = (uint64_t)(uintptr_t)obj * 1000003ull + (uint64_t)(uint32_t)layer_id;
    if (island_chains_enabled())
        h ^= (uint64_t)(uint32_t)(island_id + 1) * 0x9E3779B97F4A7C15ull;
    return h;
}

// One sub-layer (emission unit) of a printed layer.
struct SublayerKey {
    uint64_t chain_key    = 0;     // identity of the stacking chain, always via make_chain_key()
    int      pass_idx     = 0;     // 0-based position in the chain (pass 0 before pass 1)
    int      tool_id      = 0;     // 0-based physical filament
    double   z_actual     = 0.;    // nozzle Z, deterministic tie-break within a tool group
    // Zone of the surface (0 Top, 1 Penultimate, 2 Bottom): breaks ties at the same Z so the bottom
    // chains of a layer print before the penultimate ones and those before the top ones.
    int      zone         = 0;
    // When true, every CONSECUTIVE same-tool entry of this chain is drained in one shot before
    // the next chain gets a turn (PathBlend ramps: one object's ramp completes before the next).
    bool     atomic_chain = false;
};

// Returns a permutation (indices into `items`) in canonical grouped-by-tool emission order.
// Deterministic: identical `items` + `initial_tool` always yield the same order.
//   1. Build chains by chain_key; within a chain order by pass_idx.
//   2. ready = first not-yet-emitted item of each chain.
//   3. If `current_tool` has ready items keep it (no tool change); else pick the tool
//      maximizing (ready_count, then total demand, then lowest tool id).
//   4. Emit all ready items of the chosen tool, unlock their successors, repeat.
inline std::vector<size_t> order_sublayers_by_tool(const std::vector<SublayerKey>& items, int initial_tool)
{
    const size_t n = items.size();
    std::vector<size_t> order;
    order.reserve(n);
    if (n == 0)
        return order;

    struct Chain {
        std::vector<size_t> idxs;     // item indices, ascending by pass_idx
        size_t              next = 0; // position of the next not-yet-emitted item
    };
    std::vector<Chain> chains;
    {
        std::vector<uint64_t> seen_keys;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t key = items[i].chain_key;
            size_t ci = seen_keys.size();
            for (size_t k = 0; k < seen_keys.size(); ++k)
                if (seen_keys[k] == key) { ci = k; break; }
            if (ci == seen_keys.size()) {
                seen_keys.push_back(key);
                chains.emplace_back();
            }
            chains[ci].idxs.push_back(i);
        }
        for (Chain& c : chains) {
            std::sort(c.idxs.begin(), c.idxs.end(), [&](size_t a, size_t b) {
                if (items[a].pass_idx != items[b].pass_idx) return items[a].pass_idx < items[b].pass_idx;
                if (items[a].z_actual != items[b].z_actual) return items[a].z_actual < items[b].z_actual;
                return a < b;
            });
        }
    }

    auto ready_indices = [&]() {
        std::vector<size_t> r;
        for (const Chain& c : chains)
            if (c.next < c.idxs.size())
                r.push_back(c.idxs[c.next]);
        return r;
    };

    int    current_tool = initial_tool;
    size_t emitted      = 0;
    while (emitted < n) {
        std::vector<size_t> ready = ready_indices();
        if (ready.empty())
            break; // defensive: every chain drains

        bool current_has_ready = false;
        for (size_t ri : ready)
            if (items[ri].tool_id == current_tool) { current_has_ready = true; break; }

        int chosen = current_tool;
        if (!current_has_ready) {
            size_t best_ready = 0, best_future = 0;
            int    best_tool  = -1;
            std::vector<int> cand;
            for (size_t ri : ready) {
                const int t = items[ri].tool_id;
                if (std::find(cand.begin(), cand.end(), t) == cand.end())
                    cand.push_back(t);
            }
            std::sort(cand.begin(), cand.end());
            for (int t : cand) {
                size_t rc = 0, fc = 0;
                for (size_t ri : ready)
                    if (items[ri].tool_id == t) ++rc;
                for (size_t i = 0; i < n; ++i)
                    if (items[i].tool_id == t) ++fc;
                if (best_tool < 0 || rc > best_ready ||
                    (rc == best_ready && fc > best_future) ||
                    (rc == best_ready && fc == best_future && t < best_tool)) {
                    best_ready = rc; best_future = fc; best_tool = t;
                }
            }
            chosen = best_tool;
        }

        std::vector<size_t> batch;
        for (size_t ri : ready)
            if (items[ri].tool_id == chosen)
                batch.push_back(ri);
        std::sort(batch.begin(), batch.end(), [&](size_t a, size_t b) {
            if (items[a].z_actual != items[b].z_actual) return items[a].z_actual < items[b].z_actual;
            if (items[a].zone != items[b].zone) return items[a].zone > items[b].zone;
            if (items[a].chain_key != items[b].chain_key) return items[a].chain_key < items[b].chain_key;
            return a < b;
        });

        for (size_t ri : batch) {
            order.push_back(ri);
            ++emitted;
            for (Chain& c : chains) {
                if (c.next < c.idxs.size() && c.idxs[c.next] == ri) {
                    ++c.next;
                    // Atomic chain drain: keep consuming consecutive same-tool siblings of this chain.
                    while (items[ri].atomic_chain && c.next < c.idxs.size() && items[c.idxs[c.next]].tool_id == chosen) {
                        order.push_back(c.idxs[c.next]);
                        ++emitted;
                        ++c.next;
                    }
                    break;
                }
            }
        }
        current_tool = chosen;
    }

    // Defensive: append any leftovers so the permutation is complete.
    if (order.size() < n) {
        std::vector<char> in_order(n, 0);
        for (size_t idx : order) in_order[idx] = 1;
        for (size_t i = 0; i < n; ++i)
            if (!in_order[i]) order.push_back(i);
    }
    return order;
}

// Windowed variant: greedy windows over ascending z_actual, each window ordered by
// order_sublayers_by_tool with the running tool chained across windows. With a single window it
// degenerates to order_sublayers_by_tool.
inline std::vector<size_t> order_sublayers_by_tool_windowed(const std::vector<SublayerKey>& items, int initial_tool, double window_eps)
{
    std::vector<size_t> out;
    const size_t n = items.size();
    if (n == 0)
        return out;
    out.reserve(n);

    std::vector<size_t> by_z(n);
    for (size_t i = 0; i < n; ++i)
        by_z[i] = i;
    std::stable_sort(by_z.begin(), by_z.end(), [&](size_t a, size_t b) { return items[a].z_actual < items[b].z_actual; });

    int    running = initial_tool;
    size_t i       = 0;
    while (i < n) {
        const double zmax = items[by_z[i]].z_actual + window_eps;
        size_t j = i + 1;
        while (j < n && items[by_z[j]].z_actual <= zmax)
            ++j;
        std::vector<SublayerKey> wnd;
        wnd.reserve(j - i);
        for (size_t k = i; k < j; ++k)
            wnd.push_back(items[by_z[k]]);
        const std::vector<size_t> ord = order_sublayers_by_tool(wnd, running);
        for (size_t oi : ord)
            out.push_back(by_z[i + oi]);
        if (!ord.empty())
            running = wnd[ord.back()].tool_id;
        i = j;
    }
    return out;
}

} // namespace MultiPassScheduler
} // namespace Slic3r

#endif // slic3r_MultiPassScheduler_hpp_
