// Stop markers (pause, colour change, tool change, custom G-code) of the sliced job being printed. The host
// (Orca) remembers the markers of every plate it sent, keyed by file name; this asks for them once per job and
// again every 30 s, so the Preferences switch takes effect without reloading the page.
(function (Z) {
    'use strict';

    const cache = {};       // key -> { at, enabled, list }
    const pending = {};

    function keyOf(name) { return String(name || '').split(/[\/]/).pop().toLowerCase(); }

    // Returns { enabled, list } for the job, or null while unknown. `onChange` runs when an answer arrives.
    function get(name, onChange) {
        const k = keyOf(name);
        if (!k || !Z.Host) return null;
        const e = cache[k];
        if ((!e || Date.now() - e.at > 30000) && !pending[k]) {
            pending[k] = true;
            Z.Host.request('print_markers', { name: name }, 'print_markers', 10000).then(r => {
                const total = Number(r.total) || 0;
                cache[k] = {
                    at: Date.now(), enabled: r.enabled !== false,
                    list: (r.markers || []).map(m => ({ kind: m.k, layer: m.layer, frac: Math.max(0, Math.min(1, Number(m.frac) || 0)), t: Number(m.t) || 0, color: m.color || '', text: m.text || '' })),
                    total: total
                };
            }).catch(() => {
                cache[k] = { at: Date.now(), enabled: true, list: [], total: 0 };
            }).then(() => {
                pending[k] = false;
                if (onChange) onChange();
            });
        }
        return e || null;
    }

    Z.Markers = { get };
})(window.Z = window.Z || {});
