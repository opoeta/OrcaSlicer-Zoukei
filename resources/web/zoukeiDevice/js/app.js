// Start-up and navigation: device list <-> printer screen, driven by what the host sends.
(function (Z) {
    'use strict';

    const root = document.getElementById('app');
    const app = { devices: [], list: null, detail: null, current: null };
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

    Z.Host.on('device_list', data => {
        app.devices = (data && data.devices) || [];
        app.list.setDevices(app.devices);
        if (app.current) {
            const still = app.devices.find(d => d.address === app.current.address);
            if (!still) app.back();
            else app.current = still;
        }
    });

    // The host tells when the Device tab is shown or hidden: no camera or polling while hidden.
    Z.Host.on('panel_visible', visible => {
        if (visible) {
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
