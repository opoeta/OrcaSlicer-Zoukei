/*
 * Orca Zoukei: host-side adjustments for the embedded Creality Print device panel.
 *
 * 1. Visual identity: the compiled panel injects its CSS at run time and also sets a few
 *    colours inline. Creality's accent palette is rewritten to Orca's teal as styles and
 *    attributes are added, so the minified bundle stays untouched. Theme variables
 *    (backgrounds, borders, text) are overridden in orca-theme.css.
 * 2. Privacy: fetch / XMLHttpRequest / sendBeacon may only reach this page's own files and
 *    LAN addresses (the printers). Everything else is refused before it leaves the page.
 *
 * Loaded synchronously before the bundle. AGPL-3.0, like the panel it adjusts.
 */
(function () {
    'use strict';

    var ACCENT = '#009688';
    var ACCENT_HOVER = '#26A69A';
    var ACCENT_DARK_BG = '#223C3C';

    var REPLACEMENTS = [
        [/#17cc5f/gi, ACCENT],
        [/#15c059/gi, ACCENT],
        [/#15bf59/gi, ACCENT],
        [/#24c086/gi, ACCENT],
        [/#1fca63/gi, ACCENT_HOVER],
        [/#215335/gi, ACCENT_DARK_BG],
        [/#409eff/gi, ACCENT],
        [/rgba\(\s*23\s*,\s*204\s*,\s*95\s*,/gi, 'rgba(0, 150, 136,'],
        [/rgba\(\s*21\s*,\s*192\s*,\s*89\s*,/gi, 'rgba(0, 150, 136,'],
        [/rgb\(\s*23\s*,\s*204\s*,\s*95\s*\)/gi, 'rgb(0, 150, 136)'],
        [/rgb\(\s*21\s*,\s*192\s*,\s*89\s*\)/gi, 'rgb(0, 150, 136)'],
        [/rgb\(\s*31\s*,\s*202\s*,\s*99\s*\)/gi, 'rgb(38, 166, 154)']
    ];
    var QUICK_TEST = /17cc5f|15c059|15bf59|24c086|1fca63|215335|409eff|23,\s*204,\s*95|21,\s*192,\s*89|31,\s*202,\s*99/i;
    var COLOUR_ATTRS = ['style', 'fill', 'stroke', 'stop-color', 'color'];

    function recolour(text) {
        if (!text || !QUICK_TEST.test(text))
            return text;
        for (var i = 0; i < REPLACEMENTS.length; ++i)
            text = text.replace(REPLACEMENTS[i][0], REPLACEMENTS[i][1]);
        return text;
    }

    function fixStyleElement(el) {
        var text = el.textContent;
        var fixed = recolour(text);
        if (fixed !== text)
            el.textContent = fixed;
    }

    function fixAttributes(el) {
        if (!el.getAttribute)
            return;
        for (var i = 0; i < COLOUR_ATTRS.length; ++i) {
            var value = el.getAttribute(COLOUR_ATTRS[i]);
            if (value) {
                var fixed = recolour(value);
                if (fixed !== value)
                    el.setAttribute(COLOUR_ATTRS[i], fixed);
            }
        }
    }

    function fixTree(node) {
        if (node.nodeType !== 1)
            return;
        if (node.tagName === 'STYLE') {
            fixStyleElement(node);
            return;
        }
        fixAttributes(node);
        if (node.querySelectorAll) {
            var list = node.querySelectorAll('style,[style],[fill],[stroke],[stop-color],[color]');
            for (var i = 0; i < list.length; ++i) {
                if (list[i].tagName === 'STYLE')
                    fixStyleElement(list[i]);
                else
                    fixAttributes(list[i]);
            }
        }
    }

    var observer = new MutationObserver(function (records) {
        for (var i = 0; i < records.length; ++i) {
            var record = records[i];
            if (record.type === 'attributes') {
                fixAttributes(record.target);
            } else if (record.type === 'characterData') {
                var parent = record.target.parentNode;
                if (parent && parent.tagName === 'STYLE')
                    fixStyleElement(parent);
            } else {
                for (var j = 0; j < record.addedNodes.length; ++j) {
                    var added = record.addedNodes[j];
                    if (added.nodeType === 3 && added.parentNode && added.parentNode.tagName === 'STYLE')
                        fixStyleElement(added.parentNode);
                    else
                        fixTree(added);
                }
            }
        }
    });
    observer.observe(document.documentElement, {
        childList: true,
        subtree: true,
        characterData: true,
        attributes: true,
        attributeFilter: COLOUR_ATTRS
    });

    // ---- LAN-only network guard -------------------------------------------------------
    function isPrivateIPv4(host) {
        var m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
        if (!m)
            return false;
        var a = +m[1], b = +m[2];
        return a === 10 || a === 127 || (a === 172 && b >= 16 && b <= 31) ||
               (a === 192 && b === 168) || (a === 169 && b === 254);
    }

    function isAllowedUrl(url) {
        try {
            var parsed = new URL(String(url), window.location.href);
            var scheme = parsed.protocol;
            if (scheme === 'file:' || scheme === 'data:' || scheme === 'blob:' || scheme === 'about:')
                return true;
            if (scheme !== 'http:' && scheme !== 'https:' && scheme !== 'ws:' && scheme !== 'wss:')
                return false;
            var host = parsed.hostname;
            return host === 'localhost' || isPrivateIPv4(host);
        } catch (e) {
            return false;
        }
    }

    function blocked(url) {
        try { console.warn('[orca] blocked non-LAN request: ' + String(url).split('?')[0]); } catch (e) {}
    }

    if (window.fetch) {
        var nativeFetch = window.fetch;
        window.fetch = function (input, init) {
            var url = (input && typeof input === 'object' && 'url' in input) ? input.url : input;
            if (!isAllowedUrl(url)) {
                blocked(url);
                return Promise.reject(new TypeError('Blocked by Orca: non-LAN request'));
            }
            return nativeFetch.apply(this, arguments);
        };
    }

    if (window.XMLHttpRequest) {
        var nativeOpen = XMLHttpRequest.prototype.open;
        XMLHttpRequest.prototype.open = function (method, url) {
            if (!isAllowedUrl(url)) {
                blocked(url);
                throw new TypeError('Blocked by Orca: non-LAN request');
            }
            return nativeOpen.apply(this, arguments);
        };
    }

    if (navigator.sendBeacon) {
        var nativeBeacon = navigator.sendBeacon.bind(navigator);
        navigator.sendBeacon = function (url, data) {
            if (!isAllowedUrl(url)) {
                blocked(url);
                return false;
            }
            return nativeBeacon(url, data);
        };
    }

    if (window.WebSocket) {
        var NativeWebSocket = window.WebSocket;
        var GuardedWebSocket = function (url, protocols) {
            if (!isAllowedUrl(url)) {
                blocked(url);
                throw new SyntaxError('Blocked by Orca: non-LAN WebSocket');
            }
            return protocols === undefined ? new NativeWebSocket(url) : new NativeWebSocket(url, protocols);
        };
        GuardedWebSocket.prototype = NativeWebSocket.prototype;
        GuardedWebSocket.CONNECTING = NativeWebSocket.CONNECTING;
        GuardedWebSocket.OPEN = NativeWebSocket.OPEN;
        GuardedWebSocket.CLOSING = NativeWebSocket.CLOSING;
        GuardedWebSocket.CLOSED = NativeWebSocket.CLOSED;
        window.WebSocket = GuardedWebSocket;
    }
})();
