// Snapmaker U1 screen: the four tool heads (type / colour / vendor, official NFC lock, feed state, nozzle temperature,
// extruder map) and the head editor. Filament writes and load / unload go through the host (agent): refused while the
// printer prints, an official spool is only overridden after an explicit tick here AND the host's own confirm dialog,
// and nothing that heats or moves filament happens without a click on a button of this page.
(function (Z) {
    'use strict';

    const { h, icon } = Z;
    const { num } = Z.fmt;

    // Solid colour, sectors (dual / tri / quad) or a smooth ring (gradient) of the head's colours.
    function spoolBg(color, colors, gradient) {
        if (!colors || colors.length < 2) return color || null;
        const n = colors.length;
        if (gradient) return 'conic-gradient(' + colors.concat(colors[0]).join(',') + ')';
        return 'conic-gradient(' + colors.map((c, i) => c + ' ' + (i * 100 / n) + '% ' + ((i + 1) * 100 / n) + '%').join(',') + ')';
    }
    function swatch(color, present, colors, gradient) {
        const bg = spoolBg(color, colors, gradient);
        return h('div.spool' + (present ? '' : '.empty'), { style: present && bg ? 'background:' + bg : null });
    }
    const kindLabel = s => !s.colors || s.colors.length < 2 ? '' : s.gradient ? Z.t('ct_gradient') : [Z.t('ct_solid'), Z.t('ct_solid'), Z.t('ct_dual'), Z.t('ct_tri'), Z.t('ct_quad')][s.colors.length];
    const field = (label, input) => h('label.field', h('span', { text: label }), input);
    const norm = t => String(t || '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();
    const baseType = t => {
        const u = String(t || '').trim().toUpperCase();
        for (const b of ['PETG', 'PET', 'PLA', 'ABS', 'ASA', 'TPU', 'PC', 'PA', 'PVA', 'HIPS', 'PP', 'PPS']) if (u.indexOf(b) === 0) return b;
        return String(t || '').trim();
    };
    const baseName = n => String(n || '').split('@')[0].trim();

    // What the feeder of a head is doing, in words ('' when idle).
    function feedText(f) {
        if (f.error && f.error !== 'ok') return Z.t('u1_feed_error') + ': ' + f.error;
        const s = f.action && f.action !== 'none' ? f.action : f.state;
        if (/^unload/.test(s) && !/(finish|fail)$/.test(s)) return Z.t('u1_feed_unloading');
        if (/^(load|preload)/.test(s) && !/(finish|fail)$/.test(s)) return Z.t('u1_feed_loading');
        if (/^manual/.test(s) && !/(finish|fail)$/.test(s)) return Z.t('u1_feed_manual');
        if (/fail$/.test(s)) return Z.t('u1_feed_error') + ': ' + s;
        return '';
    }

    function createHeadsTab(ctx) {
        const link = ctx.link;
        let sel = 1, lastSig = '';
        const head = h('div.row.gap');
        const grid = h('div.u1-grid');
        const mapBox = h('div.u1-map');
        const note = h('p.note');
        const feedBusy = () => !!link.state.headsFeedBusy;
        const blocked = () => ctx.busy() || !link.online;
        const current = () => (link.heads || []).find(x => x.id === sel);

        const editBtn = h('button.btn', { type: 'button', onclick: () => openHeadEditor(ctx, sel) }, icon('edit'), Z.t('edit_slot'));
        const loadBtn = h('button.btn.primary', { type: 'button', onclick: () => feed('load') }, icon('feed'), Z.t('ifs_load'));
        const unloadBtn = h('button.btn', { type: 'button', onclick: () => feed('unload') }, icon('retract'), Z.t('ifs_unload'));
        const syncAllBtn = h('button.btn', { type: 'button', title: Z.t('u1_sync_all_info'), onclick: () => {
            Z.Host.request('u1_sync_all', {}, 'u1_sync_all', 15000).then(r => {
                if (!r.ok) Z.toast(Z.t('ifs_write_blocked', { reason: r.reason || '' }), 'err');
                else Z.toast(r.changed ? Z.t('ifs_sync_all_done', { what: r.changed }) : Z.t('ifs_sync_all_none'));
            }).catch(() => Z.toast(Z.t('err_send'), 'err'));
        } }, icon('refresh'), Z.t('ifs_sync_all'));
        const el = h('div.cfs-tab', head, grid, h('div.cfs-actions', editBtn, loadBtn, unloadBtn, syncAllBtn), note, mapBox);

        // Load / unload heat the nozzle and move filament: an explicit confirm on this page, then the host's agent.
        function feed(action) {
            const s = current();
            if (!s || blocked() || feedBusy()) return;
            const msg = Z.t(action === 'load' ? 'u1_load_confirm' : 'u1_unload_confirm', { head: s.id });
            Z.confirm(msg, { okLabel: Z.t(action === 'load' ? 'feed' : 'retract') }).then(yes => {
                if (!yes || blocked()) return;
                link.feedHead(s.index, action).then(r => {
                    if (!r.ok) { Z.toast(Z.t('ifs_write_blocked', { reason: r.reason || '' }), 'err'); return; }
                    Z.toast(Z.t('u1_feed_started'));
                    [1500, 5000].forEach(ms => setTimeout(() => link.refreshHeads(), ms));
                }).catch(() => Z.toast(Z.t('err_send'), 'err'));
            });
        }

        function slotEl(s) {
            const feedLine = feedText(s.feed);
            const kind = kindLabel(s);
            const tip = s.present ? [s.material, s.sub, kind, s.vendor].filter(Boolean).join(' · ') : Z.t('slot_empty');
            return h('button.slot.ifs-slot.u1-slot' + (sel === s.id ? '.sel' : '') + (link.state.activeHead === s.index ? '.active' : ''), {
                type: 'button', 'aria-pressed': sel === s.id ? 'true' : 'false', title: tip,
                onclick: () => { sel = s.id; update(true); },
                ondblclick: () => { sel = s.id; update(true); openHeadEditor(ctx, s.id); }
            },
                h('div.lbl', 'T' + s.id, s.official ? h('span.lockico', { title: Z.t('u1_official_locked') }, icon('lock')) : null),
                swatch(s.color, s.present, s.colors, s.gradient),
                h('div.mat', { text: s.present ? [s.material || '?', kind].filter(Boolean).join(' · ') : '—' }),
                h('div.sub', { text: s.present ? [s.sub, s.vendor].filter(Boolean).join(' · ') : Z.t('slot_empty') }),
                h('div.pct', { text: s.target > 0 || s.nozzle > 60 ? Math.round(s.nozzle) + (s.target > 0 ? ' / ' + Math.round(s.target) : '') + ' °C' : '' }),
                h('div.feedline' + (s.feed.error !== 'ok' ? '.err' : ''), { text: feedLine }));
        }

        function update(force) {
            const heads = link.heads;
            const sig = JSON.stringify([heads, link.mapTable, sel, ctx.busy(), link.online, link.state.activeHead, Z.lang]);
            if (!force && sig === lastSig) return;
            lastSig = sig;
            head.replaceChildren();
            grid.replaceChildren();
            mapBox.replaceChildren();
            if (!heads) { grid.appendChild(h('p.muted', { text: link.hasStatus ? Z.t('u1_no_data') : Z.t('cfs_loading') })); return; }
            head.appendChild(h('span.badge.on', { text: 'Snapmaker U1' }));
            heads.forEach(s => grid.appendChild(slotEl(s)));
            const off = blocked() || feedBusy();
            const cur = current();
            editBtn.disabled = off || !cur || !cur.present;
            loadBtn.disabled = off || !cur || !cur.feed.detected || !cur.feed.module;
            unloadBtn.disabled = off || !cur || !(cur.present || cur.feed.detected);
            note.textContent = ctx.busy() ? Z.t('cfs_locked') : feedBusy() ? Z.t('cfs_moving') : (cur && cur.official ? Z.t('u1_official_locked') + ' ' : '') + Z.t('u1_hint');
            if (link.mapTable.length) {
                mapBox.appendChild(h('div.lw-title', { text: Z.t('u1_map') }));
                mapBox.appendChild(h('div.lw-grid', link.mapTable.map((p, i) => h('div.tile', h('small', { text: 'T' + (i + 1) + ' →' }), h('b', { text: 'T' + (num(p) + 1) })))));
                mapBox.appendChild(h('p.muted.small', { text: Z.t('u1_map_info') }));
            }
        }

        return { el: el, update: () => update(false) };
    }

    // ---- head editor: same dialog and styles as the CFS / IFS editors; writes through the host ------------------------------

    const SUBS = ['', 'Silk', 'Matte', 'Wood', 'Marble', 'SnapSpeed', 'Support'];

    function openHeadEditor(ctx, id) {
        const link = ctx.link;
        const s = link.heads && link.heads.find(x => x.id === id);
        if (!s) return;
        if (ctx.busy()) { Z.toast(Z.t('cfs_locked'), 'err'); return; }
        if (!s.present) { Z.toast(Z.t('slot_empty'), 'err'); return; }
        const types = link.validTypes();
        let mode = 'preset', presets = null, chosen = null;

        const presetBtn = h('button', { type: 'button', onclick: () => setMode('preset') }, Z.t('mode_preset'));
        const customBtn = h('button', { type: 'button', onclick: () => setMode('custom') }, Z.t('mode_custom'));
        const colorBtn = h('button', { type: 'button', onclick: () => setMode('colour') }, Z.t('u1_colour_only'));
        const seg = h('div.seg.small', presetBtn, customBtn, colorBtn);

        const search = h('input.input', { type: 'search', placeholder: Z.t('search_filament'), 'aria-label': Z.t('search_filament') });
        const typeSel = h('select.input', h('option', { value: '', text: Z.t('all_types') }));
        const list = h('div.preset-list', { role: 'listbox' }, h('p.muted', { text: Z.t('presets_loading') }));
        const color = h('input', { type: 'color', value: s.color || '#FFFFFF', 'aria-label': Z.t('color') });
        const summary = h('p.muted.small');
        const presetPane = h('div.pane', h('div.row.gap', search, typeSel), list);

        const fType = h('select.input', types.map(t => h('option', { value: t, text: t })));
        const cur = String(s.material).toUpperCase();
        fType.value = types.indexOf(cur) >= 0 ? cur : types[0];
        const fVendor = h('input.input', { type: 'text', maxlength: 40, value: s.vendor || '' });
        const fSub = h('select.input', SUBS.map(t => h('option', { value: t, text: t || 'NONE' })));
        fSub.value = SUBS.indexOf(s.sub) >= 0 ? s.sub : '';
        const customPane = h('div.pane', { hidden: true }, h('div.form-grid', field(Z.t('type'), fType), field(Z.t('vendor'), fVendor), field(Z.t('u1_sub'), fSub)));

        // colour type (as Orca's "Filament colour" dialog): solid / dual / tri / quad / gradient, up to 4 pickers
        const initCols = s.colors && s.colors.length >= 2 ? s.colors : [s.color || '#FFFFFF'];
        const ctSel = h('select.input', ['solid', 'dual', 'tri', 'quad', 'gradient'].map(k => h('option', { value: k, text: Z.t('ct_' + k) })));
        ctSel.value = s.gradient && initCols.length >= 2 ? 'gradient' : ['solid', 'solid', 'dual', 'tri', 'quad'][initCols.length];
        const nSel = h('select.input', [2, 3, 4].map(n => h('option', { value: n, text: String(n) })));
        nSel.value = String(Math.max(2, Math.min(4, initCols.length)));
        const extraCols = [1, 2, 3].map(i => h('input', { type: 'color', value: initCols[i] || initCols[initCols.length - 1] || '#FFFFFF', 'aria-label': Z.t('color') + ' ' + (i + 1) }));
        const colorCount = () => ({ solid: 1, dual: 2, tri: 3, quad: 4, gradient: Number(nSel.value) })[ctSel.value];
        const syncColors = () => {
            const n = colorCount();
            nSel.hidden = ctSel.value !== 'gradient';
            extraCols.forEach((c, i) => { c.hidden = i + 2 > n; });
        };
        ctSel.addEventListener('change', syncColors);
        nSel.addEventListener('change', syncColors);
        syncColors();
        const colorRow = h('div.row.gap', h('label.field.inline', h('span', { text: Z.t('color') }), color), ctSel, nSel, extraCols, summary);

        // an official (NFC) spool is locked by the firmware: overriding it takes an explicit tick, then the host asks once more
        const override = h('input', { type: 'checkbox' });
        const overrideRow = s.official ? h('label.row.gap.u1-override', override, h('span', { text: Z.t('u1_override') })) : null;
        const lockNote = s.official ? h('p.note', { text: Z.t('u1_official_locked') }) : null;

        const body = h('div.editor', lockNote, h('div.row', seg), presetPane, customPane, colorRow, overrideRow);
        Z.dialog({
            title: 'T' + s.id, body: body, wide: true,
            buttons: [{ label: Z.t('cancel') }, { label: Z.t('save'), kind: 'primary', onClick: save }]
        });

        function setMode(m) {
            mode = m;
            presetBtn.classList.toggle('on', m === 'preset');
            customBtn.classList.toggle('on', m === 'custom');
            colorBtn.classList.toggle('on', m === 'colour');
            presetPane.hidden = m !== 'preset';
            customPane.hidden = m !== 'custom';
            summary.textContent = m === 'preset' && chosen ? [chosen.type, chosen.vendor, chosen.minTemp && chosen.maxTemp ? chosen.minTemp + '–' + chosen.maxTemp + ' °C' : ''].filter(Boolean).join(' · ') : '';
        }
        function renderList() {
            const q = norm(search.value), ty = typeSel.value;
            list.replaceChildren();
            const shown = (presets || []).filter(p => (!ty || baseType(p.type) === ty) && (!q || norm(p.name + ' ' + p.vendor).indexOf(q) >= 0));
            if (!shown.length) list.appendChild(h('p.muted', { text: presets ? Z.t('presets_none') : Z.t('presets_loading') }));
            shown.slice(0, 300).forEach(p => list.appendChild(h('button.preset' + (chosen === p ? '.sel' : ''), {
                type: 'button', role: 'option', 'aria-selected': chosen === p ? 'true' : 'false',
                onclick: () => { chosen = p; if (p.color) color.value = p.color; renderList(); setMode('preset'); }
            }, h('span.swatch', { style: p.color ? 'background:' + p.color : null }), h('span.pname', { text: p.name }), h('span.muted', { text: p.type }))));
        }
        search.addEventListener('input', renderList);
        typeSel.addEventListener('change', renderList);
        Z.Host.request('get_filament_presets', {}, 'get_filament_presets', 15000).then(res => {
            presets = (res.presets || []).slice().sort((a, b) => a.name.localeCompare(b.name));
            Array.from(new Set(presets.map(p => baseType(p.type)).filter(Boolean))).sort().forEach(t => typeSel.appendChild(h('option', { value: t, text: t })));
            renderList();
        }).catch(() => { presets = []; renderList(); });
        setMode('preset');

        function write(payload) {
            const n = colorCount();
            const cols = [color.value].concat(extraCols.slice(0, n - 1).map(c => c.value)).map(c => c.toUpperCase());
            Object.assign(payload, { colours: cols, color_type: n < 2 ? 'solid' : ctSel.value === 'gradient' ? 'gradient' : 'multi', force: !!s.official });
            if (ctx.busy() || !link.online) { Z.toast(Z.t('cfs_locked'), 'err'); return false; }
            link.writeHead(Object.assign({ head: s.index }, payload)).then(r => {
                if (!r.ok) { if (r.reason !== 'cancelled') Z.toast(Z.t('ifs_write_blocked', { reason: r.reason || '' }), 'err'); return; }
                Z.toast(Z.t('slot_saved'));
                [1500, 4000, 9000].forEach(ms => setTimeout(() => link.refreshHeads(), ms));   // the worker writes, then the firmware updates
            }).catch(() => Z.toast(Z.t('err_send'), 'err'));
            return true;
        }

        function save() {
            if (s.official && !override.checked) { Z.toast(Z.t('u1_override_needed'), 'err'); return false; }
            const hex = color.value.replace('#', '').toUpperCase();
            if (mode === 'preset') {
                if (!chosen) { Z.toast(Z.t('pick_preset'), 'err'); return false; }
                return write({ set_type: true, type: chosen.type || 'PLA', hex: hex, vendor: chosen.vendor || '', name: baseName(chosen.name), preset: chosen.name });
            }
            if (mode === 'custom') return write({ set_type: true, type: fType.value, hex: hex, vendor: fVendor.value.trim(), name: fSub.value });
            return write({ set_type: false, type: '', hex: hex, vendor: '', name: '' });
        }
    }

    Z.createU1HeadsTab = createHeadsTab;
})(window.Z = window.Z || {});
