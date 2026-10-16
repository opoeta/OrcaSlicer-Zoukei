// Screens that differ for a Moonraker printer: the IFS (4 slots as 2x2 like the printer), G-code files with
// the slicer's thumbnails, print history, timelapse videos and the MJPEG camera.
(function (Z) {
    'use strict';

    const { h, icon } = Z;
    const { num } = Z.fmt;

    function swatch(color, present) {
        return h('div.spool' + (present ? '' : '.empty'), { style: present && color ? 'background:' + color : null });
    }

    // ---- IFS ------------------------------------------------------------------------------------------

    const UI_NAMES = { helix: 'ZMOD + HelixScreen', guppy: 'ZMOD + GuppyScreen', original: 'ZMOD + ' };

    function createIfsTab(ctx) {
        const link = ctx.link;
        let sel = 0, lastSig = '';
        const head = h('div.row.gap');
        const grid = h('div.ifs-grid');
        const lw = h('div.lw');
        const loadBtn = h('button.btn.primary', { type: 'button', onclick: () => {
            if (ctx.busy()) return;
            Z.confirm(Z.t('ifs_load_confirm', { slot: sel }), { okLabel: Z.t('feed') }).then(yes => { if (yes) link.ifsLoad(sel); });
        } }, icon('feed'), Z.t('ifs_load'));
        const unloadBtn = h('button.btn', { type: 'button', onclick: () => {
            if (ctx.busy()) return;
            Z.confirm(Z.t('ifs_unload_confirm'), { okLabel: Z.t('retract') }).then(yes => { if (yes) link.ifsUnload(); });
        } }, icon('retract'), Z.t('ifs_unload'));
        const unlockBtn = h('button.btn', { type: 'button', onclick: () => { if (!ctx.busy()) link.ifsUnlock(); } }, icon('lock'), Z.t('ifs_unlock'));
        const editBtn = h('button.btn', { type: 'button', onclick: () => openSlotEditor(ctx, sel) }, icon('edit'), Z.t('edit_slot'));
        const note = h('p.note');
        const el = h('div.cfs-tab', head, grid, h('div.cfs-actions', editBtn, loadBtn, unloadBtn, unlockBtn), note, lw);

        function uiLabel(ifs) {
            if (!ifs) return '';
            return ifs.ui === 'original' ? Z.t('ifs_ui_original') : UI_NAMES[ifs.ui];
        }

        function slotEl(s) {
            const lwTool = link.lessWaste && Array.isArray(link.lessWaste.tools) ? link.lessWaste.tools.indexOf(s.id) : -1;
            const active = link.lessWaste && link.lessWaste.currentTool >= 0 && link.lessWaste.tools && link.lessWaste.tools[link.lessWaste.currentTool] === s.id;
            return h('button.slot.ifs-slot' + (sel === s.id ? '.sel' : '') + (active ? '.active' : ''), {
                type: 'button', 'aria-pressed': sel === s.id ? 'true' : 'false',
                title: s.present ? [s.material, s.colorName].filter(Boolean).join(' · ') : Z.t('slot_empty'),
                onclick: () => { sel = s.id; update(true); },
                ondblclick: () => { sel = s.id; update(true); openSlotEditor(ctx, s.id); }
            },
                h('div.lbl', 'IFS ' + s.id),
                swatch(s.color, s.present),
                h('div.mat', { text: s.present ? (s.material || '?') : '—' }),
                h('div.sub', { text: s.present ? s.colorName : Z.t('slot_empty') }),
                h('div.pct', { text: lwTool >= 0 && lwTool < 4 && s.present ? 'T' + lwTool : '' }));
        }

        function update(force) {
            const ifs = link.ifs, w = link.lessWaste;
            const sig = JSON.stringify([ifs, w, sel, ctx.busy(), link.online, Z.lang]);
            if (!force && sig === lastSig) return;
            lastSig = sig;
            head.replaceChildren();
            grid.replaceChildren();
            lw.replaceChildren();
            if (!ifs) { grid.appendChild(h('p.muted', { text: link.hasStatus ? Z.t('ifs_none') : Z.t('cfs_loading') })); return; }
            head.appendChild(h('span.badge.on', { text: uiLabel(ifs) }));
            if (w) head.appendChild(h('span.badge.idle', { text: 'lessWaste' }));
            // 2x2 like the printer: slots 1-2 on the first row, 3-4 on the second
            ifs.slots.slice(0, 4).forEach(s => grid.appendChild(slotEl(s)));
            const busy = ctx.busy() || !link.online;
            [editBtn, loadBtn, unloadBtn, unlockBtn].forEach(b => { b.disabled = busy; });
            loadBtn.disabled = busy || !(ifs.slots.find(s => s.id === sel) || {}).present;
            note.textContent = ctx.busy() ? Z.t('cfs_locked') : Z.t('ifs_hint');
            if (w) {
                const purge = [Z.t('lw_off'), Z.t('lw_on'), Z.t('lw_tc'), Z.t('lw_type')][w.startPurge] || '?';
                lw.appendChild(h('div.lw-title', { text: Z.t('lw_title') }));
                lw.appendChild(h('div.lw-grid',
                    [[Z.t('lw_start_purge'), purge], [Z.t('lw_line_purge'), w.linePurge ? Z.t('lw_on') : Z.t('lw_off')],
                        [Z.t('lw_leveling'), w.leveling ? Z.t('lw_on') : Z.t('lw_off')], [Z.t('lw_backup'), w.backup ? Z.t('lw_on') : Z.t('lw_off')],
                        [Z.t('lw_ifs'), w.external ? Z.t('lw_off') : Z.t('lw_on')]].map(kv =>
                        h('div.tile', h('small', { text: kv[0] }), h('b', { text: kv[1] })))));
            }
        }

        return { el: el, update: () => update(false) };
    }

    // ---- slot editor: same dialog and styles as the CFS editor; writes through the host (the agent's CHANGE_ZCOLOR path) ----

    const ZMOD_TYPES = ['PLA', 'PLA-CF', 'SILK', 'TPU', 'ABS', 'PETG', 'PETG-CF', '?'];
    const norm = t => String(t || '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();
    const baseType = t => {
        const u = String(t || '').trim().toUpperCase();
        for (const b of ['PETG', 'PET', 'PLA', 'ABS', 'ASA', 'TPU', 'PC', 'PA', 'PVA', 'HIPS', 'PP', 'PPS']) if (u.indexOf(b) === 0) return b;
        return String(t || '').trim();
    };
    const baseName = n => String(n || '').split('@')[0].trim();
    const field = (label, input) => h('label.field', h('span', { text: label }), input);

    function openSlotEditor(ctx, slotId) {
        const link = ctx.link;
        const s = link.ifs && link.ifs.slots.find(x => x.id === slotId);
        if (!s) return;
        if (ctx.busy()) { Z.toast(Z.t('cfs_locked'), 'err'); return; }
        const helix = link.ifs.ui === 'helix';
        const types = ZMOD_TYPES.filter(t => !link.validTypes().length || link.validTypes().indexOf(t) >= 0);
        let mode = 'preset', presets = null, chosen = null;

        const presetBtn = h('button', { type: 'button', onclick: () => setMode('preset') }, Z.t('mode_preset'));
        const customBtn = h('button', { type: 'button', onclick: () => setMode('custom') }, Z.t('mode_custom'));
        const seg = h('div.seg.small', presetBtn, customBtn);

        const search = h('input.input', { type: 'search', placeholder: Z.t('search_filament'), 'aria-label': Z.t('search_filament') });
        const typeSel = h('select.input', h('option', { value: '', text: Z.t('all_types') }));
        const list = h('div.preset-list', { role: 'listbox' }, h('p.muted', { text: Z.t('presets_loading') }));
        const color = h('input', { type: 'color', value: s.color || '#FFFFFF', 'aria-label': Z.t('color') });
        const summary = h('p.muted.small');
        const presetPane = h('div.pane', h('div.row.gap', search, typeSel), list);

        const fType = h('select.input', types.map(t => h('option', { value: t, text: t })));
        const cur = String(s.material).toUpperCase();
        fType.value = types.indexOf(cur) >= 0 ? cur : types[0];
        const fVendor = h('input.input', { type: 'text', maxlength: 40 });
        const fName = h('input.input', { type: 'text', maxlength: 40 });
        const fNozzle = h('input.input', { type: 'number', min: 100, max: 450, step: 1 });
        const fBed = h('input.input', { type: 'number', min: 0, max: 150, step: 1 });
        const extra = helix ? [field(Z.t('vendor'), fVendor), field(Z.t('name'), fName), field(Z.t('ifs_nozzle_temp'), fNozzle), field(Z.t('ifs_bed_temp'), fBed)] : [];
        const customPane = h('div.pane', { hidden: true }, h('div.form-grid', field(Z.t('type'), fType), extra));
        if (helix) link.api('GET', '/server/database/item?namespace=lane_data&key=lane' + s.id).then(r => {   // the current HelixScreen record
            const v = (r && r.value) || {};
            if (!fVendor.value) fVendor.value = v.vendor || '';
            if (!fName.value) fName.value = v.name || '';
            if (!fNozzle.value && v.nozzle_temp) fNozzle.value = v.nozzle_temp;
            if (!fBed.value && v.bed_temp) fBed.value = v.bed_temp;
        }).catch(() => {});

        const colorRow = h('div.row.gap', h('label.field.inline', h('span', { text: Z.t('color') }), color), summary);
        const body = h('div.editor', h('div.row', seg), presetPane, customPane, colorRow);
        const dlg = Z.dialog({
            title: 'IFS ' + s.id, body: body, wide: true,
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
            if (ctx.busy() || !link.online) { Z.toast(Z.t('cfs_locked'), 'err'); return; }
            Z.Host.request('ifs_write_slot', Object.assign({ slot: s.id }, payload), 'ifs_write_slot', 15000).then(r => {
                if (!r.ok) { Z.toast(Z.t('ifs_write_blocked', { reason: r.reason || '' }), 'err'); return; }
                dlg.close();
                Z.toast(Z.t('slot_saved'));
                [3000, 6000, 10000].forEach(ms => setTimeout(() => link.refreshIfs(), ms));   // the worker writes, then the firmware updates
            }).catch(() => Z.toast(Z.t('err_send'), 'err'));
        }

        function save() {
            const hex = color.value.replace('#', '').toUpperCase();
            if (mode === 'preset') {
                if (!chosen) { Z.toast(Z.t('pick_preset'), 'err'); return false; }
                const nozzle = chosen.minTemp && chosen.maxTemp ? Math.round((chosen.minTemp + chosen.maxTemp) / 2) : 0;
                write({ type: chosen.type || 'PLA', hex: hex, vendor: chosen.vendor || '', name: baseName(chosen.name), nozzle_temp: nozzle, bed_temp: 0 });
            } else {
                write({ type: fType.value, hex: hex, vendor: fVendor.value.trim(), name: fName.value.trim(),
                    nozzle_temp: Math.round(num(fNozzle.value)), bed_temp: Math.round(num(fBed.value)) });
            }
            return false;
        }
        function clearSlot() {
            Z.confirm(Z.t('clear_confirm', { slot: s.id }), { danger: true, okLabel: Z.t('clear_slot') }).then(yes => {
                if (yes) write({ type: '?', hex: '161616', vendor: '', name: '', nozzle_temp: 0, bed_temp: 0 });
            });
            return false;
        }
    }

    // ---- files ----------------------------------------------------------------------------------------

    function createFilesTab(ctx) {
        const link = ctx.link;
        let lastSig = '';
        const search = h('input.input', { type: 'search', placeholder: Z.t('search_files'), 'aria-label': Z.t('search_files'), oninput: () => update(true) });
        const refresh = h('button.btn', { type: 'button', onclick: () => link.loadFiles() }, icon('refresh'), Z.t('refresh'));
        const list = h('div.file-list');
        const el = h('div.files-tab', h('div.row.gap', search, refresh), list);

        function colorsOf(m) {
            const c = Array.isArray(m.filament_colors) ? m.filament_colors : [];
            return c.length > 1 ? h('span.dots', c.slice(0, 4).map(x => h('i.dot', { style: 'background:' + x }))) : null;
        }

        function row(f, locked) {
            const m = f.meta || {};
            const url = link.thumbUrl(m, f.path);
            const img = h('img', { alt: '', loading: 'lazy' });
            if (url) img.setAttribute('src', url);
            img.onerror = () => { img.hidden = true; };
            const bits = [Z.fmt.bytes(f.size)];
            if (num(m.estimated_time) > 0) bits.push(Z.fmt.duration(m.estimated_time));
            if (m.filament_type) { try { bits.push(JSON.parse(m.filament_type).filter((t, i, a) => t && a.indexOf(t) === i).join('/')); } catch (e) { bits.push(String(m.filament_type)); } }
            return h('div.file-row',
                h('div.thumb.small', icon('file'), img),
                h('div.info', h('div.fname', { text: f.name }), h('div.muted', { text: bits.filter(Boolean).join(' · ') }), colorsOf(m)),
                h('button.btn.small.primary', { type: 'button', disabled: locked, onclick: () => {
                    Z.confirm(Z.t('print_confirm', { name: f.name }), { okLabel: Z.t('print') }).then(yes => { if (yes && !ctx.busy()) link.printFile(f.path); });
                } }, icon('play'), Z.t('print')),
                h('button.icon-btn', { type: 'button', disabled: !link.online || locked, 'aria-label': Z.t('delete'), title: Z.t('delete'), onclick: () => {
                    Z.confirm(Z.t('delete_confirm', { name: f.name }), { danger: true, okLabel: Z.t('delete') }).then(yes => {
                        if (!yes) return;
                        link.deleteFile(f.path).then(() => link.loadFiles());
                    });
                } }, icon('trash')));
        }

        function update(force) {
            const files = link.files;
            const busy = ctx.busy() || !link.online;
            const q = search.value.trim().toLowerCase();
            const sig = JSON.stringify([files && files.length, files && files[0] && files[0].modified, busy, q, Z.lang]);
            if (!force && sig === lastSig) return;
            lastSig = sig;
            list.replaceChildren();
            if (!files) { list.appendChild(h('p.muted', { text: Z.t('loading') })); return; }
            const shown = files.filter(f => !q || f.name.toLowerCase().indexOf(q) >= 0).sort((a, b) => b.modified - a.modified);
            if (!shown.length) list.appendChild(h('p.muted', { text: Z.t('files_none') }));
            shown.forEach(f => list.appendChild(row(f, busy)));
        }

        return { el: el, update: () => update(false), activate() { link.loadFiles(); } };
    }

    // ---- history --------------------------------------------------------------------------------------

    function createHistoryTab(ctx) {
        const link = ctx.link;
        let lastSig = '';
        const totals = h('div.totals');
        const list = h('div.file-list');
        const el = h('div.history-tab', totals, list);
        const tile = (label, value) => h('div.tile', h('small', { text: label }), h('b', { text: value }));
        const when = t => { const d = new Date(num(t) * 1000); return isNaN(d) ? '' : d.toLocaleString(); };

        function row(j) {
            const m = j.metadata || {};
            const url = link.thumbUrl(m, j.filename);
            const img = h('img', { alt: '', loading: 'lazy' });
            if (url) img.setAttribute('src', url);
            img.onerror = () => { img.hidden = true; };
            const done = j.status === 'completed';
            return h('div.file-row',
                h('div.thumb.small', icon('file'), img),
                h('div.info', h('div.fname', { text: j.filename || '--' }),
                    h('div.muted', { text: [when(j.start_time), Z.fmt.duration(j.print_duration), (num(j.filament_used) / 1000).toFixed(1) + ' m'].filter(Boolean).join(' · ') })),
                h('span.badge.' + (done ? 'on' : 'idle'), { text: done ? Z.t('hist_done') : Z.t('hist_incomplete') }));
        }

        function update() {
            const hist = link.history;
            const sig = JSON.stringify([hist && hist.jobs.length, hist && hist.jobs[0] && hist.jobs[0].job_id, hist && hist.totals, Z.lang]);
            if (sig === lastSig) return;
            lastSig = sig;
            totals.replaceChildren();
            list.replaceChildren();
            if (!hist) { list.appendChild(h('p.muted', { text: Z.t('loading') })); return; }
            const t = hist.totals;
            if (t) {
                totals.appendChild(tile(Z.t('hist_jobs'), String(t.total_jobs || 0)));
                totals.appendChild(tile(Z.t('hist_time'), Z.fmt.duration(t.total_print_time)));
                totals.appendChild(tile(Z.t('hist_material'), (num(t.total_filament_used) / 1000).toFixed(1) + ' m'));
            }
            if (!hist.jobs.length) list.appendChild(h('p.muted', { text: Z.t('hist_none') }));
            hist.jobs.forEach(j => list.appendChild(row(j)));
        }

        return { el: el, update: update, activate() { link.loadHistory(); } };
    }

    // ---- timelapse ------------------------------------------------------------------------------------

    function createTimelapseTab(ctx) {
        const link = ctx.link;
        let lastSig = '';
        const list = h('div.file-list');
        const refresh = h('button.btn', { type: 'button', onclick: () => link.loadVideos() }, icon('refresh'), Z.t('refresh'));
        const el = h('div.timelapse-tab', h('div.row', refresh), list);

        function play(v) {
            const video = h('video', { controls: true, autoplay: true, src: link.fileUrl('timelapse', v.name), style: 'width:100%;max-height:70vh;background:#000' });
            Z.dialog({ title: v.name, body: video, wide: true, onClose: () => { video.pause(); video.removeAttribute('src'); }, buttons: [{ label: Z.t('close') }] });
        }

        function row(v) {
            return h('div.file-row',
                h('div.thumb.small', icon('camera')),
                h('div.info', h('div.fname', { text: v.name }), h('div.muted', { text: [new Date(v.modified * 1000).toLocaleString(), Z.fmt.bytes(v.size)].join(' · ') })),
                h('button.btn.small.primary', { type: 'button', onclick: () => play(v) }, icon('play'), Z.t('play')),
                h('a.btn.small', { href: link.fileUrl('timelapse', v.name), download: v.name, target: '_blank' }, icon('feed'), Z.t('download')),
                h('button.icon-btn', { type: 'button', disabled: !link.online, 'aria-label': Z.t('delete'), title: Z.t('delete'), onclick: () => {
                    Z.confirm(Z.t('delete_confirm', { name: v.name }), { danger: true, okLabel: Z.t('delete') }).then(yes => {
                        if (yes) link.deleteVideo(v.name).then(() => link.loadVideos());
                    });
                } }, icon('trash')));
        }

        function update() {
            const v = link.videos;
            const sig = JSON.stringify([v && v.map(x => x.name), link.online, Z.lang]);
            if (sig === lastSig) return;
            lastSig = sig;
            list.replaceChildren();
            if (!v) { list.appendChild(h('p.muted', { text: Z.t('loading') })); return; }
            if (!v.length) list.appendChild(h('p.muted', { text: Z.t('video_none') }));
            v.slice().sort((a, b) => b.modified - a.modified).forEach(x => list.appendChild(row(x)));
        }

        return { el: el, update: update, activate() { link.loadVideos(); } };
    }

    // ---- camera: MJPEG stream of the printer's webcam (snapshot polling when the stream fails) -----------

    class MrCamera extends Z.Camera {
        _begin(run) {
            const cam = this.link.webcam();
            this.video.hidden = true;
            this.img.hidden = false;
            if (!cam) { this.status(Z.t('camera_unavailable')); return; }
            this.img.onload = () => { if (run === this.run) this.status(''); };
            this.img.onerror = () => { if (run === this.run) this._snapshots(run, cam); };
            this.img.src = cam.stream + (cam.stream.indexOf('?') >= 0 ? '&' : '?') + '_=' + Date.now();
        }

        _snapshots(run, cam) {
            clearInterval(this._snap);
            let bad = 0;
            this.img.onload = () => { bad = 0; this.status(''); };
            this.img.onerror = () => { if (++bad > 3) { clearInterval(this._snap); this._again(run, Z.t('camera_unavailable')); } };
            const tick = () => { this.img.src = cam.snapshot + (cam.snapshot.indexOf('?') >= 0 ? '&' : '?') + '_=' + Date.now(); };
            this._snap = setInterval(tick, 1000);
            tick();
        }

        stop(keepRun) {
            clearInterval(this._snap);
            super.stop(keepRun);
        }
    }

    Z.createIfsTab = createIfsTab;
    Z.createMrFilesTab = createFilesTab;
    Z.createMrHistoryTab = createHistoryTab;
    Z.createMrTimelapseTab = createTimelapseTab;
    Z.MrCamera = MrCamera;
})(window.Z = window.Z || {});
