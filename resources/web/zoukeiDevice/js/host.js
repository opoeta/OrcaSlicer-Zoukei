// Bridge to the Orca host (C++). The page sends {command, ...} through window.wx.postMessage
// and the host answers by calling window.handleStudioCmd(encodeURIComponent(JSON{command, data})).
// Printer WebSockets are opened by the host as well (a page script swaps window.WebSocket for
// printer addresses), so link.js just uses `new WebSocket(...)`.
(function (Z) {
    'use strict';

    const params = new URLSearchParams(location.search);
    const listeners = {};
    let requestSeq = 0;

    Z.backend = params.get('backend') === 'moonraker' ? 'moonraker' : 'creality';
    Z.family = params.get('family') === 'u1' ? 'u1' : '';   // Moonraker printer family: '' (Flashforge / generic) or 'u1' (Snapmaker U1)

    const Host = {
        // Port of the host's local image relay (printer pictures), 0 when unavailable.
        imagePort: Number(params.get('port')) || 0,

        send(command, payload) {
            const message = Object.assign({ command: command }, payload || {});
            try {
                window.wx.postMessage(JSON.stringify(message));
                return true;
            } catch (e) {
                console.warn('host unavailable:', command);
                return false;
            }
        },

        // Subscribes to a host answer; returns the unsubscribe function.
        on(command, fn) {
            (listeners[command] = listeners[command] || []).push(fn);
            return () => { listeners[command] = (listeners[command] || []).filter(f => f !== fn); };
        },

        // Sends a command carrying a requestId and resolves with the host's answer of the same
        // requestId (answers come back as `answerCommand`).
        request(command, payload, answerCommand, timeoutMs) {
            return new Promise((resolve, reject) => {
                const id = 'r' + (++requestSeq) + '-' + Date.now().toString(36);
                const off = Host.on(answerCommand || command, data => {
                    if (!data || data.requestId !== id) return;
                    clearTimeout(timer);
                    off();
                    resolve(data);
                });
                const timer = setTimeout(() => { off(); reject(new Error('timeout')); }, timeoutMs || 20000);
                if (!Host.send(command, Object.assign({ requestId: id }, payload || {}))) {
                    clearTimeout(timer);
                    off();
                    reject(new Error('host unavailable'));
                }
            });
        },

        // URL of a picture on a printer, served by the host's relay on 127.0.0.1.
        imageUrl(dev, path, version) {
            if (!Host.imagePort || !dev || !path) return '';
            const q = new URLSearchParams({ address: dev.address, path: path, secure: dev.secure ? '1' : '0' });
            if (version) q.set('v', String(version));
            return 'http://127.0.0.1:' + Host.imagePort + '/device_image?' + q.toString();
        }
    };

    window.handleStudioCmd = function (encoded) {
        let message;
        try { message = JSON.parse(decodeURIComponent(encoded)); } catch (e) { return; }
        if (!message || !message.command) return;
        (listeners[message.command] || []).slice().forEach(fn => {
            try { fn(message.data, message); } catch (e) { console.error(e); }
        });
    };

    // ---- theme and language, both owned by Orca --------------------------------------------

    function applyTheme(dark) {
        document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
    }
    applyTheme(params.get('dark') !== '0');
    Host.on('is_dark_theme', applyTheme);          // also fired by the host on a live theme switch
    Host.send('is_dark_theme');

    Host.on('get_lang', code => Z.setLanguage(code));
    Host.send('get_lang');

    Z.Host = Host;
})(window.Z = window.Z || {});
