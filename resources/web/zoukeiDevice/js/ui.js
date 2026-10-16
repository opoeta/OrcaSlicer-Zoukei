// Small DOM helpers shared by every screen. No framework: elements are built with h() and
// patched in place by their owners; printer-supplied text only ever goes through textContent.
(function (Z) {
    'use strict';

    // h('div.card', {onclick: fn, title: 'x'}, child, 'text', [more]) -> Element
    function h(spec, attrs, ...kids) {
        if (attrs instanceof Node || Array.isArray(attrs) || typeof attrs === 'string' || typeof attrs === 'number') {
            kids.unshift(attrs);                     // no attributes given: the second argument is a child
            attrs = null;
        }
        const parts = spec.split('.');
        const el = document.createElement(parts[0] || 'div');
        if (parts.length > 1) el.className = parts.slice(1).join(' ');
        for (const key in (attrs || {})) {
            const v = attrs[key];
            if (v == null || v === false) continue;
            if (key.startsWith('on') && typeof v === 'function') el.addEventListener(key.slice(2), v);
            else if (key === 'class') el.className += (el.className ? ' ' : '') + v;
            else if (key === 'text') el.textContent = v;
            else if (key === 'value' || key === 'checked' || key === 'disabled') el[key] = v;
            else el.setAttribute(key, v === true ? '' : v);
        }
        append(el, kids);
        return el;
    }

    function append(el, kids) {
        for (const k of kids) {
            if (k == null || k === false) continue;
            if (Array.isArray(k)) append(el, k);
            else el.appendChild(k instanceof Node ? k : document.createTextNode(String(k)));
        }
    }

    function clear(el) {
        while (el.firstChild) el.removeChild(el.firstChild);
        return el;
    }

    // Line icons, 24x24, drawn with the current text colour.
    const ICONS = {
        back: '<path d="M19 12H5M12 19l-7-7 7-7"/>',
        plus: '<path d="M12 5v14M5 12h14"/>',
        refresh: '<path d="M21 12a9 9 0 1 1-3-6.7M21 4v5h-5"/>',
        scan: '<circle cx="11" cy="11" r="7"/><path d="M21 21l-5-5"/>',
        edit: '<path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>',
        trash: '<path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14M10 10v6M14 10v6"/>',
        more: '<path d="M5 12h.01M12 12h.01M19 12h.01" stroke-width="3"/>',
        pause: '<path d="M8 5v14M16 5v14" stroke-width="3"/>',
        play: '<path d="M7 4l13 8-13 8z"/>',
        stop: '<rect x="6" y="6" width="12" height="12" rx="1"/>',
        home: '<path d="M3 11l9-8 9 8M5 10v10h14V10"/>',
        up: '<path d="M6 15l6-6 6 6"/>',
        down: '<path d="M6 9l6 6 6-6"/>',
        left: '<path d="M15 6l-6 6 6 6"/>',
        right: '<path d="M9 6l6 6-6 6"/>',
        drop: '<path d="M12 3c4 5 6 8 6 11a6 6 0 0 1-12 0c0-3 2-6 6-11z"/>',
        bulb: '<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-4 10.5c.7.7 1 1.5 1 2.5h6c0-1 .3-1.8 1-2.5A6 6 0 0 0 12 3z"/>',
        fan: '<circle cx="12" cy="12" r="1.5"/>' +
            '<path d="M12 10.5C11 6 12 3 14.5 3c2 0 2.5 2 1.5 4-.7 1.5-2.2 2.7-4 3.5z"/>' +
            '<path transform="rotate(120 12 12)" d="M12 10.5C11 6 12 3 14.5 3c2 0 2.5 2 1.5 4-.7 1.5-2.2 2.7-4 3.5z"/>' +
            '<path transform="rotate(240 12 12)" d="M12 10.5C11 6 12 3 14.5 3c2 0 2.5 2 1.5 4-.7 1.5-2.2 2.7-4 3.5z"/>',
        lock: '<path d="M6 11h12v9H6zM8 11V8a4 4 0 0 1 8 0v3"/>',
        file: '<path d="M6 3h8l4 4v14H6zM14 3v4h4"/>',
        temp: '<path d="M10 14V5a2 2 0 0 1 4 0v9a4 4 0 1 1-4 0z"/>',
        clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
        x: '<path d="M6 6l12 12M18 6L6 18"/>',
        check: '<path d="M5 12l5 5 9-10"/>',
        layers: '<path d="M12 3l9 5-9 5-9-5zM3 13l9 5 9-5"/>',
        camera: '<path d="M4 7h4l2-3h4l2 3h4v12H4z"/><circle cx="12" cy="13" r="3.5"/>',
        printer: '<path d="M7 9V3h10v6M7 17H4v-7h16v7h-3M7 14h10v7H7z"/>',
        warn: '<path d="M12 3l10 18H2zM12 10v5M12 18h.01"/>',
        feed: '<path d="M12 4v13M6 12l6 6 6-6"/>',
        chart: '<path d="M3 20h18M5 16l4-5 4 3 6-8"/>',
        retract: '<path d="M12 20V7M6 12l6-6 6 6"/>'
    };

    function icon(name, cls) {
        const span = document.createElement('span');
        span.className = 'ico' + (cls ? ' ' + cls : '');
        // The markup comes from the fixed table above, never from printer data.
        span.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
            'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + (ICONS[name] || '') + '</svg>';
        return span;
    }

    // ---- toasts and dialogs -------------------------------------------------------------------

    function toast(message, kind) {
        const box = document.getElementById('toasts');
        const el = h('div.toast' + (kind ? '.' + kind : ''), { role: 'status' }, message);
        box.appendChild(el);
        setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 300); }, kind === 'err' ? 6000 : 3200);
    }

    // dialog({title, body: Node, buttons: [{label, kind, onClick}], wide}) -> {close, el}
    // onClick may return false to keep the dialog open.
    function dialog(opts) {
        const prevFocus = document.activeElement;
        const close = () => {
            overlay.remove();
            document.removeEventListener('keydown', onKey, true);
            if (opts.onClose) opts.onClose();
            if (prevFocus && prevFocus.focus) prevFocus.focus();
        };
        const btns = (opts.buttons || []).map(b => h('button.btn' + (b.kind ? '.' + b.kind : ''), {
            type: 'button',
            onclick: () => { if (!b.onClick || b.onClick() !== false) close(); }
        }, b.label));
        const box = h('div.dialog' + (opts.wide ? '.wide' : ''), { role: 'dialog', 'aria-modal': 'true' },
            h('div.dialog-head', h('h3', { text: opts.title }),
                h('button.icon-btn', { type: 'button', 'aria-label': Z.t('close'), onclick: close }, icon('x'))),
            h('div.dialog-body', opts.body),
            btns.length ? h('div.dialog-foot', btns) : null);
        const overlay = h('div.overlay', { onmousedown: e => { if (e.target === overlay) close(); } }, box);
        const onKey = e => { if (e.key === 'Escape') { e.stopPropagation(); close(); } };
        document.addEventListener('keydown', onKey, true);
        document.body.appendChild(overlay);
        const first = box.querySelector('input,select,textarea') || box.querySelector('.dialog-foot .btn');
        if (first) first.focus();
        return { close, el: box };
    }

    // Resolves true on the confirming button, false on anything else (cancel, Escape, backdrop).
    function confirmDialog(message, opts) {
        opts = opts || {};
        return new Promise(resolve => {
            let answer = false;
            dialog({
                title: opts.title || Z.t('confirm'),
                body: h('p', { text: message }),
                onClose: () => resolve(answer),
                buttons: [
                    { label: Z.t('cancel') },
                    { label: opts.okLabel || Z.t('ok'), kind: opts.danger ? 'danger' : 'primary', onClick: () => { answer = true; } }
                ]
            });
        });
    }

    // ---- formatting ---------------------------------------------------------------------------

    function duration(seconds) {
        seconds = Math.max(0, Math.round(Number(seconds) || 0));
        const hh = Math.floor(seconds / 3600), mm = Math.floor(seconds % 3600 / 60);
        if (hh) return hh + 'h ' + String(mm).padStart(2, '0') + 'm';
        return mm + 'm ' + String(seconds % 60).padStart(2, '0') + 's';
    }

    function bytes(n) {
        n = Number(n) || 0;
        if (n >= 1073741824) return (n / 1073741824).toFixed(2) + ' GB';
        if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
        if (n >= 1024) return Math.round(n / 1024) + ' KB';
        return n + ' B';
    }

    function baseName(path) { return String(path || '').split(/[\\/]/).pop(); }

    function num(v, fallback) { const n = parseFloat(v); return isFinite(n) ? n : (fallback === undefined ? 0 : fallback); }

    // The printer writes colours as "#0RRGGBB" (a leading zero nibble); the page uses "#RRGGBB".
    function colorFromPrinter(c) {
        c = String(c || '').replace(/[^0-9a-fA-F]/g, '');
        if (c.length === 7 && c[0] === '0') c = c.slice(1);
        else if (c.length === 8) c = c.slice(0, 6);
        return c.length === 6 ? '#' + c.toUpperCase() : '';
    }
    function colorToPrinter(c) {
        const v = colorFromPrinter(c);
        return v ? '#0' + v.slice(1).toLowerCase() : '';
    }

    Z.h = h; Z.clear = clear; Z.icon = icon; Z.toast = toast; Z.dialog = dialog; Z.confirm = confirmDialog;
    Z.fmt = { duration, bytes, baseName, num, colorFromPrinter, colorToPrinter };
})(window.Z = window.Z || {});
