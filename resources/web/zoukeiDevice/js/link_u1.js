// Snapmaker U1 backend: the Moonraker link plus the four tool heads. The U1 (Klipper + Moonraker) reports its heads in
// print_task_config (type / colour / official lock per head, extruder map), filament_feed left|right (feeder state per head)
// and extruder .. extruder3 (nozzle temperatures). Reads go through the host like every Moonraker request; writes that
// change filament data (type / colour, load / unload) go through the host's agent, which refuses them while printing.
// Firmware facts: github.com/Snapmaker/u1-klipper (print_task_config.py, filament_feed.py, lava/fluidd.cfg).
(function (Z) {
    'use strict';

    const { num } = Z.fmt;
    const HEADS = 4;
    const enc = s => encodeURIComponent(s);
    const hexOf = h => { const c = String(h || '').replace(/[^0-9a-fA-F]/g, ''); return c.length >= 6 ? '#' + c.slice(0, 6).toUpperCase() : ''; };
    const none = v => { const s = String(v == null ? '' : v).trim(); return /^none$/i.test(s) ? '' : s; };

    // The four heads from the raw objects. null when the printer has no print_task_config (not a U1).
    function readHeads(raw) {
        const ptc = raw.print_task_config;
        if (!ptc || typeof ptc !== 'object') return null;
        const feed = Object.assign({}, raw['filament_feed left'], raw['filament_feed right']);
        const map = Array.isArray(ptc.extruder_map_table) ? ptc.extruder_map_table : [];
        const at = (k, i) => Array.isArray(ptc[k]) ? ptc[k][i] : undefined;
        const heads = [];
        for (let i = 0; i < HEADS; i++) {
            const rgba = String(at('filament_color_rgba', i) || 'FFFFFFFF');
            const color = hexOf(rgba) || '#FFFFFF';
            const multi = at('filament_color_multi', i);
            const cs = multi && Array.isArray(multi.colors) ? multi.colors.map(hexOf).filter(Boolean).slice(0, 4) : [];
            const isMulti = cs.length >= 2 && cs[0] === color;
            const ex = raw[i ? 'extruder' + i : 'extruder'] || {};
            const f = feed['extruder' + i] || {};
            const mapFrom = map.slice(0, HEADS).findIndex(p => p === i);
            heads.push({
                id: i + 1, index: i,
                present: !!at('filament_exist', i), official: !!at('filament_official', i), editable: !!at('filament_edit', i),
                soft: !!at('filament_soft', i),
                vendor: none(at('filament_vendor', i)), material: none(at('filament_type', i)), sub: none(at('filament_sub_type', i)),
                color: color, colors: isMulti ? cs : [], gradient: isMulti && num(multi.mode) === 1,
                nozzle: num(ex.temperature), target: num(ex.target),
                feed: {
                    module: f.module_exist !== false, detected: !!f.filament_detected, auto: f.disable_auto === undefined ? true : !f.disable_auto,
                    state: String(f.channel_state || ''), action: String(f.channel_action_state || 'none'), error: String(f.channel_error || 'ok')
                },
                mapFrom: mapFrom
            });
        }
        return heads;
    }

    // A feed state that is work in progress (the others are idle, finished or failed).
    function feedBusy(f) {
        if (f.action && f.action !== 'none') return true;
        const s = f.state;
        if (!s || /^(none|inited|wait_insert|test)$/.test(s)) return false;
        return !/(finish|fail)$/.test(s);
    }

    class U1Link extends Z.MoonrakerLink {
        constructor(dev, options) {
            super(dev, options);
            this.heads = null;
            this.mapTable = [];
            this.activeExtruder = 'extruder';
        }

        // Which objects exist decides the cards: U1 heads, fans, the chamber (cavity) sensors.
        _plan() {
            const o = this._objects;
            this.fans = {};
            if (o.indexOf('fan') >= 0) this.fans.modelFanPct = 'fan';
            if (o.indexOf('fan_generic cavity_fan') >= 0) this.fans.caseFanPct = 'fan_generic cavity_fan';
            this.led = null;       // the cavity LED is a Klipper [led]; its channels are not known here, so the card stays hidden
            this.hasLw = false;
            const heads = ['extruder', 'extruder1', 'extruder2', 'extruder3'].filter(k => o.indexOf(k) >= 0);
            const fast = ['print_stats', 'virtual_sdcard', 'display_status', 'heater_bed', 'toolhead', 'gcode_move', 'webhooks',
                'print_task_config', 'filament_feed left', 'filament_feed right'].filter(k => o.indexOf(k) >= 0);
            this.fastKeys = fast.concat(heads, Object.keys(this.fans).map(k => this.fans[k]));
            this.slowKeys = [];
        }

        _derive() {
            const orig = this.raw, th = orig.toolhead || {};
            const act = th.extruder && orig[th.extruder] ? th.extruder : 'extruder';
            this.activeExtruder = act;
            this.heads = readHeads(orig);
            this.mapTable = orig.print_task_config && Array.isArray(orig.print_task_config.extruder_map_table) ? orig.print_task_config.extruder_map_table.slice(0, HEADS) : [];
            // the nozzle card follows the head in use
            this.raw = act === 'extruder' ? orig : Object.assign({}, orig, { extruder: orig[act] });
            try { super._derive(); } finally { this.raw = orig; }
            this.state.activeHead = act === 'extruder' ? 0 : num(act.replace('extruder', ''));
            this.state.headsFeedBusy = !!(this.heads && this.heads.some(x => feedBusy(x.feed)));
            this.ifs = null;
            this.boxes = this.heads ? { heads: this.heads } : null;
            this._emit('boxes');
        }

        setNozzleTemp(v) {
            const heater = this.activeExtruder || 'extruder';
            return this._order('SET_HEATER_TEMPERATURE HEATER=' + heater + ' TARGET=' + Math.round(v), { targetNozzleTemp: v });
        }
        setLight() { return false; }
        _loadLanes() { return Promise.resolve(); }   // HelixScreen's lane_data does not exist on the U1

        // ---- filament: writes through the host's agent (refused while printing, official spools need its confirm) -------------

        writeHead(payload) {
            return Z.Host.request('u1_write_head', payload, 'u1_write_head', 20000);
        }
        feedHead(head, action) {
            return Z.Host.request('u1_feed', { head: head, action: action }, 'u1_feed', 20000);
        }
        // A fresh read of the head objects (after a write the firmware needs a moment).
        async refreshHeads() {
            try {
                const keys = ['print_task_config', 'filament_feed left', 'filament_feed right'].filter(k => this._objects.indexOf(k) >= 0);
                const r = await this.api('GET', '/printer/objects/query?' + keys.map(enc).join('&'));
                Object.assign(this.raw, (r && r.status) || {});
                this._derive();
            } catch (e) { /* the next poll retries */ }
        }
        // For the slot editor: the firmware's main types.
        validTypes() { return ['PLA', 'PLA-CF', 'PETG', 'PETG-CF', 'ABS', 'ASA', 'TPU', 'PVA', 'PA', 'PA-CF', 'PA-GF', 'PC', 'PEBA', 'TPE']; }
        refreshIfs() { return this.refreshHeads(); }

        // Printing a file of the printer: the printer's current extruder map applies (Orca sets it when it sends the print).
        async printFile(path) {
            try {
                await this.api('POST', '/printer/print/start?filename=' + enc(path));
                return true;
            } catch (e) {
                Z.toast(e.message, 'err');
                return false;
            }
        }
    }

    // Opens the link just long enough to learn status and head colours (device list cards).
    function peekU1(dev, timeoutMs) {
        return new Promise(resolve => {
            const link = new U1Link(dev, { once: true });
            let done = false;
            const finish = () => {
                if (done) return;
                done = true;
                clearTimeout(timer);
                const heads = link.heads || [];
                const boxes = heads.length ? { materialBoxs: [{ type: 0, materials: heads.map(s => ({ state: s.present ? 1 : 0, type: s.material, name: s.sub || s.vendor, color: s.color })) }] } : null;
                const result = { online: link.hasStatus, state: link.state, boxes: boxes };
                link.close();
                resolve(result);
            };
            const timer = setTimeout(finish, timeoutMs || 8000);
            link.on('ready', () => setTimeout(finish, 600));
            link.connect();
        });
    }

    Z.U1Link = U1Link;
    Z.peekU1 = peekU1;
    Z.u1FeedBusy = feedBusy;
})(window.Z = window.Z || {});
