// CFS tab: the external spool and every CFS unit with its slots, feed / retract, and the slot
// editor (an Orca filament preset or a custom material). Writing follows the native CFS agent:
// the slot gets the printer library's identity for the material (RFID id, vendor, name), the
// temperatures of the chosen preset and a colour; RFID spools are read-only.
(function (Z) {
    'use strict';

    const { h, icon } = Z;
    const { num, colorFromPrinter } = Z.fmt;
    const SLOT_RFID = 2;          // slot.state of a Creality RFID spool
    const COMMON_TYPES = ['PLA', 'PETG', 'ABS', 'ASA', 'TPU', 'PA', 'PC', 'PVA', 'HIPS', 'PET', 'PP', 'PPS'];

    // ---- library matching (same order of preference as the native agent) -------------------

    function norm(text) {
        return String(text || '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();
    }
    function baseType(type) {
        const t = String(type || '').trim().toUpperCase();
        const bases = ['PETG', 'PET', 'PLA', 'ABS', 'ASA', 'TPU', 'PC', 'PA', 'PVA', 'HIPS', 'PP', 'PPS', 'BVOH'];
        for (const b of bases) if (t.indexOf(b) === 0) return b;
        return String(type || '').trim();
    }
    function presetBaseName(name) { return String(name || '').split('@')[0].trim(); }

    function matchLibrary(library, vendor, type, name) {
        if (!library || !library.length) return null;
        const v = norm(vendor), t = norm(type), n = norm(name), bt = norm(baseType(type));
        const typeOk = (e, strict) => !t || (strict ? norm(e.type) === t : norm(e.type) === t || norm(baseType(e.type)) === bt);
        let e = library.find(x => norm(x.brand) === v && norm(x.type) === t && norm(x.name) === n);
        if (e) return e;
        e = library.find(x => typeOk(x, true) && norm(x.name) === n);
        if (e) return e;
        const nameWords = new Set(n.split(' ').filter(Boolean));
        const scored = [];
        library.forEach(x => {
            if (!typeOk(x, false)) return;
            const xn = norm(x.name);
            if (!xn) return;
            let score = 0;
            if (n.indexOf(xn) >= 0 || xn.indexOf(n) >= 0) score += 20;
            const words = xn.split(' ').filter(w => w && w !== 'generic' && w !== norm(x.type));
            if (words.length && words.every(w => nameWords.has(w))) score += 10;
            if (v && norm(x.brand) === v) score += 5;
            if (score) scored.push({ x: x, score: score, generic: norm(x.brand) === 'generic' });
        });
        if (scored.length) {
            scored.sort((p, q) => q.score - p.score || Number(p.generic) - Number(q.generic) || p.x.name.localeCompare(q.x.name));
            return scored[0].x;
        }
        if (!t) return null;
        return library.find(x => norm(x.brand) === 'generic' && typeOk(x, true)) || library.find(x => norm(x.brand) === 'generic' && typeOk(x, false)) || null;
    }

    // The slot record for an Orca preset: identity from the printer library when it knows the
    // material, numbers from the preset.
    function specFromPreset(preset, color, library) {
        const entry = matchLibrary(library, preset.vendor, preset.type, presetBaseName(preset.name));
        const spec = { color: color, rfid: '', vendor: '', type: '', name: '', minTemp: 0, maxTemp: 0, pressure: 0 };
        if (entry) {
            Object.assign(spec, { rfid: entry.id, vendor: entry.brand, type: entry.type, name: entry.name,
                minTemp: entry.minTemp, maxTemp: entry.maxTemp, pressure: entry.pressure });
        } else {
            spec.vendor = preset.vendor || 'Generic';
            spec.type = preset.type || 'PLA';
            spec.name = presetBaseName(preset.name) || spec.vendor + ' ' + spec.type;
        }
        if (preset.minTemp) spec.minTemp = preset.minTemp;
        if (preset.maxTemp) spec.maxTemp = preset.maxTemp;
        if (preset.pressure) spec.pressure = preset.pressure;
        if (!spec.minTemp || !spec.maxTemp) {                 // never hand the printer a 0..0 range
            const generic = matchLibrary(library, 'Generic', spec.type, 'Generic ' + spec.type);
            if (!spec.minTemp) spec.minTemp = generic ? generic.minTemp : 190;
            if (!spec.maxTemp) spec.maxTemp = generic ? generic.maxTemp : 240;
        }
        return spec;
    }

    // ---- slot data --------------------------------------------------------------------------

    function slotInfo(box, m) {
        const color = colorFromPrinter(m.color);
        const loaded = m.state !== 0 && !!(m.vendor || m.type || m.name || color);
        const external = box.type === 1;
        return {
            boxId: box.id, slotId: m.id, external: external, loaded: loaded, color: color,
            label: external ? Z.t('ext_short') : box.id + 'ABCD'.charAt(m.id),
            type: m.type || '', vendor: m.vendor || '', name: m.name || '', rfid: m.rfid || '',
            locked: m.state === SLOT_RFID, percent: m.percent, active: Number(m.selected) === 1,
            minTemp: num(m.minTemp), maxTemp: num(m.maxTemp), pressure: num(m.pressure)
        };
    }

    // ---- the tab ----------------------------------------------------------------------------

    function createCfsTab(ctx) {
        const link = ctx.link;
        let sel = null;                 // {box, slot}
        let lastSig = '';
        let poll = null;

        const units = h('div.cfs-units');
        const editBtn = h('button.btn', { type: 'button', onclick: editSelected }, icon('edit'), Z.t('edit_slot'));
        const feedBtn = h('button.btn.primary', { type: 'button', onclick: () => feed(true) }, icon('feed'), Z.t('feed'));
        const retractBtn = h('button.btn', { type: 'button', onclick: () => feed(false) }, icon('retract'), Z.t('retract'));
        const rfidBtn = h('button.btn', { type: 'button', onclick: refreshRfid }, icon('refresh'), Z.t('reread_rfid'));
        const refillBtn = h('button.btn', { type: 'button', onclick: openAutoRefill }, icon('layers'), Z.t('auto_refill'));
        const note = h('p.note');
        const el = h('div.cfs-tab', units, h('div.cfs-actions', editBtn, feedBtn, retractBtn, rfidBtn, refillBtn), note);

        function selectedSlot() {
            if (!sel || !link.boxes) return null;
            const box = (link.boxes.materialBoxs || []).find(b => b.id === sel.box);
            const m = box && (box.materials || []).find(x => x.id === sel.slot);
            return m ? slotInfo(box, m) : null;
        }

        function feed(into) {
            const s = selectedSlot();
            if (!s || ctx.busy()) return;
            link.feed(s.boxId, s.slotId, into);
            setTimeout(() => link.get({ boxsInfo: 1 }), 1500);
        }
        function refreshRfid() {
            const s = selectedSlot();
            if (!s || ctx.busy()) return;
            link.refreshRfid(s.boxId, s.slotId);
            setTimeout(() => link.get({ boxsInfo: 1 }), 1500);
        }
        function editSelected() {
            const s = selectedSlot();
            if (s) openSlotEditor(ctx, s);
        }

        function slotEl(s) {
            const spool = h('div.spool' + (s.loaded ? '' : '.empty'), { style: s.loaded && s.color ? 'background:' + s.color : null });
            const isSel = sel && sel.box === s.boxId && sel.slot === s.slotId;
            const name = [s.vendor && s.vendor.toLowerCase() !== 'generic' ? s.vendor : '', s.name && s.name !== s.type ? s.name : ''].filter(Boolean).join(' ');
            return h('button.slot' + (isSel ? '.sel' : '') + (s.active ? '.active' : ''), {
                type: 'button', 'aria-pressed': isSel ? 'true' : 'false',
                title: s.loaded ? [s.type, s.vendor, s.name].filter(Boolean).join(' · ') : Z.t('slot_empty'),
                onclick: () => { sel = { box: s.boxId, slot: s.slotId }; update(); }
            },
                h('div.lbl', s.label, s.locked ? icon('lock', 'rfid') : null),
                spool,
                h('div.mat', { text: s.loaded ? (s.type || '?') : '—' }),
                h('div.sub', { text: s.loaded ? (name || s.vendor || '') : Z.t('slot_empty') }),
                h('div.pct', { text: s.loaded && num(s.percent, -1) >= 0 ? Math.round(num(s.percent)) + '%' : '' }));
        }

        // Slots that hold the same material and colour take over from each other (1A > 1B > 2B).
        function refillGroups() {
            const same = (link.boxes && link.boxes.same_material) || [];
            return same.filter(g => Array.isArray(g) && Array.isArray(g[2]) && g[2].length >= 2).map(g => ({
                color: colorFromPrinter(g[1]), material: g[3] || '',
                slots: g[2].map(m => m.boxId + String.fromCharCode(65 + num(m.materialId)))
            }));
        }
        function openAutoRefill() {
            const groups = refillGroups();
            const input = h('input', { type: 'checkbox', role: 'switch' });
            input.checked = Number((link.boxConfig || {}).autoRefill) === 1;
            input.addEventListener('change', () => { if (link.online) link.setAutoRefill(input.checked); else input.checked = !input.checked; });
            const body = h('div', h('div.ar-switch', h('label.switch', input, h('span.track')), h('b', { text: Z.t('auto_refill') })),
                h('p.muted', { text: Z.t('auto_refill_info') }),
                groups.length ? groups.map(g => h('div.ar-group', g.slots.reduce((acc, name, i) => {
                    if (i) acc.push(icon('right'));
                    acc.push(h('span.ar-chip', h('i', { style: g.color ? 'background:' + g.color : null }), name));
                    return acc;
                }, []), h('span.ar-mat', { text: g.material }))) : h('p.muted', { text: Z.t('auto_refill_none') }));
            Z.dialog({ title: Z.t('auto_refill'), body: body, buttons: [{ label: Z.t('close') }] });
        }

        function unitEl(box) {
            const slots = (box.materials || []).map(m => slotInfo(box, m));
            const external = box.type === 1;
            const cfg = link.boxConfig || {};
            const tags = [];
            if (!external) {
                if (num(box.humidity, -1) >= 0) tags.push(h('span.tag', icon('drop'), Math.round(num(box.humidity)) + '%'));
                if (num(box.temp, -1) >= 0) tags.push(h('span.tag', icon('temp'), Math.round(num(box.temp)) + '°C'));
            }
            return h('div.unit' + (external ? '.ext' : ''),
                h('div.unit-head', h('b', { text: external ? Z.t('ext_spool') : 'CFS ' + box.id }), h('div.spacer'), tags),
                h('div.slots', slots.map(slotEl)),
                !external && Number(cfg.autoRefill) === 1 ? h('div.unit-foot', h('button.tag.accent.btn-tag', { type: 'button', title: Z.t('auto_refill'), onclick: openAutoRefill }, 'AUTO')) : null);
        }

        function update() {
            const boxes = (link.boxes && link.boxes.materialBoxs) || [];
            const busy = ctx.busy();
            const feeding = ctx.phase().feeding;
            const s = selectedSlot();
            const sig = JSON.stringify([boxes, sel, link.boxConfig, Z.lang]);
            if (sig !== lastSig) {
                lastSig = sig;
                units.replaceChildren();
                // external spool first, then the CFS units
                boxes.filter(b => b.type === 1).concat(boxes.filter(b => b.type !== 1)).forEach(b => units.appendChild(unitEl(b)));
                if (!boxes.length) units.appendChild(h('p.muted', { text: link.online ? Z.t('cfs_loading') : Z.t('cfs_none') }));
            }
            const blocked = busy || feeding || !link.online;
            editBtn.disabled = !s || busy || !link.online;
            feedBtn.disabled = !s || blocked || !s.loaded;
            retractBtn.disabled = !s || blocked || !s.loaded;
            rfidBtn.disabled = !s || blocked || s.external;
            note.textContent = busy ? Z.t('cfs_locked') : feeding ? Z.t('cfs_moving') : !s ? Z.t('cfs_pick') : '';
        }

        return {
            el: el, update: update,
            activate() {
                link.get({ boxsInfo: 1 });
                clearInterval(poll);
                poll = setInterval(() => link.get({ boxsInfo: 1 }), 10000);
            },
            deactivate() { clearInterval(poll); }
        };
    }

    // ---- slot editor ------------------------------------------------------------------------

    function openSlotEditor(ctx, s) {
        const link = ctx.link;
        if (s.locked) {
            Z.dialog({
                title: Z.t('slot') + ' ' + s.label,
                body: h('div', h('p.muted', { text: Z.t('rfid_readonly') }),
                    h('dl.kv', kv(Z.t('type'), s.type), kv(Z.t('vendor'), s.vendor), kv(Z.t('name'), s.name),
                        kv(Z.t('temp_range'), s.minTemp + ' – ' + s.maxTemp + ' °C'))),
                buttons: [{ label: Z.t('close') }]
            });
            return;
        }

        let mode = 'preset';                       // 'preset' | 'custom'
        let presets = null;                        // from Orca
        let chosen = null;
        const libraryNote = h('p.muted.small');
        if (!link.library) link.get({ reqMaterials: 1 });
        const offLib = link.on('library', () => { libraryNote.textContent = ''; });
        libraryNote.textContent = link.library ? '' : Z.t('library_loading');

        // mode switch
        const presetBtn = h('button', { type: 'button', onclick: () => setMode('preset') }, Z.t('mode_preset'));
        const customBtn = h('button', { type: 'button', onclick: () => setMode('custom') }, Z.t('mode_custom'));
        const seg = h('div.seg.small', presetBtn, customBtn);

        // preset picker
        const search = h('input.input', { type: 'search', placeholder: Z.t('search_filament'), 'aria-label': Z.t('search_filament') });
        const typeSel = h('select.input', h('option', { value: '', text: Z.t('all_types') }));
        const list = h('div.preset-list', { role: 'listbox' }, h('p.muted', { text: Z.t('presets_loading') }));
        const color = h('input', { type: 'color', value: s.color || '#FFFFFF', 'aria-label': Z.t('color') });
        const summary = h('p.muted.small');
        const presetPane = h('div.pane', h('div.row.gap', search, typeSel), list);

        // custom form
        const fType = h('input.input', { type: 'text', list: 'zk-types', value: s.type || 'PLA', maxlength: 20 });
        const typeList = h('datalist', { id: 'zk-types' }, COMMON_TYPES.map(t => h('option', { value: t })));
        const fVendor = h('input.input', { type: 'text', value: s.vendor || 'Generic', maxlength: 40 });
        const fName = h('input.input', { type: 'text', value: s.name || '', maxlength: 40 });
        const fMin = h('input.input', { type: 'number', min: 100, max: 450, step: 1, value: s.minTemp || 190 });
        const fMax = h('input.input', { type: 'number', min: 100, max: 450, step: 1, value: s.maxTemp || 240 });
        const fPa = h('input.input', { type: 'number', min: 0, max: 2, step: 0.001, value: s.pressure || 0 });
        const customPane = h('div.pane', { hidden: true }, h('div.form-grid',
            field(Z.t('type'), fType), field(Z.t('vendor'), fVendor), field(Z.t('name'), fName),
            field(Z.t('temp_min'), fMin), field(Z.t('temp_max'), fMax), field(Z.t('pressure_advance'), fPa)), typeList);

        const colorRow = h('div.row.gap', h('label.field.inline', h('span', { text: Z.t('color') }), color), summary);
        const warn = ctx.busy() ? h('div.alert', icon('warn'), h('span', { text: Z.t('cfs_locked') })) : null;

        const body = h('div.editor', warn, h('div.row', seg), libraryNote, presetPane, customPane, colorRow);
        const dlg = Z.dialog({
            title: Z.t('slot') + ' ' + s.label, body: body, wide: true,
            onClose: () => offLib(),
            buttons: [
                { label: Z.t('clear_slot'), kind: 'danger', onClick: clearSlot },
                { label: Z.t('cancel') },
                { label: Z.t('save'), kind: 'primary', onClick: save }
            ]
        });
        const foot = dlg.el.querySelector('.dialog-foot');
        foot.firstChild.style.marginRight = 'auto';

        function setMode(m) {
            mode = m;
            presetBtn.classList.toggle('on', m === 'preset');
            customBtn.classList.toggle('on', m === 'custom');
            presetPane.hidden = m !== 'preset';
            customPane.hidden = m !== 'custom';
            showSummary();
        }

        function showSummary() {
            if (mode === 'preset' && chosen)
                summary.textContent = [chosen.type, chosen.vendor, chosen.minTemp && chosen.maxTemp ? chosen.minTemp + '–' + chosen.maxTemp + ' °C' : ''].filter(Boolean).join(' · ');
            else summary.textContent = '';
        }

        function renderList() {
            const q = norm(search.value), ty = typeSel.value;
            list.replaceChildren();
            const shown = (presets || []).filter(p => (!ty || baseType(p.type) === ty) && (!q || norm(p.name + ' ' + p.vendor).indexOf(q) >= 0));
            if (!shown.length) list.appendChild(h('p.muted', { text: presets ? Z.t('presets_none') : Z.t('presets_loading') }));
            shown.slice(0, 300).forEach(p => {
                const item = h('button.preset' + (chosen === p ? '.sel' : ''), { type: 'button', role: 'option', 'aria-selected': chosen === p ? 'true' : 'false',
                    onclick: () => {
                        chosen = p;
                        if (p.color) color.value = p.color;
                        renderList();
                        showSummary();
                    } },
                    h('span.swatch', { style: p.color ? 'background:' + p.color : null }),
                    h('span.pname', { text: p.name }),
                    h('span.muted', { text: p.type }));
                list.appendChild(item);
            });
        }
        search.addEventListener('input', renderList);
        typeSel.addEventListener('change', renderList);

        Z.Host.request('get_filament_presets', {}, 'get_filament_presets', 15000).then(res => {
            presets = (res.presets || []).slice().sort((a, b) => a.name.localeCompare(b.name));
            const types = Array.from(new Set(presets.map(p => baseType(p.type)).filter(Boolean))).sort();
            types.forEach(t => typeSel.appendChild(h('option', { value: t, text: t })));
            if (s.type && types.indexOf(baseType(s.type)) >= 0) typeSel.value = baseType(s.type);
            renderList();
        }).catch(() => { presets = []; renderList(); });
        setMode('preset');

        function ensureLibrary(ms) {
            if (link.library) return Promise.resolve();
            link.get({ reqMaterials: 1 });
            return new Promise(resolve => {
                const t = setTimeout(() => { off(); resolve(); }, ms);
                const off = link.on('library', () => { clearTimeout(t); off(); resolve(); });
            });
        }

        function buildSpec() {
            if (mode === 'preset') {
                if (!chosen) { Z.toast(Z.t('pick_preset'), 'err'); return null; }
                return specFromPreset(chosen, color.value.toUpperCase(), link.library);
            }
            const type = fType.value.trim(), vendor = fVendor.value.trim() || 'Generic';
            const min = Math.round(num(fMin.value)), max = Math.round(num(fMax.value));
            if (!type) { Z.toast(Z.t('err_type'), 'err'); fType.focus(); return null; }
            if (!(min >= 100 && max <= 450 && min < max)) { Z.toast(Z.t('err_temps'), 'err'); return null; }
            const name = fName.value.trim() || vendor + ' ' + type;
            const entry = matchLibrary(link.library, vendor, type, name);
            return { rfid: entry ? entry.id : '', type: type, vendor: vendor, name: name, color: color.value.toUpperCase(), minTemp: min, maxTemp: max, pressure: num(fPa.value) };
        }

        function verify(spec) {
            // the printer does not always push the change: ask and compare for a few seconds
            let tries = 0;
            const check = () => {
                link.get({ boxsInfo: 1 });
                setTimeout(() => {
                    const b = link.boxes && (link.boxes.materialBoxs || []).find(x => x.id === s.boxId);
                    const m = b && (b.materials || []).find(x => x.id === s.slotId);
                    const ok = spec ? (m && m.type === spec.type && m.name === spec.name && (!spec.color || colorFromPrinter(m.color) === colorFromPrinter(spec.color))) :
                        (m && !m.type && !m.name);
                    if (ok) Z.toast(Z.t('slot_saved'));
                    else if (++tries < 6) check();
                    else Z.toast(Z.t('slot_unconfirmed'), 'err');
                }, 700);
            };
            check();
        }

        function save() {
            if (ctx.busy() || !link.online) { Z.toast(Z.t('cfs_locked'), 'err'); return false; }
            const run = () => {
                const spec = buildSpec();
                if (!spec) return;
                if (!link.modifyMaterial(s.boxId, s.slotId, spec)) { Z.toast(Z.t('err_send'), 'err'); return; }
                verify(spec);
                dlg.close();
            };
            if (mode === 'preset' && !chosen) { Z.toast(Z.t('pick_preset'), 'err'); return false; }
            if (link.library) { run(); return false; }
            ensureLibrary(8000).then(run);          // identity comes from the printer's library
            return false;
        }

        function clearSlot() {
            if (ctx.busy() || !link.online) { Z.toast(Z.t('cfs_locked'), 'err'); return false; }
            Z.confirm(Z.t('clear_confirm', { slot: s.label }), { danger: true, okLabel: Z.t('clear_slot') }).then(yes => {
                if (!yes) return;
                if (link.modifyMaterial(s.boxId, s.slotId, null)) { verify(null); dlg.close(); }
                else Z.toast(Z.t('err_send'), 'err');
            });
            return false;
        }
    }

    function field(label, input) { return h('label.field', h('span', { text: label }), input); }
    function kv(k, v) { return [h('dt', { text: k }), h('dd', { text: v || '—' })]; }

    Z.createCfsTab = createCfsTab;
})(window.Z = window.Z || {});
