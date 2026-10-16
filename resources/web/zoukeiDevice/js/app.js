// Start-up and navigation: device list <-> printer screen, driven by what the host sends.
(function (Z) {
    'use strict';

    const root = document.getElementById('app');
    const app = { devices: [], list: null, detail: null, current: null, currentMac: '' };
    Z.app = app;

    function build() {
        root.replaceChildren();
        app.list = Z.createList();
        app.detail = Z.createDetail();
        root.appendChild(app.list.el);
        root.appendChild(app.detail.el);
        app.detail.el.hidden = true;
        app.list.setDevices(app.devices);
    }

    app.open = function (dev) {
        app.current = dev;
        Z.Host.send('device_select', { mac: dev.mac });
        app.list.deactivate();
        app.list.el.hidden = true;
        app.detail.el.hidden = false;
        app.detail.open(dev);
    };

    app.back = function () {
        app.current = null;
        app.detail.close();
        app.detail.el.hidden = true;
        app.list.el.hidden = false;
        app.list.activate();
    };

    // The Device tab opens straight on the printer Orca is using (e.g. right after sending a print);
    // the list stays one click away through the back button.
    function openCurrent() {
        if (app.current || !app.detail) return;
        const mac = (app.currentMac || '').toUpperCase();
        const dev = app.devices.find(d => (d.mac || '').toUpperCase() === mac) ||
                    (app.devices.length === 1 ? app.devices[0] : null);
        if (dev) app.open(dev);
    }

    Z.Host.on('device_list', data => {
        const first = app.devices.length === 0;
        app.devices = (data && data.devices) || [];
        if (data && typeof data.current === 'string') app.currentMac = data.current;
        app.list.setDevices(app.devices);
        if (first) openCurrent();
        if (app.current) {
            const still = app.devices.find(d => d.address === app.current.address);
            if (!still) app.back();
            else app.current = still;
        }
    });

    // The host tells when the Device tab is shown or hidden: no camera or polling while hidden.
    Z.Host.on('panel_visible', visible => {
        if (visible) {
            openCurrent();
            if (app.current) app.detail.resume(); else app.list.activate();
        } else {
            app.detail.suspend();
            app.list.deactivate();
        }
    });

    // The language arrives from the host a moment after start; texts are built after that.
    let started = false;
    function start() {
        if (started) return;
        started = true;
        build();
        app.list.activate();
        Z.Host.send('device_list');
    }
    Z.onLanguage(() => {
        if (!started) { start(); return; }
        const reopen = app.current;
        app.detail.close();
        build();
        if (reopen) app.open(reopen); else app.list.activate();
    });
    setTimeout(start, 400);       // no host (plain browser): default language
})(window.Z = window.Z || {});
