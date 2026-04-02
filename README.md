# Press

Firmware for a printing press stepper drive. Uses an ESP32 to control a
Nema 23 stepper motor via an HY DIV-268N-5A driver, through a ~3:1
gearbox.

Two physical buttons jog the motor forward and reverse. Once on WiFi
there's a web UI for control and configuration. The device is
reachable at `press.local` via mDNS.

## Wiring

### Stepper driver (HY DIV-268N-5A)

| GPIO | Driver pin | Notes                        |
|------|------------|------------------------------|
| 18   | STEP       | RMT output, 3 us pulse width |
| 19   | DIR        | Direction                    |
| 21   | ENA        | Active LOW                   |

### Buttons

| GPIO | Function |
|------|----------|
| 32   | Forward  |
| 33   | Reverse  |

Wire each button between GPIO and GND. Internal pull-ups are enabled
in firmware.

GPIO 2 is reserved for a status LED (onboard).

## Build

You need [PlatformIO](https://platformio.org/). Everything goes through
the Makefile. Run `make` with no arguments to list all targets.

    make build           # build firmware
    make upload          # flash firmware
    make uploadfs        # flash web UI (LittleFS)
    make upload-all      # flash everything
    make monitor         # serial console (115200)
    make erase           # wipe flash
    make fullclean       # nuke build artifacts + sdkconfig

## WiFi provisioning

First boot starts BLE provisioning. Use the ESP BLE Provisioning app
to set WiFi credentials:

- [Android](https://play.google.com/store/apps/details?id=com.espressif.provble)
- [iOS](https://apps.apple.com/app/esp-ble-provisioning/id1473590141)

The proof of possession code is printed on the serial console.

## Web interface

Once the device is on WiFi, open `http://press.local` in a browser.
The UI lets you:

- Jog the motor forward/reverse (hold the button to move)
- Run fixed-distance moves (single click)
- Emergency stop
- Adjust max speed, start speed, acceleration, microsteps, and
  move distance
- Change the ESP log level at runtime

The web files live in the `data/` directory and are flashed to a
LittleFS partition with `make uploadfs`.

## Web API

REST endpoints:

    GET  /api/status       motor state, wifi, heap
    GET  /api/config       motion parameters
    POST /api/config       update speed, accel, etc
    POST /api/move         jog/move commands
    GET  /api/firmware     build date and IDF version
