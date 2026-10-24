// Device console: live activity log of the printer as kept by Orca (websocket changes, sync decisions, agent events,
// errors, klippy.log lines). The host owns the log (Utils/DeviceActivityLog); this window only polls it by sequence number.
// Printer-supplied text only ever goes through textContent.
(function (Z) {
    'use strict';

    const { h, icon } = Z;
    const SOURCES = ['ws-out', 'ws-in', 'sync', 'agent', 'error', 'klipper'];
    const MAX_ROWS = 2000;
    const POLL_MS = 700;

    function pad(n, w) { return String(n).padStart(w || 2, '0'); }
    function clock(ms) {
        const d = new Date(ms);
        return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds()) + '.' + pad(d.getMilliseconds(), 3);
    }

    function openConsole(dev) {
        if (!dev || !Z.Host) return;
        const host = dev.address;
        const state = { since: 0, rows: 0, paused: false, stick: true, q: '', total: 0, timer: 0, klipperTimer: 0, busy: false, closed: false };
        const on = {};
        SOURCES.forEach(s => { on[s] = true; });

        // ---- widgets -----------------------------------------------------------------------
        const list = h('div.con-list', { role: 'log', 'aria-live': 'off', tabindex: '0' });
        const empty = h('div.con-empty.muted', { text: Z.t('con_empty') });
        const status = h('span.muted.con-status');
        const follow = h('button.btn.small.primary.con-follow', { type: 'button', hidden: true, onclick: () => { state.stick = true; list.scrollTop = list.scrollHeight; follow.hidden = true; } }, icon('down'), Z.t('con_follow'));

        const chips = SOURCES.map(s => {
            const cb = h('input', { type: 'checkbox', checked: true });
            cb.addEventListener('change', () => { on[s] = cb.checked; list.classList.toggle('hide-' + s, !cb.checked); updateStatus(); });
            return h('label.con-chip.src-' + s, cb, h('span', { text: s }));
        });
        const search = h('input.input.con-search', { type: 'search', placeholder: Z.t('con_search'), 'aria-label': Z.t('con_search') });
        search.addEventListener('input', () => { state.q = search.value.trim().toLowerCase(); applyTextFilter(); });

        const pauseBtn = h('button.btn.small', { type: 'button', onclick: () => setPaused(!state.paused) }, icon('pause'), Z.t('con_pause'));
        const clearBtn = h('button.btn.small', { type: 'button', onclick: () => {
            Z.Host.send('device_activity_clear', { host: host });
            list.replaceChildren();
            state.rows = 0;
            updateStatus();
        } }, icon('trash'), Z.t('con_clear'));
        const copyBtn = h('button.btn.small', { type: 'button', onclick: () => {
            Z.Host.send('device_activity_copy', { host: host, sources: enabledSources(), q: state.q });
            Z.toast(Z.t('con_copied'));
        } }, icon('copy'), Z.t('con_copy'));
        const exportBtn = h('button.btn.small', { type: 'button', onclick: () => {
            Z.Host.request('device_activity_export', { host: host, sources: enabledSources(), q: state.q }, 'device_activity_export', 120000)
                .then(a => Z.toast(a.ok ? Z.t('con_exported', { path: a.path }) : Z.t('con_export_failed')))
                .catch(() => {});
        } }, icon('file'), Z.t('con_export'));

        const allBox = h('input', { type: 'checkbox' });
        allBox.addEventListener('change', () => Z.Host.send('device_activity_show_all', { on: allBox.checked }));
        const showAll = h('label.con-check', allBox, h('span', { text: Z.t('con_show_all') }));

        const klipBox = h('input', { type: 'checkbox' });
        klipBox.addEventListener('change', () => { klipBox.checked ? startKlipper() : stopKlipper(); });
        const klipLabel = h('label.con-check', { hidden: dev.backend !== 'moonraker' }, klipBox, h('span', { text: Z.t('con_klipper') }));

        const body = h('div.con',
            h('div.con-bar', h('div.con-chips', chips), h('div.spacer'), search),
            h('div.con-bar', pauseBtn, clearBtn, copyBtn, exportBtn, h('div.spacer'), showAll, klipLabel),
            h('div.con-wrap', list, empty, follow),
            h('div.con-foot', status, h('div.spacer'), h('small.muted', { text: Z.t('con_details') + ' ' + Z.t('con_secrets') })));

        const dlg = Z.dialog({ title: Z.t('console_title') + ' - ' + (dev.name || dev.modelName || dev.model || host), body: body, wide: true, onClose: shutdown });
        dlg.el.classList.add('console-dlg');

        // ---- behaviour ---------------------------------------------------------------------
        function enabledSources() { return SOURCES.filter(s => on[s]).join(','); }

        function setPaused(p) {
            state.paused = p;
            pauseBtn.replaceChildren(icon(p ? 'play' : 'pause'), p ? Z.t('con_resume') : Z.t('con_pause'));
            updateStatus();
            if (!p) poll();
        }

        function updateStatus() {
            let shown = 0;
            for (const s of SOURCES) if (on[s]) shown += list.querySelectorAll('.con-row.src-' + s + ':not([hidden])').length;
            status.textContent = Z.t('con_count', { n: shown, total: state.rows }) + (state.paused ? ' - ' + Z.t('con_paused') : '');
            empty.hidden = state.rows > 0;
        }

        function matches(row) { return !state.q || row.dataset.text.indexOf(state.q) >= 0; }
        function applyTextFilter() {
            list.querySelectorAll('.con-row').forEach(r => { r.hidden = !matches(r); });
            updateStatus();
        }

        function addRow(e) {
            const text = h('span.con-text', { text: e.text });
            const row = h('div.con-row.src-' + e.source, { 'data-seq': e.seq },
                h('span.con-time', { text: clock(e.t) }), h('span.con-src', { text: e.source }), text);
            row.dataset.text = String(e.text).toLowerCase();
            if (e.raw) {
                row.classList.add('has-raw');
                let pre = null;
                row.addEventListener('click', () => {
                    if (window.getSelection && String(window.getSelection())) return;      // selecting text, not toggling
                    if (pre) { pre.remove(); pre = null; return; }
                    let txt = e.raw;
                    try { txt = JSON.stringify(JSON.parse(e.raw), null, 2); } catch (err) { /* plain text */ }
                    pre = h('pre.con-raw', { text: txt });
                    row.appendChild(pre);
                });
            }
            row.hidden = !matches(row);
            list.appendChild(row);
            state.rows++;
        }

        function trim() {
            while (list.childElementCount > MAX_ROWS) { list.firstElementChild.remove(); state.rows--; }
        }

        function poll() {
            if (state.closed || state.paused || state.busy) return;
            state.busy = true;
            Z.Host.request('device_activity', { host: host, since: state.since, limit: 500, raw: 1 }, 'device_activity', 4000).then(a => {
                state.busy = false;
                if (state.closed) return;
                const entries = (a && a.entries) || [];
                if (entries.length) {
                    const stick = state.stick;
                    const before = list.scrollTop;
                    entries.forEach(addRow);
                    trim();
                    state.since = Math.max(state.since, a.next || 0, entries[entries.length - 1].seq);
                    list.scrollTop = stick ? list.scrollHeight : before;
                    updateStatus();
                    if (entries.length >= 500) setTimeout(poll, 30);          // backlog: keep draining
                }
                if (a && typeof a.show_all === 'boolean') allBox.checked = a.show_all;
            }).catch(() => { state.busy = false; });
        }

        list.addEventListener('scroll', () => {
            const atEnd = list.scrollHeight - list.scrollTop - list.clientHeight < 24;
            state.stick = atEnd;                                              // scrolling up pauses auto-scroll, reaching the end resumes it
            follow.hidden = atEnd;
        });

        function startKlipper() {
            stopKlipper();
            const tick = () => {
                Z.Host.request('device_klipper_poll', { host: host }, 'device_klipper_poll', 15000).then(a => {
                    if (a && a.error) Z.toast(Z.t('con_klipper_err'));
                }).catch(() => {});
            };
            tick();
            state.klipperTimer = setInterval(tick, 3000);
        }
        function stopKlipper() { if (state.klipperTimer) { clearInterval(state.klipperTimer); state.klipperTimer = 0; } }

        function shutdown() {
            state.closed = true;
            clearInterval(state.timer);
            stopKlipper();
        }

        updateStatus();
        poll();
        state.timer = setInterval(poll, POLL_MS);
    }

    Z.openConsole = openConsole;
})(window.Z = window.Z || {});
