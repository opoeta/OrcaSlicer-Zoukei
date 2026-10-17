// One printer, one WebSocket. The printer pushes its status as JSON objects with only the keys
// that changed; requests are {"method":"get","params":{...}} and orders {"method":"set",...}.
// Message shapes: see painel-k2-protocolo.md of the Orca Zoukei research notes.
(function (Z) {
    'use strict';

    const { num } = Z.fmt;

    function socketUrl(dev) {
        // The host swaps WebSocket for printer addresses, so TLS to the printer's own CA works.
        return dev.secure ? 'wss://' + dev.address + ':' + (dev.wssPort || 443) : 'ws://' + dev.address + ':9999';
    }

    class PrinterLink {
        constructor(dev, options) {
            this.dev = dev;
            this.options = options || {};
            this.state = {};          // merged status keys
            this.boxes = null;        // boxsInfo: {materialBoxs: [...], ...}
            this.boxConfig = null;
            this.files = null;        // retGcodeFileInfo2
            this.videos = null;       // elapseVideoList (timelapse)
            this.history = null;      // {totalJob, totalUsageTime, totalUsageMaterial, historyList}
            this.library = null;      // retMaterials, reduced to what slot editing needs
            this.token = '';          // videoToken
            this.online = false;
            this.hasStatus = false;
            this._events = {};
            this._closed = false;
            this._attempt = 0;
            this._ws = null;
        }

        on(name, fn) {
            (this._events[name] = this._events[name] || []).push(fn);
            return () => { this._events[name] = this._events[name].filter(f => f !== fn); };
        }

        _emit(name, a, b) {
            (this._events[name] || []).slice().forEach(fn => { try { fn(a, b); } catch (e) { console.error(e); } });
        }

        connect() {
            if (this._closed) return;
            let ws;
            try {
                ws = new WebSocket(socketUrl(this.dev));
            } catch (e) {
                this._scheduleReconnect();
                return;
            }
            this._ws = ws;
            this._armTimer(10000);                       // connection timeout
            ws.onopen = () => {
                if (this._ws !== ws) return;
                this._attempt = 0;
                this.online = true;
                this._armTimer(30000);                   // the printer talks every second or so
                this._emit('online', true);
                this.get({ boxsInfo: 1 });
                this.get({ boxConfig: 1 });
            };
            ws.onmessage = e => { if (this._ws === ws) this._message(e.data); };
            ws.onerror = () => {};
            ws.onclose = () => {
                if (this._ws !== ws) return;
                this._ws = null;
                clearTimeout(this._timer);
                if (this.online) { this.online = false; this._emit('online', false); }
                this._scheduleReconnect();
            };
        }

        _armTimer(ms) {
            clearTimeout(this._timer);
            this._timer = setTimeout(() => { try { this._ws && this._ws.close(); } catch (e) { /* already closed */ } }, ms);
        }

        _scheduleReconnect() {
            if (this._closed || this.options.once) return;
            const delay = Math.min(2000 * Math.pow(2, this._attempt++), 15000);
            this._retry = setTimeout(() => this.connect(), delay);
        }

        close() {
            this._closed = true;
            clearTimeout(this._timer);
            clearTimeout(this._retry);
            const ws = this._ws;
            this._ws = null;
            try { ws && ws.close(); } catch (e) { /* already closed */ }
            if (this.online) { this.online = false; this._emit('online', false); }
        }

        _raw(text) {
            try { this._ws && this._ws.readyState === 1 && this._ws.send(text); } catch (e) { /* closed */ }
        }

        send(method, params) {
            if (!this._ws || this._ws.readyState !== 1) return false;
            this._raw(JSON.stringify({ method: method, params: params }));
            return true;
        }
        get(params) { return this.send('get', params); }

        // An order. `patch` is applied to the local status at once so the controls answer
        // immediately; the printer's own push overwrites it a moment later.
        set(params, patch) {
            const ok = this.send('set', params);
            if (ok && patch) this._merge(patch);
            return ok;
        }

        _merge(obj) {
            Object.assign(this.state, obj);
            this._emit('state', obj);
        }

        _message(raw) {
            this._armTimer(30000);
            if (typeof raw !== 'string' || raw === 'ok') return;
            let msg;
            try { msg = JSON.parse(raw); } catch (e) {
                if (raw.indexOf('heart_beat') >= 0) this._raw('ok');
                return;
            }
            if (!msg || typeof msg !== 'object' || Array.isArray(msg)) return;
            if ('heart_beat' in msg) this._raw('ok');

            if (msg.boxsInfo) { this.boxes = msg.boxsInfo; this._emit('boxes'); return; }
            if (msg.boxState && typeof msg.boxState === 'object') {   // one unit's climate / state
                const b = this.boxes && (this.boxes.materialBoxs || []).find(x => x.id === msg.boxState.id);
                if (b) { Object.assign(b, msg.boxState); this._emit('boxes'); }
                return;
            }
            if (msg.boxConfig) { this.boxConfig = msg.boxConfig; this._emit('boxconfig'); return; }
            if (msg.elapseVideoList) { this.videos = msg.elapseVideoList; this._emit('videos'); return; }
            if (msg.retGcodeFileInfo2) { this.files = msg.retGcodeFileInfo2; this._emit('files'); return; }
            if (msg.historyList) { this.history = msg; this._emit('history'); return; }
            if (msg.retMaterials) { this.library = reduceLibrary(msg.retMaterials); this._emit('library'); return; }
            if (msg.videoToken) { this.token = msg.videoToken; this._emit('token'); return; }
            if ('current_object' in msg || 'excluded_objects' in msg || 'objects' in msg) return;

            const first = !this.hasStatus;
            this.hasStatus = true;
            this._merge(msg);
            if (first) this._emit('ready');
        }

        // ---- orders (all "set"; the callers decide when they are allowed) -----------------------

        setNozzleTemp(v) { return this.set({ nozzleTempControl: v }, { targetNozzleTemp: v }); }
        setBedTemp(v) { return this.set({ bedTempControl: { num: 0, val: v } }, { targetBedTemp0: v }); }
        setChamberTemp(v) { return this.set({ boxTempControl: v }, { targetBoxTemp: v }); }
        // AI / video switches: the key is the status key (aiSw, aiDetection, aiFirstFloor, aiPausePrint, materialDetect, videoElapse).
        setAi(key, on) { const p = { [key]: on ? 1 : 0 }; return this.set(p, p); }
        setLight(on) { return this.set({ lightSw: on ? 1 : 0 }, { lightSw: on ? 1 : 0 }); }
        // key: modelFanPct | caseFanPct | auxiliaryFanPct. The K2 takes the fans as M106 (P0 model,
        // P1 case, P2 side; S 0-255); only the F022 accepts the plain percentage for the model fan.
        setFan(key, pct) {
            const p = { modelFanPct: 0, caseFanPct: 1, auxiliaryFanPct: 2 }[key];
            const patch = { [key]: pct };
            if (key === 'modelFanPct' && this.state.model === 'F022') return this.set(patch, patch);
            return this.set({ gcodeCmd: 'M106 P' + p + ' S' + Math.round(pct * 255 / 100) }, patch);
        }
        // Auto refill keeps the other CFS options as the printer reported them.
        setAutoRefill(on) {
            const c = this.boxConfig || {};
            this.boxConfig = Object.assign({}, c, { autoRefill: on ? 1 : 0 });
            this._emit('boxconfig');
            return this.set({ boxConfig: { autoRefill: on ? 1 : 0, cAutoFeed: c.cAutoFeed || 0, cSelfTest: c.cSelfTest || 0 } });
        }
        deleteVideo(name) { return this.set({ ctrlVideoFiles: { cmd: 'remove', printId: '', file: name } }); }
        // mode: 'silent' | 50 | 100 | 125
        setSpeed(mode) {
            if (mode === 'silent') return this.set({ speedMode: 1 }, { speedMode: 1 });
            return this.set({ speedMode: 0, setFeedratePct: mode }, { speedMode: 0, curFeedratePct: mode });
        }
        pause() { return this.set({ pause: 1 }); }
        resume() { return this.set({ pause: 0 }); }
        stop() { return this.set({ stop: 1 }); }
        home(axes) { return this.set({ autohome: axes }); }      // "X Y" | "Z"
        gcode(cmd) { return this.set({ gcodeCmd: cmd }); }
        move(axis, dist) { return this.set({ setPosition: axis + dist + (axis === 'Z' ? ' F600' : ' F3000') }); }
        feed(boxId, slotId, feedIn) { return this.set({ feedInOrOut: { boxId: boxId, materialId: slotId, isFeed: feedIn ? 1 : 0 } }); }
        refreshRfid(boxId, slotId) { return this.set({ refreshBox: { boxId: boxId, materialId: slotId } }); }
        modifyMaterial(boxId, slotId, spec) {
            const clear = !spec;
            spec = spec || {};
            return this.set({
                modifyMaterial: {
                    boxId: boxId, id: slotId,
                    rfid: clear ? '' : (spec.rfid || ''),
                    type: clear ? '' : spec.type, vendor: clear ? '' : spec.vendor, name: clear ? '' : spec.name,
                    color: clear ? '' : Z.fmt.colorToPrinter(spec.color),
                    minTemp: clear ? 0 : spec.minTemp + 1e-8, maxTemp: clear ? 0 : spec.maxTemp + 1e-8,   // keeps them floats
                    pressure: clear ? 0 : (spec.pressure || 0)
                }
            });
        }
        printFile(path) { return this.set({ opGcodeFile: 'printprt:' + path, enableSelfTest: 0 }); }
        deleteFile(path) { return this.set({ opGcodeFile: 'deleteprt:' + path }); }
    }

    // The printer's material library is large (hundreds of entries with colour lists and slicer
    // keys); only the identity and the temperature range matter for writing a slot.
    function reduceLibrary(list) {
        const out = [];
        (Array.isArray(list) ? list : []).forEach(item => {
            const b = item && item.base, kv = (item && item.kvParam) || {};
            if (!b || !b.id || !b.name) return;
            out.push({
                id: String(b.id), brand: b.brand || '', name: b.name,
                type: b.meterialType || b.materialType || kv.filament_type || '',
                minTemp: num(b.minTemp, num(kv.nozzle_temperature_range_low)),
                maxTemp: num(b.maxTemp, num(kv.nozzle_temperature_range_high)),
                pressure: num(kv.pressure_advance)
            });
        });
        return out;
    }

    // Plain-language reading of the merged status.
    function derive(s) {
        const code = Number(s.state);
        const phase = code === 1 ? 'printing' : code === 5 ? 'paused' : code === 6 ? 'pausing' :
            code === 2 ? 'done' : code === 4 ? 'stopped' : code === 8 ? 'restoring' : 'idle';
        const busy = phase === 'printing' || phase === 'paused' || phase === 'pausing' || phase === 'restoring' || Number(s.deviceState) === 1;
        let progress = num(s.printProgress);
        if (phase === 'idle') progress = 0;
        if (phase === 'done') progress = 100;
        // multi-colour jobs: filament change / loading while no print is running
        const feeding = [0, 3, 14].indexOf(Number(s.feedState || 0)) < 0;
        return { phase: phase, busy: busy, progress: Math.max(0, Math.min(100, progress)), feeding: feeding };
    }

    // Opens a socket just long enough to learn status and CFS (device list cards).
    // Resolves {online, state, boxes}; never rejects.
    function peek(dev, timeoutMs) {
        return new Promise(resolve => {
            const link = new PrinterLink(dev, { once: true });
            let done = false;
            const finish = () => {
                if (done) return;
                done = true;
                clearTimeout(timer);
                const result = { online: link.hasStatus || !!link.boxes, state: link.state, boxes: link.boxes };
                link.close();
                resolve(result);
            };
            const timer = setTimeout(finish, timeoutMs || 7000);
            link.on('boxes', () => { if (link.hasStatus) finish(); });
            link.on('ready', () => { if (link.boxes) finish(); else setTimeout(finish, 2500); });
            link.connect();
        });
    }

    Z.PrinterLink = PrinterLink;
    Z.derive = derive;
    Z.peek = peek;
})(window.Z = window.Z || {});
