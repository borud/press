#pragma once

// GPIO assignments for stepper driver (HY DIV-268N-5A)
#define PIN_STEP        18  // RMT TX channel -> STEP input
#define PIN_DIR         19  // Direction output
#define PIN_ENABLE      21  // Active LOW enable

// Button inputs (GPIOs with internal pull-up support)
#define PIN_BTN_FWD     32
#define PIN_BTN_REV     33

// Status LED (onboard)
#define PIN_STATUS_LED  2

// RMT configuration
#define STEP_RESOLUTION_HZ  1000000  // 1 MHz = 1 us tick
#define STEP_PULSE_TICKS    3        // 3 us pulse width (min 2 us for HY DIV-268N-5A)
