// Moonraker backend (Flashforge AD5X on ZMOD: Klipper + Moonraker). Same interface as PrinterLink in
// link.js, so the printer screen is shared: the printer's objects are polled through the host
// (the page may not call a LAN address itself) and folded into the status keys the screen reads.
// Orders are G-code scripts or Moonraker print calls. The IFS (4 slots) comes from ZMOD's
// zmod_color object; the lessWaste plugin, when installed, is spoken to the way its own start dialog does.
(function (Z) {
    'use strict';

    const { num } = Z.fmt;
    const FAST = ['print_stats', 'virtual_sdcard', 'display_status', 'extruder', 'heater_bed', 'toolhead', 'gcode_move', 'webhooks'];
    const SLOW = ['save_variables', 'zmod_color', 'zmod_ifs'];
    const STATE = { standby: 0, printing: 1, paused: 5, complete: 2, cancelled: 4, error: 4 };

    const enc = s => encodeURIComponent(s);
    const sleep = ms => new Promise(r => setTimeout(r, ms));
    const hexOf = h => { const c = String(h || '').replace(/[^0-9a-fA-F]/g, ''); return c.length >= 6 ? '#' + c.slice(0, 6).toUpperCase() : ''; };

    function colorDistance(a, b) {
        const p = hexOf(a), q = hexOf(b);
        if (!p || !q) return 1e6;
        let d = 0;
        for (let i = 0; i < 3; i++) { const x = parseInt(p.substr(1 + i * 2, 2), 16) - parseInt(q.substr(1 + i * 2, 2), 16); d += x * x; }
        return Math.sqrt(d);
    }

    // The IFS as ZMOD reports it: ui detection follows the agent (display_off + helix / guppy).
    function readIfs(raw, lanes) {
        const zc = raw.zmod_color;
        const vars = (raw.save_variables && raw.save_variables.variables) || {};
        const ui = num(vars.display_off) !== 0 ? (num(vars.helix) !== 0 ? 'helix' : num(vars.guppy) !== 0 ? 'guppy' : 'original') : 'original';
        if (!zc || !Array.isArray(zc.slots)) return null;
        const ports = (raw.zmod_ifs && raw.zmod_ifs.Ports) || [];
        return {
            ui: ui, available: zc.ifs !== false,
            slots: zc.slots.map((s, i) => {
                const id = Number(s.ID) || i + 1, color = hexOf(s.HEX), lane = lanes && lanes['lane' + id];
                // dual / tri / quad / gradient info of lane_data, valid only while its first colour is what the firmware reports
                const cs = lane && Array.isArray(lane.multi_color_hexes) ? lane.multi_color_hexes.map(hexOf).filter(Boolean).slice(0, 4) : [];
                const multi = cs.length >= 2 && cs[0] === color && hexOf(lane.color) === color;
                return {
                    id: id, material: String(s.Material || ''), colorName: String(s.Color || ''),
                    color: color, present: !!s.hasFilament || ports[i] === true,
                    colors: multi ? cs : [], gradient: multi && lane.color_type === 'gradient'
                };
            })
        };
    }

    class MoonrakerLink {
        constructor(dev, options) {
            this.dev = dev;
            this.options = options || {};
            this.state = {};
            this.raw = {};
            this.boxes = null;
            this.boxConfig = null;
            this.files = null;
            this.videos = null;
            this.history = null;
            this.library = null;
            this.token = '';
            this.online = false;
            this.hasStatus = false;
            this.ifs = null;
            this.lessWaste = null;
            this.webcams = [];
            this.meta = null;            // metadata of the current print file
            this._metaFor = '';
            this.fans = {};
            this.led = null;
            this.limits = { nozzle: 300, bed: 120 };
            this._events = {};
            this._closed = false;
            this._active = true;
            this._tick = 0;
            this._fails = 0;
            this._objects = [];
        }

        on(name, fn) {
            (this._events[name] = this._events[name] || []).push(fn);
            return () => { this._events[name] = this._events[name].filter(f => f !== fn); };
        }
        _emit(name, a, b) {
            (this._events[name] || []).slice().forEach(fn => { try { fn(a, b); } catch (e) { console.error(e); } });
        }

        // ---- transport --------------------------------------------------------------------------

        api(method, path, body) {
            return Z.Host.request('moonraker_http', { address: this.dev.address, method: method, path: path, body: body ? JSON.stringify(body) : '' }, 'moonraker_http', 60000)
                .then(r => {
                    let j = null;
                    try { j = JSON.parse(r.body); } catch (e) { /* not JSON */ }
                    if (!r.ok) {
                        const msg = (j && j.error && j.error.message) || r.error || ('HTTP ' + r.status);
                        throw new Error(String(msg).split('\n')[0]);
                    }
                    return j && 'result' in j ? j.result : j;
                });
        }

        // A G-code script; failures are shown to the user (the printer's own message).
        script(gcode, quiet) {
            return this.api('POST', '/printer/gcode/script', { script: gcode })
                .catch(e => { if (!quiet) Z.toast(e.message, 'err'); throw e; });
        }
        _order(gcode, patch) {
            if (!this.online) return false;
            if (patch) this._merge(patch);
            this.script(gcode).catch(() => {});
            return true;
        }
        _merge(obj) {
            Object.assign(this.state, obj);
            this._emit('state', obj);
        }

        base() { return 'http://' + this.dev.address + ':' + (this.dev.port || 7125); }
        webcam() { return this.webcams[0] || null; }

        // ---- connection / polling -----------------------------------------------------------------

        connect() {
            if (this._closed) return;
            this._boot();
        }

        async _boot() {
            try {
                const list = await this.api('GET', '/printer/objects/list');
                this._objects = (list && list.objects) || [];
                this._plan();
                this.api('GET', '/server/webcams/list').then(r => { this.webcams = this._webcams((r && r.webcams) || []); this._emit('camera'); }).catch(() => {});
                this.api('GET', '/printer/objects/query?configfile=settings').then(r => {
                    const set = (r && r.status && r.status.configfile && r.status.configfile.settings) || {};
                    if (set.extruder && num(set.extruder.max_temp) > 0) this.limits.nozzle = num(set.extruder.max_temp);
                    if (set.heater_bed && num(set.heater_bed.max_temp) > 0) this.limits.bed = num(set.heater_bed.max_temp);
                    this._derive();
                }).catch(() => {});
                await this._fetch(true);
                this._schedule();
            } catch (e) {
                this._fail();
                if (!this._closed) this._retry = setTimeout(() => this._boot(), 5000);
            }
        }

        _webcams(list) {
            const out = [];
            list.forEach(c => {
                if (c.enabled === false || /iframe|webrtc/i.test(String(c.service || ''))) return;
                const url = u => /^https?:\/\//i.test(u) ? (u.indexOf('//' + this.dev.address) > 0 ? u : '') : 'http://' + this.dev.address + (u || '');
                const stream = url(c.stream_url || '/webcam/?action=stream'), snap = url(c.snapshot_url || '/webcam/?action=snapshot');
                if (stream) out.push({ name: c.name, stream: stream, snapshot: snap });
            });
            if (!out.length) out.push({ name: 'video', stream: 'http://' + this.dev.address + '/webcam/?action=stream', snapshot: 'http://' + this.dev.address + '/webcam/?action=snapshot' });
            return out;
        }

        // Which objects exist decides the cards: fans, light, lessWaste, sensors.
        _plan() {
            const o = this._objects;
            this.fans = {};
            o.forEach(n => {
                if (n === 'fan') this.fans.modelFanPct = n;
                else if (n === 'fan_generic fanM106') this.fans.modelFanPct = n;
                else if (n === 'fan_generic chamber_fan') this.fans.caseFanPct = n;
            });
            o.filter(n => /^fan_generic /.test(n) && Object.values(this.fans).indexOf(n) < 0).slice(0, 1).forEach(n => { this.fans.auxiliaryFanPct = n; });
            const led = o.find(n => /^led /.test(n)) || o.find(n => /^output_pin .*(led|light)/i.test(n));
            this.led = led ? { obj: led, pin: /^output_pin/.test(led) } : null;
            this.hasLw = o.indexOf('gcode_macro _IFS_VARS') >= 0 && o.indexOf('gcode_macro _IFS_COLORS_PRINT') >= 0 && o.indexOf('gcode_macro _IFS_COLORS') >= 0;
            this.fastKeys = FAST.filter(k => o.indexOf(k) >= 0)
                .concat(Object.keys(this.fans).map(k => this.fans[k]), this.led ? [this.led.obj] : []);
            this.slowKeys = SLOW.filter(k => o.indexOf(k) >= 0).concat(this.hasLw ? ['gcode_macro _IFS_VARS', 'gcode_macro _IFS_COLORS'] : [],
                o.filter(n => /^filament_switch_sensor _ifs_port_sensor_\d$/.test(n)));
        }

        async _fetch(first) {
            if (this._busy) return;
            this._busy = true;
            try {
                const keys = this.fastKeys.slice();
                if (first || this._tick++ % 6 === 0) Array.prototype.push.apply(keys, this.slowKeys);
                const r = await this.api('GET', '/printer/objects/query?' + keys.map(enc).join('&'));
                Object.assign(this.raw, (r && r.status) || {});
                if (keys.indexOf('zmod_color') >= 0) await this._loadLanes();
                this._fails = 0;
                const firstStatus = !this.hasStatus;
                this.hasStatus = true;
                if (!this.online) { this.online = true; this._emit('online', true); }
                this._derive();
                if (firstStatus) this._emit('ready');
            } catch (e) {
                this._fail();
            } finally {
                this._busy = false;
            }
        }

        _fail() {
            if (++this._fails >= 3 && this.online) { this.online = false; this._emit('online', false); }
        }

        _schedule() {
            clearTimeout(this._timer);
            if (this._closed) return;
            this._timer = setTimeout(async () => {
                if (this._active) await this._fetch(false);
                this._schedule();
            }, this.options.once ? 1e9 : (this.online ? 1500 : 4000));
        }

        setActive(on) {
            this._active = on;
            if (on && !this._closed) { clearTimeout(this._timer); this._fetch(false).then(() => this._schedule()); }
        }

        close() {
            this._closed = true;
            clearTimeout(this._timer);
            clearTimeout(this._retry);
            if (this.online) { this.online = false; this._emit('online', false); }
        }

        // ---- reading the objects into the screen's status keys ------------------------------------------

        _derive() {
            const r = this.raw;
            const ps = r.print_stats || {}, vs = r.virtual_sdcard || {}, ex = r.extruder || {}, bed = r.heater_bed || {};
            const gm = r.gcode_move || {}, th = r.toolhead || {}, wh = r.webhooks || {};
            const code = STATE[ps.state] != null ? STATE[ps.state] : 0;
            const active = code === 1 || code === 5;
            const file = String(ps.filename || '');
            if (file && file !== this._metaFor) this._loadMeta(file);
            let progress = active ? num(vs.progress) * 100 : (code === 2 ? 100 : 0);
            const done = num(ps.print_duration);
            const est = this.meta && num(this.meta.estimated_time) > 0 ? num(this.meta.estimated_time) : 0;
            let left = 0;
            if (active) left = progress > 1 ? done * (100 - progress) / progress : est;
            const pos = th.position || [];
            const o = {
                state: code, deviceState: 0,
                nozzleTemp: num(ex.temperature), targetNozzleTemp: num(ex.target), maxNozzleTemp: this.limits.nozzle,
                bedTemp0: num(bed.temperature), targetBedTemp0: num(bed.target), maxBedTemp: this.limits.bed,
                printFileName: file, printProgress: progress, printJobTime: done || num(ps.total_duration), printLeftTime: left,
                layer: ps.info && ps.info.current_layer != null ? ps.info.current_layer : 0,
                TotalLayer: (ps.info && ps.info.total_layer != null ? ps.info.total_layer : (this.meta ? num(this.meta.layer_count) : 0)) || 0,
                curFeedratePct: Math.round(num(gm.speed_factor, 1) * 100), speedMode: 0,
                curPosition: pos.length >= 3 ? 'X:' + num(pos[0]).toFixed(2) + ' Y:' + num(pos[1]).toFixed(2) + ' Z:' + num(pos[2]).toFixed(2) : '',
                hostname: this.dev.name || ''
            };
            Object.keys(this.fans).forEach(k => { const f = r[this.fans[k]]; if (f) o[k] = Math.round(num(f.speed) * 100); });
            if (this.led && r[this.led.obj]) {
                const l = r[this.led.obj];
                o.lightSw = (this.led.pin ? num(l.value) > 0 : (l.color_data || []).some(px => px.some(c => c > 0))) ? 1 : 0;
            }
            const down = wh.state && wh.state !== 'ready';
            o.err = ps.state === 'error' ? { errcode: 1, value: ps.message || '' } : down ? { errcode: 2, value: wh.state_message || wh.state } : null;
            this.ifs = readIfs(r, this._lanes);
            const lw = r['gcode_macro _IFS_VARS'];
            this.lessWaste = lw ? {
                tools: lw.tools, startPurge: num(lw.start_purge_setting), linePurge: num(lw.line_purge) === 1, leveling: num(lw.kamp) === 1,
                backup: num(lw.backup) === 1, external: num(lw.external) === 1, currentTool: num(lw.current_tool, -1), minVersion: lw.min_version
            } : null;
            o.pauseMarks = this.pauses && this._pauseKey && this._pauseKey.indexOf(file + '|') === 0 && this.pauses.size > 0
                ? this.pauses.list.map(p => ({ layer: num(p[0]), frac: Math.min(1, num(p[1]) / this.pauses.size) })) : null;
            Object.assign(this.state, o);
            this.boxes = this.ifs ? this.ifs : null;
            this._emit('state', o);
            this._emit('boxes');
        }

        _loadMeta(file) {
            this._metaFor = file;
            this.api('GET', '/server/files/metadata?filename=' + enc(file)).then(m => { if (this._metaFor === file) { this.meta = m; this._derive(); this._loadPauses(file, m); } }).catch(() => {});
        }

        // Pause / filament-change commands of the job's file (M600, M601, PAUSE, @pause), scanned by the host once
        // per file version: list of [layer, byte offset]. The bar position is offset / size, like Klipper's progress.
        _loadPauses(file, m) {
            const key = file + '|' + num(m && m.modified) + '|' + num(m && m.size);
            const cache = Z._pauseCache = Z._pauseCache || {};
            this._pauseKey = key;
            this.pauses = cache[key] || null;
            if (cache[key] || !m || num(m.size) <= 0 || num(m.size) > 300e6) { this._derive(); return; }
            Z.Host.request('moonraker_pauses', { address: this.dev.address, path: '/server/files/gcodes/' + String(file).split('/').map(enc).join('/') }, 'moonraker_pauses', 240000)
                .then(r => {
                    if (!r || !r.ok) return;
                    cache[key] = { list: r.pauses || [], size: num(r.size) };
                    if (this._pauseKey === key) { this.pauses = cache[key]; this._derive(); }
                }).catch(() => {});
        }

        // Picture of a G-code file: the largest thumbnail the slicer embedded.
        thumbUrl(meta, name) {
            const list = (meta && meta.thumbnails) || [];
            if (!list.length) return '';
            const best = list.slice().sort((a, b) => num(b.width) - num(a.width))[0];
            const dir = String(name || '').indexOf('/') >= 0 ? String(name).replace(/\/[^/]*$/, '') + '/' : '';
            return this.base() + '/server/files/gcodes/' + (dir + best.relative_path).split('/').map(enc).join('/');
        }
        currentThumb() { return this.meta && this._metaFor === this.state.printFileName ? this.thumbUrl(this.meta, this._metaFor) : ''; }
        fileUrl(root, name) { return this.base() + '/server/files/' + root + '/' + String(name).split('/').map(enc).join('/'); }

        // ---- lists --------------------------------------------------------------------------------------

        loadFiles() {
            return this.api('GET', '/server/files/directory?path=gcodes&extended=true').then(r => {
                this.files = ((r && r.files) || []).filter(f => /\.(gcode|gco|g|3mf)$/i.test(f.filename)).map(f => ({
                    name: f.filename, path: f.filename, modified: num(f.modified), size: num(f.size), meta: f
                }));
                this._emit('files');
            }).catch(() => { if (!this.files) { this.files = []; this._emit('files'); } });
        }
        loadHistory() {
            return Promise.all([this.api('GET', '/server/history/list?limit=60&order=desc'), this.api('GET', '/server/history/totals').catch(() => null)]).then(res => {
                this.history = { jobs: (res[0] && res[0].jobs) || [], totals: (res[1] && res[1].job_totals) || null };
                this._emit('history');
            }).catch(() => { if (!this.history) { this.history = { jobs: [], totals: null }; this._emit('history'); } });
        }
        loadVideos() {
            return this.api('GET', '/server/files/list?root=timelapse').then(r => {
                this.videos = (r || []).filter(f => /\.mp4$/i.test(f.path)).map(f => ({ name: f.path, modified: num(f.modified), size: num(f.size) }));
                this._emit('videos');
            }).catch(() => { if (!this.videos) { this.videos = []; this._emit('videos'); } });
        }
        deleteFile(path) { return this.api('DELETE', '/server/files/gcodes/' + String(path).split('/').map(enc).join('/')).catch(e => Z.toast(e.message, 'err')); }
        deleteVideo(name) { return this.api('DELETE', '/server/files/timelapse/' + String(name).split('/').map(enc).join('/')).catch(e => Z.toast(e.message, 'err')); }

        // ---- orders ---------------------------------------------------------------------------------------

        setNozzleTemp(v) { return this._order('SET_HEATER_TEMPERATURE HEATER=extruder TARGET=' + Math.round(v), { targetNozzleTemp: v }); }
        setBedTemp(v) { return this._order('SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=' + Math.round(v), { targetBedTemp0: v }); }
        setChamberTemp() { return false; }
        // ZMOD's own light macros (LED_ON / LED_OFF drive SET_LED LED=chamber_led WHITE=1 / 0).
        setLight(on) { return this._order(on ? 'LED_ON' : 'LED_OFF', { lightSw: on ? 1 : 0 }); }
        setFan(key, pct) {
            const obj = this.fans[key];
            if (!obj) return false;
            const patch = { [key]: pct };
            if (obj === 'fan') return this._order('M106 S' + Math.round(pct * 255 / 100), patch);
            return this._order('SET_FAN_SPEED FAN=' + obj.split(' ').pop() + ' SPEED=' + (pct / 100).toFixed(2), patch);
        }
        setSpeed(mode) { return this._order('M220 S' + Math.round(mode), { curFeedratePct: mode }); }
        pause() { return this._order('PAUSE'); }
        resume() { return this._order('RESUME'); }
        stop() { return this._order('CANCEL_PRINT'); }
        home(axes) { return this._order('G28 ' + axes); }
        gcode(cmd) { return this._order(cmd); }
        move(axis, dist) { return this._order('G91\nG1 ' + axis + dist + ' F' + (axis === 'Z' ? 600 : 3000) + '\nG90'); }
        get() { return false; }
        set() { return false; }

        // IFS orders go through ZMOD's macros (the printer asks for the temperature itself).
        ifsLoad(slot) { return this._order('LOAD_MATERIAL T=' + (slot - 1)); }
        ifsUnload() { return this._order('UNLOAD_FILAMENT'); }
        ifsUnlock() { return this._order('IFS_UNLOCK'); }

        // ZMOD's accepted materials (zmod_color.valid_types) and a fresh read of the IFS objects.
        validTypes() {
            const zc = this.raw.zmod_color;
            return zc && Array.isArray(zc.valid_types) ? zc.valid_types.map(String) : [];
        }
        // lane_data (Moonraker DB): Orca stores the extra colours of a dual / tri / quad / gradient slot there.
        async _loadLanes() {
            try {
                const r = await this.api('GET', '/server/database/item?namespace=lane_data');
                this._lanes = (r && r.value && typeof r.value === 'object') ? r.value : null;
            } catch (e) { this._lanes = null; }
        }
        async refreshIfs() {
            try {
                await this._loadLanes();
                const q = await this._query(['zmod_color', 'zmod_ifs', 'save_variables'].filter(k => this._objects.indexOf(k) >= 0));
                Object.assign(this.raw, q);
                this._derive();
            } catch (e) { /* the next poll retries */ }
        }

        // ---- starting a print -----------------------------------------------------------------------------

        // Without lessWaste: Moonraker's own print start. With it: the plugin's dialog flow driven from here.
        async printFile(path) {
            try {
                if (!this.hasLw) { await this.api('POST', '/printer/print/start?filename=' + enc(path)); return true; }
                return await this._printLessWaste(path);
            } catch (e) {
                Z.toast(e.message, 'err');
                return false;
            }
        }

        async _query(keys) {
            const r = await this.api('GET', '/printer/objects/query?' + keys.map(enc).join('&'));
            return (r && r.status) || {};
        }

        // lessWaste's own entry (_ZSDCARD_PRINT_FILE) reads the G-code (tools, colours, types, feed rates, first-layer box) and
        // opens its dialog; Orca then answers it - tool -> IFS slot, IFS on - and presses Print (_IFS_COLORS_PRINT).
        async _printLessWaste(path) {
            const q = s => s.replace(/"/g, '');
            Z.toast(Z.t('lw_preparing'));
            await this.script('SET_GCODE_VARIABLE MACRO=_IFS_VARS VARIABLE=start VALUE=0');
            // the MD5 check can outlast the request: the dialog below is what counts
            await this.script('_ZSDCARD_PRINT_FILE FILENAME="' + q(path) + '"', true).catch(() => {});
            let vars = null, cols = null;
            for (let i = 0; i < 150; i++) {
                await sleep(1000);
                if (this._closed) return false;
                let st;
                try { st = await this._query(['gcode_macro _IFS_VARS', 'gcode_macro _IFS_COLORS']); } catch (e) { continue; }
                vars = st['gcode_macro _IFS_VARS']; cols = st['gcode_macro _IFS_COLORS'];
                if (vars && cols && num(vars.start) === 1 && cols.filename === path) break;
                vars = null;
            }
            if (!vars) throw new Error(Z.t('lw_no_dialog'));
            const tools = (Array.isArray(cols.tools) ? cols.tools : []).map(Number).filter(n => n >= 0 && n < 16);
            const slots = (this.ifs && this.ifs.slots) || [];
            const ports = [1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5];
            const cur = Array.isArray(vars.tools) ? vars.tools.slice(0, 16) : ports;
            const types = Array.isArray(vars.types) ? vars.types : [], colors = Array.isArray(vars.colors) ? vars.colors : [];
            const external = num(vars.external) === 1 && tools.length <= 1;
            const taken = new Set();
            let plan = [];
            if (!external) {
                // keep the plugin's default (T0 -> IFS1 ...) when that slot holds the filament; else the nearest colour of the same type
                tools.forEach(t => {
                    const want = '#' + String(colors[t] || '').replace('#', ''), type = String(types[t] || '').toUpperCase();
                    const ok = s => s && s.present && !taken.has(s.id);
                    let pick = slots.find(s => s.id === (cur[t] || t + 1) && ok(s) && (!type || String(s.material).toUpperCase() === type) && colorDistance(want, s.color) < 60);
                    if (!pick) {
                        pick = slots.filter(s => ok(s) && (!type || String(s.material).toUpperCase() === type))
                            .sort((a, b) => colorDistance(want, a.color) - colorDistance(want, b.color))[0] || slots.filter(ok)[0];
                    }
                    if (!pick) throw new Error(Z.t('lw_no_slot', { tool: 'T' + t }));
                    taken.add(pick.id);
                    ports[t] = pick.id;
                    plan.push('T' + t + '→IFS' + pick.id);
                });
                await this.script('_IFS_VARS tools="[' + ports.join(',') + ']"');
                await this.script('_IFS_VARS external=0');
            }
            if (tools.length > 1 && String(cols.version).split('.').reduce((a, n) => a * 100 + num(n), 0) < String(vars.min_version).split('.').reduce((a, n) => a * 100 + num(n), 0)) {
                await this.script('_IFS_COLORS_CANCEL', true).catch(() => {});
                throw new Error(Z.t('lw_old_gcode', { v: cols.version, min: vars.min_version }));
            }
            await this.script('_IFS_COLORS_PRINT');
            if (plan.length) Z.toast(plan.join('  '));
            return true;
        }
    }

    // Opens the link just long enough to learn status and IFS colours (device list cards).
    function peekMoonraker(dev, timeoutMs) {
        return new Promise(resolve => {
            const link = new MoonrakerLink(dev, { once: true });
            let done = false;
            const finish = () => {
                if (done) return;
                done = true;
                clearTimeout(timer);
                const slots = link.ifs ? link.ifs.slots : [];
                const boxes = slots.length ? { materialBoxs: [{ type: 0, materials: slots.map(s => ({ state: s.present ? 1 : 0, type: s.material, name: s.colorName, color: s.color })) }] } : null;
                const result = { online: link.hasStatus, state: link.state, boxes: boxes };
                link.close();
                resolve(result);
            };
            const timer = setTimeout(finish, timeoutMs || 8000);
            link.on('ready', () => setTimeout(finish, 600));
            link.connect();
        });
    }

    Z.MoonrakerLink = MoonrakerLink;
    Z.peekMoonraker = peekMoonraker;
})(window.Z = window.Z || {});
