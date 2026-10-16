// Printer screen: camera and current print on the left; temperatures, LED, fans, speed and the
// tabs (CFS, XYZ, files, history) on the right. Every widget is built once and patched by
// update(); inputs the user is touching are never overwritten.
(function (Z) {
    'use strict';

    const { h, icon } = Z;
    const { num } = Z.fmt;

    // ---- small widgets ----------------------------------------------------------------------

    function toggle(onChange) {
        const input = h('input', { type: 'checkbox', role: 'switch' });
        input.addEventListener('change', () => onChange(input.checked));
        return { el: h('label.switch', input, h('span.track')), input: input };
    }

    // Current temperature + editable target + upper limit.
    function tempCard(cfg) {
        const cur = h('span.big', { text: '--' });
        const target = h('input.input.num', { type: 'number', min: 0, step: 1, inputmode: 'numeric', 'aria-label': Z.t('target') });
        const limit = h('span.muted.limit');
        let max = cfg.fallbackMax;
        const commit = () => {
            if (target.value === '') return;
            let v = Math.round(num(target.value));
            v = Math.max(0, Math.min(max, v));
            target.value = v;
            if (!cfg.link.online || !cfg.canWrite()) return;
            cfg.send(v);
        };
        target.addEventListener('change', commit);
        target.addEventListener('keydown', e => { if (e.key === 'Enter') { commit(); target.blur(); } });
        const off = h('button.btn.small.ghost', { type: 'button', onclick: () => { target.value = 0; commit(); } }, Z.t('off'));
        const bar = h('div.mini-bar', h('i'));
        const el = h('div.card.temp', h('h3', icon(cfg.icon || 'temp'), cfg.title),
            h('div.temp-row',
                h('div', h('small', { text: Z.t('current') }), cur),
                h('div', h('small', { text: Z.t('target') }), h('div.input-unit', target, h('span', { text: '°C' })))),
            bar, h('div.row', limit, h('div.spacer'), off));
        return {
            el: el,
            update(s) {
                if (cfg.present && !cfg.present(s)) { el.hidden = true; return; }
                el.hidden = false;
                const c = num(s[cfg.cur]), t = num(s[cfg.tgt]);
                if (cfg.max && num(s[cfg.max]) > 0) max = num(s[cfg.max]);
                target.max = max;
                cur.textContent = Math.round(c) + '°C';
                if (document.activeElement !== target) target.value = Math.round(t);
                limit.textContent = Z.t('max_temp', { v: max });
                el.classList.toggle('heating', t > 0);
                bar.firstChild.style.width = t > 0 ? Math.max(0, Math.min(100, c / t * 100)) + '%' : '0%';
            }
        };
    }

    function fanCard(cfg) {
        let last = 100;                 // value restored when the switch is turned on again
        let dragging = false;
        const label = h('span.pct', { text: '0%' });
        const slider = h('input', { type: 'range', min: 0, max: 100, step: 1, value: 0, 'aria-label': cfg.title });
        const sw = toggle(on => {
            const v = on ? last : 0;
            slider.value = v;
            label.textContent = v + '%';
            cfg.link.setFan(cfg.key, v);
        });
        slider.addEventListener('input', () => { dragging = true; label.textContent = slider.value + '%'; sw.input.checked = Number(slider.value) > 0; });
        slider.addEventListener('change', () => {
            dragging = false;
            const v = Number(slider.value);
            if (v > 0) last = v;
            cfg.link.setFan(cfg.key, v);
        });
        const el = h('div.card.fan', h('h3', icon('fan'), cfg.title, h('div.spacer'), sw.el), h('div.fan-row', slider, label));
        return {
            el: el,
            update(s) {
                if (!(cfg.key in s)) { el.hidden = true; return; }
                el.hidden = false;
                const v = Math.round(num(s[cfg.key]));
                if (v > 0) last = v;
                if (!dragging) { slider.value = v; label.textContent = v + '%'; sw.input.checked = v > 0; }
            }
        };
    }

    // ---- the screen -------------------------------------------------------------------------

    function createDetail() {
        let link = null, camera = null, dev = null;
        let tabs = [], activeTab = 0, pending = false, offs = [];

        const root = h('section.screen.detail');

        function build(device) {
            dev = device;
            link = new Z.PrinterLink(dev);
            camera = new Z.Camera(dev, link);
            const phaseOf = () => Z.derive(link.state);
            const canWrite = () => link.online;       // temperatures etc. are allowed while printing too
            const ctx = { dev: dev, link: link, busy: () => phaseOf().busy, phase: phaseOf };

            // header
            const name = h('h1', { text: dev.name || dev.modelName || dev.model || dev.address });
            const sub = h('span.muted', { text: [dev.modelName || dev.model, dev.address].filter(Boolean).join(' · ') });
            const stateBadge = h('span.badge.wait', { text: Z.t('connecting') });
            const back = h('button.btn', { type: 'button', onclick: () => Z.app.back() }, icon('back'), Z.t('printers'));
            const alertBox = h('div.alert', { hidden: true }, icon('warn'), h('span'));

            // current print
            const thumb = h('img', { alt: '' });
            const thumbBox = h('div.thumb', icon('file'), thumb);
            const fileName = h('div.file', { text: Z.t('no_job') });
            const phaseText = h('span.badge');
            const pct = h('span.pct', { text: '0%' });
            const fill = h('i');
            const meta = h('div.meta');
            const pauseBtn = h('button.btn', { type: 'button', onclick: () => {
                const d = phaseOf();
                if (d.phase === 'paused') link.resume(); else link.pause();
            } }, icon('pause'), Z.t('pause'));
            const stopBtn = h('button.btn.danger', { type: 'button', onclick: () => {
                Z.confirm(Z.t('stop_confirm'), { danger: true, okLabel: Z.t('stop') }).then(yes => { if (yes) link.stop(); });
            } }, icon('stop'), Z.t('stop'));
            const printCard = h('div.card.print',
                h('div.print-head', thumbBox, h('div.print-info', fileName, h('div.row', phaseText, h('div.spacer'), pct))),
                h('div.progress', fill), meta,
                h('div.row.actions', pauseBtn, stopBtn));

            const left = h('aside.left', h('div.card.camera', h('h3', icon('camera'), Z.t('camera')), camera.el), printCard);

            // controls
            const nozzle = tempCard({ title: Z.t('nozzle'), cur: 'nozzleTemp', tgt: 'targetNozzleTemp', max: 'maxNozzleTemp', fallbackMax: 350, link: link, canWrite: canWrite, send: v => link.setNozzleTemp(v) });
            const bed = tempCard({ title: Z.t('bed'), cur: 'bedTemp0', tgt: 'targetBedTemp0', max: 'maxBedTemp', fallbackMax: 120, link: link, canWrite: canWrite, send: v => link.setBedTemp(v) });
            const chamber = tempCard({ title: Z.t('chamber'), cur: 'boxTemp', tgt: 'targetBoxTemp', max: 'maxBoxTemp', fallbackMax: 60, link: link, canWrite: canWrite,
                present: s => 'boxTemp' in s && num(s.maxBoxTemp, 1) > 0, send: v => link.setChamberTemp(v) });

            const ledSw = toggle(on => link.setLight(on));
            const led = { el: h('div.card.led', h('h3', icon('bulb'), Z.t('led'), h('div.spacer'), ledSw.el), h('div.led-state', icon('bulb'))),
                update(s) { this.el.hidden = !('lightSw' in s); ledSw.input.checked = Number(s.lightSw) === 1; this.el.classList.toggle('on', Number(s.lightSw) === 1); } };

            const fans = [
                fanCard({ title: Z.t('fan_model'), key: 'modelFanPct', link: link }),
                fanCard({ title: Z.t('fan_case'), key: 'caseFanPct', link: link }),
                fanCard({ title: Z.t('fan_aux'), key: 'auxiliaryFanPct', link: link })
            ];

            // speed
            const speedModes = [['silent', Z.t('speed_silent')], [50, Z.t('speed_stable') + ' 50%'], [100, Z.t('speed_standard') + ' 100%'], [125, Z.t('speed_ultra') + ' 125%']];
            const speedBtns = speedModes.map(m => h('button', { type: 'button', 'data-mode': m[0], onclick: () => link.setSpeed(m[0]) }, m[1]));
            const speed = { el: h('div.card.speed', h('h3', Z.t('speed')), h('div.seg', speedBtns)),
                update(s) {
                    const cur = Number(s.speedMode) === 1 ? 'silent' : Math.round(num(s.curFeedratePct, 100));
                    speedBtns.forEach((b, i) => b.classList.toggle('on', String(speedModes[i][0]) === String(cur)));
                } };

            // tabs
            tabs = [
                { key: 'cfs', label: Z.t('tab_cfs'), page: Z.createCfsTab(ctx) },
                { key: 'move', label: Z.t('tab_move'), page: Z.createMoveTab(ctx) },
                { key: 'files', label: Z.t('tab_files'), page: Z.createFilesTab(ctx) },
                { key: 'history', label: Z.t('tab_history'), page: Z.createHistoryTab(ctx) }
            ];
            const tabButtons = tabs.map((t, i) => h('button.tab', { type: 'button', role: 'tab', onclick: () => selectTab(i) }, t.label));
            const panes = tabs.map(t => h('div.tab-pane', { role: 'tabpanel', hidden: true }, t.page.el));
            function selectTab(i) {
                tabs[activeTab].page.deactivate && tabs[activeTab].page.deactivate();
                activeTab = i;
                tabButtons.forEach((b, k) => { b.classList.toggle('on', k === i); b.setAttribute('aria-selected', k === i); });
                panes.forEach((p, k) => { p.hidden = k !== i; });
                tabs[i].page.activate && tabs[i].page.activate();
                tabs[i].page.update && tabs[i].page.update();
            }
            const tabCard = h('div.card.tabs', h('div.tab-bar', { role: 'tablist' }, tabButtons), panes);

            const right = h('main.right',
                h('div.temps', nozzle.el, bed.el, chamber.el, led.el),
                h('div.fans', fans.map(f => f.el)), speed.el, tabCard);

            root.appendChild(h('header.topbar', back, h('div.title', name, sub), h('div.spacer'), stateBadge));
            root.appendChild(alertBox);
            root.appendChild(h('div.cols', left, right));

            // ---- patching ---------------------------------------------------------------------
            const widgets = [nozzle, bed, chamber, led, speed].concat(fans);
            function update() {
                pending = false;
                const s = link.state;
                const d = Z.derive(s);
                root.classList.toggle('offline', !link.online);
                root.classList.toggle('busy', d.busy);
                widgets.forEach(w => w.update(s));

                stateBadge.className = 'badge ' + (!link.online ? 'off' : d.phase === 'printing' ? 'on' : d.phase === 'paused' || d.phase === 'pausing' ? 'warn' : 'idle');
                stateBadge.textContent = !link.online ? (link.hasStatus ? Z.t('offline') : Z.t('connecting')) : Z.t('phase_' + d.phase);
                name.textContent = dev.name || s.hostname || dev.modelName || dev.model || dev.address;

                const err = s.err && Number(s.err.errcode) ? s.err : null;
                alertBox.hidden = !err;
                if (err) alertBox.lastChild.textContent = Z.t('printer_error') + ' ' + err.errcode + (err.value ? ': ' + err.value : '');

                const job = Z.fmt.baseName(s.printFileName);
                fileName.textContent = job || Z.t('no_job');
                phaseText.className = 'badge ' + (d.phase === 'printing' ? 'on' : d.phase === 'paused' || d.phase === 'pausing' ? 'warn' : 'idle');
                phaseText.textContent = Z.t('phase_' + d.phase);
                pct.textContent = Math.round(d.progress) + '%';
                fill.style.width = d.progress + '%';
                const left = leftSeconds(s.printLeftTime);
                meta.textContent = '';
                if (d.busy || d.phase === 'done') {
                    [[Z.t('layer'), Number(s.TotalLayer) ? (s.layer || 0) + ' / ' + s.TotalLayer : '--'],
                        [Z.t('elapsed'), Z.fmt.duration(s.printJobTime)],
                        [Z.t('remaining'), d.busy ? Z.fmt.duration(left) : '--']].forEach(kv => meta.appendChild(h('div', h('small', { text: kv[0] }), h('b', { text: kv[1] }))));
                }
                const showThumb = !!job && (d.busy || d.phase === 'done' || d.phase === 'stopped');
                const url = showThumb ? Z.Host.imageUrl(dev, '/downloads/original/current_print_image.png', (s.printStartTime || '') + job) : '';
                if (url && thumb.getAttribute('src') !== url) { thumb.hidden = false; thumb.setAttribute('src', url); }
                if (!url) { thumb.removeAttribute('src'); thumb.hidden = true; }
                thumb.onerror = () => { thumb.hidden = true; };
                thumb.onload = () => { thumb.hidden = false; };
                pauseBtn.disabled = !link.online || !(d.phase === 'printing' || d.phase === 'paused');
                pauseBtn.replaceChildren(icon(d.phase === 'paused' ? 'play' : 'pause'), d.phase === 'paused' ? Z.t('resume') : Z.t('pause'));
                stopBtn.disabled = !link.online || !d.busy;

                tabs[activeTab].page.update && tabs[activeTab].page.update();
            }
            const schedule = () => { if (!pending) { pending = true; requestAnimationFrame(update); } };
            offs = [link.on('state', schedule), link.on('boxes', schedule), link.on('online', schedule), link.on('files', schedule),
                link.on('history', schedule), link.on('library', schedule), link.on('boxconfig', schedule), link.on('ready', schedule),
                link.on('online', on => { if (on) camera.start(); })];
            selectTab(0);
            update();
            link.connect();
        }

        // printLeftTime is seconds on current firmware; some printers send minutes as text ("12m")
        function leftSeconds(v) {
            if (typeof v === 'string' && /m$/i.test(v)) return num(v) * 60;
            return num(v);
        }

        return {
            el: root,
            open(device) {
                this.close();
                build(device);
            },
            close() {
                offs.forEach(off => off());
                offs = [];
                tabs.forEach(t => t.page.deactivate && t.page.deactivate());
                if (camera) camera.stop();
                if (link) link.close();
                link = camera = dev = null;
                tabs = [];
                activeTab = 0;
                while (root.firstChild) root.removeChild(root.firstChild);
            },
            isOpen() { return !!link; },
            suspend() { if (camera) camera.stop(); },
            resume() { if (camera && link && link.online) camera.start(); }
        };
    }

    Z.createDetail = createDetail;
})(window.Z = window.Z || {});
