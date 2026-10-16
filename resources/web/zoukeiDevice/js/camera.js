// Camera: WebRTC to the printer. The browser builds the offer; the host posts it to the printer
// (the page may not POST to a LAN address) and returns the printer's answer. Printers without
// WebRTC serve an MJPEG stream on port 8080, shown in an <img>.
(function (Z) {
    'use strict';

    const { h, icon } = Z;

    class Camera {
        constructor(dev, link) {
            this.dev = dev;
            this.link = link;
            this.pc = null;
            this.run = 0;               // bumps on every (re)start so late answers are ignored
            this.retries = 0;
            this.video = h('video', { autoplay: true, muted: true, playsinline: true });
            this.video.muted = true;
            this.img = h('img', { alt: '' });
            this.msg = h('div.cam-msg');
            this.el = h('div.cam',
                this.video, this.img, this.msg,
                h('button.icon-btn.cam-btn', { type: 'button', title: Z.t('camera_reconnect'), onclick: () => this.start() }, icon('refresh')));
            this.img.hidden = true;
        }

        status(text) { this.msg.textContent = text || ''; this.msg.hidden = !text; }

        start() {
            this.stop(true);
            const run = ++this.run;
            this.status(Z.t('camera_connecting'));
            if (this.link.hasStatus) this._begin(run); else this._waitReady(run);
        }

        _waitReady(run) {
            const off = this.link.on('ready', () => { off(); if (run === this.run) this._begin(run); });
        }

        stop(keepRun) {
            if (!keepRun) this.run++;
            clearTimeout(this._retry);
            if (this.pc) { try { this.pc.close(); } catch (e) { /* closed */ } this.pc = null; }
            this.video.srcObject = null;
            this.img.removeAttribute('src');
            this.img.hidden = true;
            this.video.hidden = false;
        }

        _begin(run) {
            const s = this.link.state;
            if (!Number(s.webrtcSupport)) {
                // old firmware: plain MJPEG
                this.video.hidden = true;
                this.img.hidden = false;
                this.img.onload = () => this.status('');
                this.img.onerror = () => this.status(Z.t('camera_unavailable'));
                this.img.src = 'http://' + this.dev.address + ':8080/?action=stream';
                return;
            }
            const encrypted = Array.isArray(s.features) && s.features.indexOf('videoInfo.videoEncryption') >= 0;
            if (!encrypted) return this._offer(run, false, '');
            // encrypted video needs the printer's short-lived token
            let given = false;
            const go = () => { if (!given && run === this.run) { given = true; this._offer(run, true, this.link.token); } };
            if (this.link.token) return go();
            const off = this.link.on('token', () => { off(); go(); });
            this.link.get({ getToken: 1 });
            setTimeout(() => { off(); go(); }, 3000);
        }

        async _offer(run, encrypted, token) {
            try {
                const pc = new RTCPeerConnection({ iceServers: [] });
                this.pc = pc;
                pc.ontrack = ev => {
                    if (run !== this.run) return;
                    this.video.srcObject = ev.streams && ev.streams[0] ? ev.streams[0] : new MediaStream([ev.track]);
                    this.video.play().catch(() => {});
                };
                pc.oniceconnectionstatechange = () => {
                    if (run !== this.run) return;
                    const st = pc.iceConnectionState;
                    if (st === 'connected' || st === 'completed') { this.retries = 0; this.status(''); }
                    else if (st === 'failed' || st === 'disconnected') this._again(run, Z.t('camera_lost'));
                };
                pc.addTransceiver('video', { direction: 'sendrecv' });
                await pc.setLocalDescription(await pc.createOffer());
                await new Promise(resolve => {         // wait for the candidates: the printer does not trickle
                    if (pc.iceGatheringState === 'complete') return resolve();
                    const t = setTimeout(resolve, 3000);
                    pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') { clearTimeout(t); resolve(); } };
                });
                if (run !== this.run) return;
                const url = encrypted ?
                    'https://' + this.dev.address + ':' + (this.dev.videoPort || 443) + '/call/webrtc_local' :
                    'http://' + this.dev.address + ':8000/call/webrtc_local';
                const answer = await Z.Host.request('get_webrtc_local_param',
                    { url: url, sdp: pc.localDescription.sdp, videoEncryption: encrypted, token: token || '' },
                    'get_webrtc_local_param', encrypted ? 40000 : 20000);
                if (run !== this.run) return;
                if (!answer.sdp) throw new Error(answer.error || 'no answer');
                const desc = JSON.parse(atob(answer.sdp));
                await pc.setRemoteDescription(new RTCSessionDescription(desc));
            } catch (e) {
                if (run === this.run) this._again(run, Z.t('camera_unavailable'));
            }
        }

        _again(run, text) {
            this.status(text);
            if (this.retries >= 4) return;
            this.retries++;
            clearTimeout(this._retry);
            this._retry = setTimeout(() => { if (run === this.run) this.start(); }, 5000);
        }
    }

    Z.Camera = Camera;
})(window.Z = window.Z || {});
