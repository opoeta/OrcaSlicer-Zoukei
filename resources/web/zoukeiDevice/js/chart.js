// Temperature history: the last ~10 minutes of nozzle, bed and chamber readings, kept in memory
// while a printer screen is open and drawn on a plain canvas in a dialog.
(function (Z) {
    'use strict';

    const { h } = Z;
    const WINDOW_MS = 10 * 60 * 1000;
    const SERIES = [['n', 'nozzle', '#E5484D'], ['b', 'bed', '#3B82F6'], ['c', 'chamber', '#D946EF']];

    function createHistory(link) {
        const data = [];
        let timer = null;
        function sample() {
            const s = link.state;
            if (!link.online || !link.hasStatus) return;
            const now = Date.now();
            data.push({ t: now, n: Z.fmt.num(s.nozzleTemp), b: Z.fmt.num(s.bedTemp0), c: 'boxTemp' in s ? Z.fmt.num(s.boxTemp) : null });
            while (data.length && now - data[0].t > WINDOW_MS) data.shift();
        }
        timer = setInterval(sample, 2000);
        sample();
        return { data: data, stop() { clearInterval(timer); } };
    }

    function draw(canvas, data) {
        const css = getComputedStyle(document.documentElement);
        const muted = css.getPropertyValue('--muted').trim() || '#888', line = css.getPropertyValue('--line').trim() || '#444';
        const dpr = window.devicePixelRatio || 1;
        const W = canvas.clientWidth, H = canvas.clientHeight;
        if (!W || !H) return;
        canvas.width = W * dpr; canvas.height = H * dpr;
        const g = canvas.getContext('2d');
        g.scale(dpr, dpr);
        g.font = '11px sans-serif';
        const L = 36, R = 8, T = 8, B = 20;
        let max = 50;
        data.forEach(p => SERIES.forEach(s => { if (p[s[0]] != null && p[s[0]] > max) max = p[s[0]]; }));
        max = Math.ceil(max / 50) * 50;
        const t1 = data.length ? data[data.length - 1].t : Date.now(), t0 = t1 - WINDOW_MS;
        const x = t => L + (t - t0) / WINDOW_MS * (W - L - R), y = v => T + (1 - v / max) * (H - T - B);
        g.strokeStyle = line; g.fillStyle = muted; g.lineWidth = 1;
        g.textAlign = 'right';
        for (let v = 0; v <= max; v += max / 5) {
            g.beginPath(); g.moveTo(L, y(v) + .5); g.lineTo(W - R, y(v) + .5); g.stroke();
            g.fillText(Math.round(v) + '°', L - 4, y(v) + 4);
        }
        g.textAlign = 'center';
        for (let i = 0; i <= 4; i++) {
            const t = t0 + WINDOW_MS * i / 4, d = new Date(t);
            g.fillText([d.getHours(), d.getMinutes(), d.getSeconds()].map(n => String(n).padStart(2, '0')).join(':'), Math.min(Math.max(x(t), L + 24), W - R - 24), H - 5);
        }
        SERIES.forEach(s => {
            g.strokeStyle = s[2]; g.lineWidth = 1.6; g.beginPath();
            let open = false;
            data.forEach(p => {
                if (p[s[0]] == null) { open = false; return; }
                if (open) g.lineTo(x(p.t), y(p[s[0]])); else { g.moveTo(x(p.t), y(p[s[0]])); open = true; }
            });
            g.stroke();
        });
    }

    function openChart(history) {
        const canvas = h('canvas.chart');
        const last = h('div.chart-legend');
        const body = h('div', canvas, last);
        let timer = null;
        const refresh = () => {
            draw(canvas, history.data);
            const p = history.data[history.data.length - 1];
            last.replaceChildren(...SERIES.filter(s => !p || p[s[0]] != null).map(s =>
                h('span', h('i', { style: 'background:' + s[2] }), Z.t(s[1]) + (p ? ' ' + Math.round(p[s[0]]) + '°C' : ''))));
            if (!history.data.length) last.appendChild(h('span.muted', { text: Z.t('chart_wait') }));
        };
        Z.dialog({ title: Z.t('hist_chart_title'), body: body, wide: true, onClose: () => clearInterval(timer), buttons: [{ label: Z.t('close') }] });
        refresh();
        timer = setInterval(refresh, 2000);
    }

    Z.createTempHistory = createHistory;
    Z.openTempChart = openChart;
})(window.Z = window.Z || {});
