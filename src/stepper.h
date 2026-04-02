#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STEPPER_DIR_FORWARD = 0,
    STEPPER_DIR_REVERSE = 1,
} stepper_dir_t;

typedef enum {
    STEPPER_STATE_IDLE = 0,
    STEPPER_STATE_ACCELERATING,
    STEPPER_STATE_RUNNING,
    STEPPER_STATE_DECELERATING,
} stepper_state_t;

typedef struct {
    int step_gpio;
    int dir_gpio;
    int enable_gpio;
    uint32_t resolution_hz;  // RMT resolution (1 MHz recommended)
    uint32_t pulse_ticks;    // Pulse high duration in RMT ticks
} stepper_config_t;

typedef struct {
    uint32_t max_speed_hz;    // Target speed (default 2000)
    uint32_t start_speed_hz;  // Ramp start speed (default 100)
    uint32_t accel_steps;     // Steps in acceleration ramp (default 200)
} stepper_motion_params_t;

/**
 * Initialize the stepper driver hardware (RMT channel, GPIO).
 */
esp_err_t stepper_init(const stepper_config_t *config);

/**
 * Enable the stepper driver (active LOW on ENABLE pin).
 */
esp_err_t stepper_enable(void);

/**
 * Disable the stepper driver (releases holding torque).
 */
esp_err_t stepper_disable(void);

/**
 * Set motor direction.
 */
esp_err_t stepper_set_direction(stepper_dir_t dir);

/**
 * Set motion parameters (speed, acceleration).
 */
esp_err_t stepper_set_motion_params(const stepper_motion_params_t *params);

/**
 * Get current motion parameters.
 */
const stepper_motion_params_t *stepper_get_motion_params(void);

/**
 * Run a specific number of steps at the given frequency (no acceleration).
 */
esp_err_t stepper_run_steps(uint32_t steps, uint32_t speed_hz);

/**
 * Run a specific number of steps with S-curve acceleration and deceleration.
 * Phases: accel (accel_steps) -> uniform (remaining) -> decel (accel_steps).
 * If total steps < 2*accel_steps, the ramps are shortened proportionally.
 */
esp_err_t stepper_run_profiled(uint32_t steps);

/**
 * Start continuous stepping with S-curve acceleration.
 * Motor accelerates to max_speed and runs until stepper_ramp_stop() or stepper_stop().
 */
esp_err_t stepper_run_continuous(uint32_t speed_hz);

/**
 * Decelerate to stop using S-curve profile.
 */
esp_err_t stepper_ramp_stop(void);

/**
 * Immediately stop pulse generation (no deceleration).
 */
esp_err_t stepper_stop(void);

/**
 * Check if the stepper is currently running.
 */
bool stepper_is_running(void);

/**
 * Get the current stepper state.
 */
stepper_state_t stepper_get_state(void);

/**
 * Register a callback for when a profiled move completes.
 * Called from the stepper state task context (not ISR).
 */
typedef void (*stepper_move_done_cb_t)(void);
void stepper_set_move_done_callback(stepper_move_done_cb_t cb);
