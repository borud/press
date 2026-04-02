(function() {
    'use strict';

    const $ = (sel) => document.querySelector(sel);

    function post(url, body) {
        return fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: body ? JSON.stringify(body) : ''
        });
    }

    // Status polling
    function updateStatus() {
        fetch('/api/status')
            .then(r => r.json())
            .then(s => {
                const el = $('#activity');
                el.textContent = s.activity;
                el.className = 'activity-text';
                if (s.activity.includes('forward')) el.classList.add('active-fwd');
                else if (s.activity.includes('reverse')) el.classList.add('active-rev');

                $('#hostname').textContent = 'press.local';
                $('#wifi').textContent = s.wifi_connected ? s.ip : 'disconnected';
                $('#heap').textContent = Math.round(s.free_heap / 1024) + ' KB';
            })
            .catch(() => {});
    }

    function loadConfig() {
        fetch('/api/config')
            .then(r => r.json())
            .then(c => {
                $('#max-speed').value = c.max_speed_hz;
                $('#max-speed-val').textContent = c.max_speed_hz;
                $('#start-speed').value = c.start_speed_hz;
                $('#start-speed-val').textContent = c.start_speed_hz;
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
                $('#fw-version').textContent = 'v' + f.version;
            })
            .catch(() => {});
    }

    // Fixed-distance move buttons — single click
    $('#btn-move-fwd').addEventListener('click', () => {
        post('/api/move', { action: 'move-fwd' });
    });
    $('#btn-move-rev').addEventListener('click', () => {
        post('/api/move', { action: 'move-rev' });
    });

    // Jog buttons — hold to move
    function setupJog(btnId, action) {
        const btn = $(btnId);
        const start = () => post('/api/move', { action: action });
        const stop = () => post('/api/move', { action: 'jog-stop' });

        btn.addEventListener('mousedown', start);
        btn.addEventListener('mouseup', stop);
        btn.addEventListener('mouseleave', stop);
        btn.addEventListener('touchstart', (e) => { e.preventDefault(); start(); });
        btn.addEventListener('touchend', (e) => { e.preventDefault(); stop(); });
    }

    setupJog('#btn-jog-fwd', 'jog-fwd');
    setupJog('#btn-jog-rev', 'jog-rev');

    // Stop button — emergency stop
    $('#btn-stop').addEventListener('click', () => {
        post('/api/move', { action: 'stop' });
    });

    // Slider live display
    ['max-speed', 'start-speed', 'accel-steps'].forEach(id => {
        $('#' + id).addEventListener('input', function() {
            $('#' + id + '-val').textContent = this.value;
        });
    });

    // Save distance on change (immediate, no save button needed)
    $('#move-distance').addEventListener('change', function() {
        post('/api/config', { move_distance_cm: parseFloat(this.value) });
    });

    // Log level — apply immediately
    $('#log-level').addEventListener('change', function() {
        post('/api/config', { log_level: parseInt(this.value) });
    });

    // Save params
    $('#btn-save-params').addEventListener('click', () => {
        post('/api/config', {
            max_speed_hz: parseInt($('#max-speed').value),
            start_speed_hz: parseInt($('#start-speed').value),
            accel_steps: parseInt($('#accel-steps').value),
            microsteps: parseInt($('#microsteps').value)
        }).then(() => loadConfig());
    });

    // Init
    loadConfig();
    loadFirmware();
    setInterval(updateStatus, 200);
    updateStatus();
})();
