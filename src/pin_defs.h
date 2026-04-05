#pragma once

// GPIO assignments for stepper driver (HY DIV-268N-5A) via ULN2003 inverting buffer.
// The ULN2003 is an open-collector inverter with 10k pull-downs on its inputs.
//
//   ESP32 GPIO  ->  ULN2003 input  ->  ULN2003 output  ->  Driver signal
//   GPIO 18     ->  pin 2          ->  pin 15           ->  PUL- (step)
//   GPIO 19     ->  pin 3          ->  pin 14           ->  DIR-
//   GPIO 21     ->  pin 1          ->  pin 16           ->  EN-
//
// Because the ULN2003 inverts, set stepper_config_t.invert_signals = false
// (the driver's active-LOW inputs cancel with the ULN2003 inversion).
#define PIN_STEP        18  // RMT TX channel -> ULN2003 -> PUL-
#define PIN_DIR         19  // Direction       -> ULN2003 -> DIR-
#define PIN_ENABLE      21  // Enable          -> ULN2003 -> EN-

// Button inputs (GPIOs with internal pull-up support)
#define PIN_BTN_FWD     25
#define PIN_BTN_REV     33

// Status LED (onboard)
#define PIN_STATUS_LED  2

// RMT configuration
#define STEP_RESOLUTION_HZ  1000000  // 1 MHz = 1 us tick
#define STEP_PULSE_TICKS    10       // 10 us pulse width (allows for ULN2003 propagation delay)
