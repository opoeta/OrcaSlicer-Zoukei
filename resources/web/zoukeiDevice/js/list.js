// Device list screen: the printers known to Orca Zoukei, adding by IP, scanning the LAN,
// renaming and removing. Online state and the CFS colour chips come from a short peek at
// each printer (a socket opened just long enough to read status and CFS).
(function (Z) {
    'use strict';

    const { h, icon, clear } = Z;
    const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

    function validIpv4(text) {
        const m = IPV4.exec(text);
        return !!m && m.slice(1).every(p => Number(p) <= 255);
    }

    function displayName(dev) { return dev.name || dev.modelName || dev.model || dev.address; }

    // Row(s) of coloured dots, one per CFS slot.
    function cfsChips(boxes) {
        const units = ((boxes && boxes.materialBoxs) || []).filter(b => b.type === 0);
        if (!units.length) return null;
        return h('div.chips', units.map(unit => h('div.chip-row', (unit.materials || []).map(m => {
            const color = Z.fmt.colorFromPrinter(m.color);
            const loaded = m.state !== 0 && (m.type || m.name || m.vendor || color);
            return h('span.dot' + (loaded ? '' : '.empty'), {
                style: loaded && color ? 'background:' + color : null,
                title: loaded ? [m.type, m.name].filter(Boolean).join(' · ') : Z.t('slot_empty')
            });
        }))));
    }

    function createList() {
        const mr = Z.backend === 'moonraker';
        const peeks = {};              // address -> {online, state, boxes}
        let devices = [];
        let timer = null;
        let active = false;
        let scanning = false;
        const cards = new Map();       // address -> element

        // ---- header: add by IP + scan --------------------------------------------------------
        const ipInput = h('input.input', { type: 'text', placeholder: '192.168.0.50', maxlength: 15, 'aria-label': Z.t('ip_address'), spellcheck: 'false' });
        const addBtn = h('button.btn.primary', { type: 'button', onclick: addByIp }, icon('plus'), Z.t('add'));
        const scanBtn = h('button.btn', { type: 'button', onclick: scan }, icon('scan'), Z.t('scan_lan'));
        ipInput.addEventListener('keydown', e => { if (e.key === 'Enter') addByIp(); });
        const found = h('div.found', { hidden: true });
        const grid = h('div.device-grid');
        const empty = h('div.list-empty', { hidden: true }, h('p', { text: Z.t(mr ? 'list_empty_ff' : 'list_empty') }));

        const el = h('section.screen.list',
            h('header.topbar',
                h('h1', { text: Z.t('printers') }),
                h('div.spacer'),
                mr ? null : h('div.add-ip', ipInput, addBtn),
                mr ? null : scanBtn),
            found, grid, empty);

        function addByIp() {
            const address = ipInput.value.trim();
            if (!validIpv4(address)) { Z.toast(Z.t('err_bad_ip'), 'err'); ipInput.focus(); return; }
            addDevice(address, addBtn).then(ok => { if (ok) ipInput.value = ''; });
        }

        function addDevice(address, button) {
            if (button) button.disabled = true;
            return Z.Host.request('device_add', { address: address }, 'device_add_result', 30000)
                .then(res => {
                    if (res.ok) { Z.toast(Z.t('added') + ' ' + address); return true; }
                    Z.toast(res.errorType === 'invalid_address' ? Z.t('err_not_lan') : Z.t('err_probe') + (res.error ? ' (' + res.error + ')' : ''), 'err');
                    return false;
                })
                .catch(() => { Z.toast(Z.t('err_probe'), 'err'); return false; })
                .then(ok => { if (button) button.disabled = false; return ok; });
        }

        // ---- LAN scan -----------------------------------------------------------------------
        function scan() {
            if (scanning) return;
            scanning = true;
            scanBtn.disabled = true;
            clear(found).hidden = false;
            found.appendChild(h('div.found-head', h('span.spinner'), h('span', { text: Z.t('scanning') })));
            Z.Host.request('lan_scan', {}, 'lan_scan_result', 90000)
                .then(res => showFound(res))
                .catch(() => { clear(found).hidden = true; Z.toast(Z.t('err_scan'), 'err'); })
                .then(() => { scanning = false; scanBtn.disabled = false; });
        }

        function showFound(res) {
            clear(found);
            if (res.busy) { found.hidden = true; return; }
            const known = new Set(devices.map(d => d.address));
            const list = (res.found || []).filter(p => p && p.address);
            found.appendChild(h('div.found-head', h('strong', { text: Z.t('scan_found', { n: list.length }) }),
                h('div.spacer'),
                h('button.icon-btn', { type: 'button', 'aria-label': Z.t('close'), onclick: () => { found.hidden = true; } }, icon('x'))));
            if (!list.length) found.appendChild(h('p.muted', { text: Z.t('scan_none') }));
            list.forEach(p => {
                const isKnown = known.has(p.address);
                const btn = h('button.btn.small', { type: 'button', disabled: isKnown, onclick: () => addDevice(p.address, btn).then(ok => { if (ok) { btn.textContent = Z.t('added'); btn.disabled = true; } }) },
                    isKnown ? Z.t('added') : Z.t('add'));
                found.appendChild(h('div.found-row',
                    h('span.name', { text: p.modelName || p.model || 'Creality' }),
                    h('span.muted', { text: p.address }),
                    h('div.spacer'), btn));
            });
        }

        // ---- cards --------------------------------------------------------------------------
        function badge(dev) {
            const p = peeks[dev.address];
            if (!p) return h('span.badge.wait', { text: Z.t('checking') });
            if (!p.online) return h('span.badge.off', { text: Z.t('offline') });
            const d = Z.derive(p.state);
            const label = d.phase === 'printing' ? Z.t('phase_printing') + ' ' + Math.round(d.progress) + '%' : Z.t('online');
            return h('span.badge.on', { text: label });
        }

        function cardFor(dev) {
            const menu = h('div.menu', { hidden: true },
                h('button', { type: 'button', onclick: e => { e.stopPropagation(); menu.hidden = true; rename(dev); } }, icon('edit'), Z.t('rename')),
                h('button.danger', { type: 'button', onclick: e => { e.stopPropagation(); menu.hidden = true; remove(dev); } }, icon('trash'), Z.t('remove')));
            const more = h('button.icon-btn', { type: 'button', 'aria-label': Z.t('more'), onclick: e => {
                e.stopPropagation();
                const wasHidden = menu.hidden;
                document.querySelectorAll('.device-card .menu').forEach(m => { m.hidden = true; });
                menu.hidden = !wasHidden;
            } }, icon('more'));
            const cover = dev.cover ? h('img', { src: dev.cover, alt: '' }) : icon('printer');
            const p = peeks[dev.address];
            const card = h('article.device-card' + (p && p.online ? '' : '.dim'), { tabindex: 0, role: 'button', onclick: () => Z.app.open(dev),
                onkeydown: e => { if (e.key === 'Enter') Z.app.open(dev); } },
                h('div.cover', cover),
                h('div.info',
                    h('div.row', h('h2', { text: displayName(dev) }), h('div.spacer'), mr ? null : more, mr ? null : menu),
                    h('div.muted', { text: [dev.modelName || dev.model, dev.address].filter(Boolean).join(' · ') }),
                    h('div.row', badge(dev)),
                    cfsChips(p && p.boxes)));
            return card;
        }

        function renderCards() {
            const seen = new Set();
            devices.forEach((dev, i) => {
                seen.add(dev.address);
                const card = cardFor(dev);
                const old = cards.get(dev.address);
                if (old && old.parentNode === grid) grid.replaceChild(card, old);
                else grid.insertBefore(card, grid.children[i] || null);
                cards.set(dev.address, card);
            });
            cards.forEach((card, address) => { if (!seen.has(address)) { card.remove(); cards.delete(address); } });
            empty.hidden = devices.length > 0;
        }

        function rename(dev) {
            const input = h('input.input', { type: 'text', value: dev.name || '', maxlength: 60, placeholder: dev.modelName || dev.model || '' });
            const save = () => { Z.Host.send('device_rename', { address: dev.address, name: input.value.trim() }); };
            const dlg = Z.dialog({ title: Z.t('rename'), body: h('label.field', h('span', { text: Z.t('name') }), input),
                buttons: [{ label: Z.t('cancel') }, { label: Z.t('save'), kind: 'primary', onClick: save }] });
            input.addEventListener('keydown', e => { if (e.key === 'Enter') { save(); dlg.close(); } });
        }

        function remove(dev) {
            Z.confirm(Z.t('remove_confirm', { name: displayName(dev) }), { danger: true, okLabel: Z.t('remove') })
                .then(yes => { if (yes) Z.Host.send('device_remove', { address: dev.address }); });
        }

        // ---- peeking ------------------------------------------------------------------------
        function peekAll() {
            devices.forEach(dev => {
                (mr ? Z.peekMoonraker : Z.peek)(dev, 7000).then(result => {
                    peeks[dev.address] = result;
                    if (!active) return;
                    const card = cards.get(dev.address);
                    const idx = devices.indexOf(dev);
                    if (card && idx >= 0 && card.parentNode === grid) {
                        const fresh = cardFor(dev);
                        grid.replaceChild(fresh, card);
                        cards.set(dev.address, fresh);
                    }
                });
            });
        }

        document.addEventListener('click', () => el.querySelectorAll('.menu').forEach(m => { m.hidden = true; }));

        return {
            el: el,
            setDevices(list) {
                devices = list.slice();
                renderCards();
                if (active) peekAll();
            },
            peekOf(address) { return peeks[address]; },
            activate() {
                active = true;
                renderCards();
                peekAll();
                clearInterval(timer);
                timer = setInterval(peekAll, 30000);
            },
            deactivate() { active = false; clearInterval(timer); }
        };
    }

    Z.createList = createList;
})(window.Z = window.Z || {});
