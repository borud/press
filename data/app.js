(function() {
    'use strict';

    const $ = (sel) => document.querySelector(sel);
    let online = false;
    let stepsPerCm = 1;

    function hzToCmSec(hz) {
        return (hz / stepsPerCm).toFixed(1);
    }

    function setOnline(state) {
        if (state === online) return;
        online = state;
        const el = $('#conn-status');
        el.classList.toggle('online', state);
        el.classList.toggle('offline', !state);
    }

    function post(url, body) {
        if (body) body.t = Date.now();
        return fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: body ? JSON.stringify(body) : ''
        });
    }

    // Move command with abort timeout — prevents queued requests from
    // causing delayed motion.  Stop commands use plain post() instead
    // so they never self-cancel.
    function postMove(action) {
        var ctrl = new AbortController();
        var timer = setTimeout(function() { ctrl.abort(); }, 800);
        var body = { action: action, t: Date.now() };
        return fetch('/api/move', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
            signal: ctrl.signal
        }).finally(function() { clearTimeout(timer); });
    }

    // Activity display update (from SSE or poll fallback)
    function updateActivity(s) {
        const el = $('#activity');
        el.textContent = s.activity;
        el.className = 'activity-text';
        if (s.activity.includes('forward')) el.classList.add('active-fwd');
        else if (s.activity.includes('reverse')) el.classList.add('active-rev');

        if (s.armed !== undefined) {
            const btn = $('#btn-arm');
            btn.textContent = s.armed ? 'ARMED' : 'DISARMED';
            btn.classList.toggle('armed', s.armed);
        }
    }

    // Full status poll (for system info that doesn't come via SSE)
    function updateStatus() {
        fetch('/api/status')
            .then(r => r.json())
            .then(s => {
                setOnline(true);
                updateActivity(s);
                $('#hostname').textContent = 'press.local';
                $('#wifi').textContent = s.wifi_connected ? s.ip : 'disconnected';
                $('#heap').textContent = Math.round(s.free_heap / 1024) + ' KB';
            })
            .catch(() => {
                setOnline(false);
            });
    }

    // SSE for real-time activity updates
    function connectSSE() {
        const es = new EventSource('/api/events');
        es.onopen = function() {
            setOnline(true);
            loadConfig();
            updateStatus();
        };
        es.onmessage = function(e) {
            try {
                updateActivity(JSON.parse(e.data));
            } catch (_) {}
        };
        es.onerror = function() {
            es.close();
            setOnline(false);
            setTimeout(connectSSE, 2000);
        };
    }

    function loadConfig() {
        fetch('/api/config')
            .then(r => r.json())
            .then(c => {
                if (c.steps_per_cm) stepsPerCm = c.steps_per_cm;
                $('#max-speed').value = c.max_speed_hz;
                $('#max-speed-val').textContent = hzToCmSec(c.max_speed_hz);
                $('#start-speed').value = c.start_speed_hz;
                $('#start-speed-val').textContent = hzToCmSec(c.start_speed_hz);
                $('#accel-steps').value = c.accel_steps;
                $('#accel-steps-val').textContent = c.accel_steps;
                $('#move-distance').value = c.move_distance_cm;
                $('#microsteps').value = c.microsteps;
                if (c.log_level !== undefined) {
                    $('#log-level').value = c.log_level;
                }
            })
            .catch(() => {});
    }

    function loadFirmware() {
        fetch('/api/firmware')
            .then(r => r.json())
            .then(f => {
                $('#fw-version').textContent = f.version;
            })
            .catch(() => {});
    }

    // Fixed-distance move buttons — single click
    $('#btn-move-fwd').addEventListener('click', () => {
        postMove('move-fwd');
    });
    $('#btn-move-rev').addEventListener('click', () => {
        postMove('move-rev');
    });

    // Jog buttons — hold to move, release anywhere to stop.
    // Sends keepalive pings every 200ms; firmware auto-stops after 500ms without one.
    let jogActive = null;       // null or action string
    let jogKeepaliveId = null;  // setInterval id

    function jogStart(action) {
        if (jogActive) return;
        jogActive = action;
        postMove(action);
        jogKeepaliveId = setInterval(function() {
            postMove('jog-keepalive');
        }, 200);
    }

    function jogStop() {
        if (!jogActive) return;
        jogActive = null;
        if (jogKeepaliveId !== null) {
            clearInterval(jogKeepaliveId);
            jogKeepaliveId = null;
        }
        post('/api/move', { action: 'jog-stop' })
            .catch(function() {
                post('/api/move', { action: 'jog-stop' });
            });
    }

    function setupJog(btnId, action) {
        var btn = $(btnId);
        btn.addEventListener('mousedown', function() { jogStart(action); });
        btn.addEventListener('touchstart', function(e) { e.preventDefault(); jogStart(action); });
    }

    setupJog('#btn-jog-fwd', 'jog-fwd');
    setupJog('#btn-jog-rev', 'jog-rev');

    document.addEventListener('mouseup', jogStop);
    document.addEventListener('touchend', jogStop);
    document.addEventListener('touchcancel', jogStop);
    window.addEventListener('blur', jogStop);
    document.addEventListener('visibilitychange', function() {
        if (document.hidden) jogStop();
    });

    // ARM button — toggle motor enable
    $('#btn-arm').addEventListener('click', () => {
        const isArmed = $('#btn-arm').classList.contains('armed');
        post('/api/arm', { armed: !isArmed });
    });

    // Stop button — emergency stop
    $('#btn-stop').addEventListener('click', () => {
        post('/api/move', { action: 'stop' });
    });

    // Sliders — update display live, apply to controller on release
    ['max-speed', 'start-speed', 'accel-steps'].forEach(id => {
        const keyMap = { 'max-speed': 'max_speed_hz', 'start-speed': 'start_speed_hz', 'accel-steps': 'accel_steps' };
        $('#' + id).addEventListener('input', function() {
            if (id === 'accel-steps') {
                $('#' + id + '-val').textContent = this.value;
            } else {
                $('#' + id + '-val').textContent = hzToCmSec(this.value);
            }
        });
        $('#' + id).addEventListener('change', function() {
            post('/api/config', { [keyMap[id]]: parseInt(this.value) });
        });
    });

    // Microsteps — apply immediately, then reload config to update steps_per_cm
    $('#microsteps').addEventListener('change', function() {
        post('/api/config', { microsteps: parseInt(this.value) })
            .then(function() { loadConfig(); });
    });

    // Move distance — apply immediately
    $('#move-distance').addEventListener('change', function() {
        post('/api/config', { move_distance_cm: parseFloat(this.value) });
    });

    // Log level — apply immediately
    $('#log-level').addEventListener('change', function() {
        post('/api/config', { log_level: parseInt(this.value) });
    });

    // Save params — persist current config to flash
    $('#btn-save-params').addEventListener('click', () => {
        post('/api/config/save');
    });

    // Init
    loadConfig();
    loadFirmware();
    connectSSE();
    updateStatus();
    // Slow poll for system info (heap, wifi) — also detects reconnection
    setInterval(updateStatus, 5000);
})();
