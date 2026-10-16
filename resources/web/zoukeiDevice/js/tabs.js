// The simple tabs of the printer screen: XYZ control, local files and print history.
(function (Z) {
    'use strict';

    const { h, icon } = Z;
    const { num } = Z.fmt;

    // ---- XYZ ----------------------------------------------------------------------------------

    function createMoveTab(ctx) {
        const link = ctx.link;
        let step = 10;
        const pos = h('div.pos');
        const steps = [0.1, 1, 10, 50];
        const stepBtns = steps.map(v => h('button', { type: 'button', onclick: () => { step = v; mark(); } }, v + ' mm'));
        const mark = () => stepBtns.forEach((b, i) => b.classList.toggle('on', steps[i] === step));
        mark();

        const jog = (axis, sign, ico, label) => h('button.btn.jog', { type: 'button', 'aria-label': label, title: label,
            onclick: () => { if (!ctx.busy()) link.move(axis, (sign < 0 ? '-' : '') + step); } }, icon(ico));
        const homeBtn = (label, run) => h('button.btn', { type: 'button', onclick: () => { if (!ctx.busy()) run(); } }, icon('home'), label);

        const controls = h('div.jog-area',
            h('div.pad',
                h('span'), jog('Y', 1, 'up', 'Y+'), h('span'),
                jog('X', -1, 'left', 'X−'), h('span.pad-mid', { text: 'XY' }), jog('X', 1, 'right', 'X+'),
                h('span'), jog('Y', -1, 'down', 'Y−'), h('span')),
            h('div.pad.z',
                jog('Z', 1, 'up', 'Z+'), h('span.pad-mid', { text: 'Z' }), jog('Z', -1, 'down', 'Z−')),
            h('div.col.gap',
                h('div.seg.small', stepBtns),
                h('div.row.gap',
                    homeBtn('XY', () => link.home('X Y')),
                    homeBtn('Z', () => link.home('Z')),
                    homeBtn(Z.t('home_all'), () => link.gcode('G28')))));
        const note = h('p.note');
        const el = h('div.move-tab', pos, controls, note);

        return {
            el: el,
            update() {
                const busy = ctx.busy();
                const p = String(link.state.curPosition || '').replace(/:/g, ' ').replace(/\s+/g, ' ').trim();
                pos.textContent = p ? Z.t('position') + ': ' + p.replace(/ (?=[XYZ] )/g, '   ') : '';
                controls.querySelectorAll('button').forEach(b => { b.disabled = busy || !link.online; });
                // the step selector stays usable even while printing
                stepBtns.forEach(b => { b.disabled = false; });
                note.textContent = busy ? Z.t('move_locked') : '';
            }
        };
    }

    // ---- local files --------------------------------------------------------------------------

    function createFilesTab(ctx) {
        const link = ctx.link, dev = ctx.dev;
        let lastSig = '';
        const search = h('input.input', { type: 'search', placeholder: Z.t('search_files'), 'aria-label': Z.t('search_files'), oninput: () => update(true) });
        const refresh = h('button.btn', { type: 'button', onclick: () => link.get({ reqGcodeFile: 1 }) }, icon('refresh'), Z.t('refresh'));
        const list = h('div.file-list');
        const el = h('div.files-tab', h('div.row.gap', search, refresh), list);

        function row(f, locked) {
            const thumbName = Z.fmt.baseName(f.thumbnail);
            const url = thumbName ? Z.Host.imageUrl(dev, '/downloads/humbnail/' + encodeURIComponent(thumbName), f.create_time) : '';
            const img = h('img', { alt: '', loading: 'lazy' });
            if (url) img.setAttribute('src', url);
            img.onerror = () => { img.hidden = true; };
            const bits = [Z.fmt.bytes(f.file_size)];
            if (num(f.timeCost) > 0) bits.push(Z.fmt.duration(f.timeCost));
            if (f.material) bits.push(String(f.material).split(';').filter((m, i, a) => m && a.indexOf(m) === i).join('/'));
            return h('div.file-row',
                h('div.thumb.small', icon('file'), img),
                h('div.info', h('div.fname', { text: f.name }), h('div.muted', { text: bits.join(' · ') })),
                h('button.btn.small.primary', { type: 'button', disabled: locked, onclick: () => {
                    Z.confirm(Z.t('print_confirm', { name: f.name }), { okLabel: Z.t('print') }).then(yes => { if (yes && !ctx.busy()) link.printFile(f.path); });
                } }, icon('play'), Z.t('print')),
                h('button.icon-btn', { type: 'button', disabled: !link.online, 'aria-label': Z.t('delete'), title: Z.t('delete'), onclick: () => {
                    Z.confirm(Z.t('delete_confirm', { name: f.name }), { danger: true, okLabel: Z.t('delete') }).then(yes => {
                        if (!yes) return;
                        link.deleteFile(f.path);
                        setTimeout(() => link.get({ reqGcodeFile: 1 }), 1200);
                    });
                } }, icon('trash')));
        }

        function update(force) {
            const files = link.files;
            const busy = ctx.busy();
            const q = search.value.trim().toLowerCase();
            const sig = JSON.stringify([files && files.length, files && files[0] && files[0].path, busy, link.online, q, Z.lang]);
            if (!force && sig === lastSig) return;
            lastSig = sig;
            list.replaceChildren();
            if (!files) { list.appendChild(h('p.muted', { text: Z.t('loading') })); return; }
            const shown = files.filter(f => !q || String(f.name).toLowerCase().indexOf(q) >= 0)
                .sort((a, b) => num(b.create_time) - num(a.create_time));
            if (!shown.length) list.appendChild(h('p.muted', { text: Z.t('files_none') }));
            shown.forEach(f => list.appendChild(row(f, busy || !link.online)));
        }

        return { el: el, update: () => update(false), activate() { link.get({ reqGcodeFile: 1 }); } };
    }

    // ---- history ------------------------------------------------------------------------------

    function createHistoryTab(ctx) {
        const link = ctx.link, dev = ctx.dev;
        let lastSig = '';
        const totals = h('div.totals');
        const list = h('div.file-list');
        const el = h('div.history-tab', totals, list);

        function tile(label, value) { return h('div.tile', h('small', { text: label }), h('b', { text: value })); }

        function row(r) {
            const full = String(r.filename || '').split('gcodes/').pop();
            const stem = Z.fmt.baseName(full).replace(/\.gcode[^.]*$/, '');
            const url = Z.Host.imageUrl(dev, '/downloads/humbnail/' + encodeURIComponent(stem + '.png'), r.starttime);
            const img = h('img', { alt: '', loading: 'lazy' });
            if (url) img.setAttribute('src', url);
            img.onerror = () => { img.hidden = true; };
            const done = Number(r.printfinish) === 1;
            return h('div.file-row',
                h('div.thumb.small', icon('file'), img),
                h('div.info', h('div.fname', { text: Z.fmt.baseName(full) || '--' }),
                    h('div.muted', { text: [r.dateTime, Z.fmt.duration(r.usagetime), (num(r.usagematerial) / 1000).toFixed(1) + ' m'].filter(Boolean).join(' · ') })),
                h('span.badge.' + (done ? 'on' : 'idle'), { text: done ? Z.t('hist_done') : Z.t('hist_incomplete') }));
        }

        function update() {
            const hist = link.history;
            const sig = JSON.stringify([hist && hist.historyList && hist.historyList.length, hist && hist.totalJob, Z.lang]);
            if (sig === lastSig) return;
            lastSig = sig;
            totals.replaceChildren();
            list.replaceChildren();
            if (!hist) { list.appendChild(h('p.muted', { text: Z.t('loading') })); return; }
            totals.appendChild(tile(Z.t('hist_jobs'), String(hist.totalJob || 0)));
            totals.appendChild(tile(Z.t('hist_time'), Z.fmt.duration(hist.totalUsageTime)));
            totals.appendChild(tile(Z.t('hist_material'), (num(hist.totalUsageMaterial) / 1000).toFixed(1) + ' m'));
            const rows = hist.historyList || [];
            if (!rows.length) list.appendChild(h('p.muted', { text: Z.t('hist_none') }));
            rows.forEach(r => list.appendChild(row(r)));
        }

        return { el: el, update: update, activate() { link.get({ reqHistory: 1 }); } };
    }

    Z.createMoveTab = createMoveTab;
    Z.createFilesTab = createFilesTab;
    Z.createHistoryTab = createHistoryTab;
})(window.Z = window.Z || {});
