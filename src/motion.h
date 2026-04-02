#pragma once

#include "buttons.h"
#include "esp_err.h"
#include <stdint.h>

/**
 * Initialize the motion controller.
 * Must be called after stepper_init() and config_init().
 */
esp_err_t motion_init(void);

/**
 * Get a human-readable activity status string.
 * Returns one of: "idle", "jog forward", "jog reverse",
 *                 "move forward", "move reverse"
 */
const char *motion_get_activity(void);

/**
 * Start jogging in the given direction. Motor runs until motion_jog_stop().
 */
esp_err_t motion_jog_start(button_state_t dir);

/**
 * Stop jogging with S-curve deceleration.
 */
esp_err_t motion_jog_stop(void);

/**
 * Move a fixed number of steps with S-curve profile (non-blocking).
 * Positive = forward, negative = reverse.
 */
esp_err_t motion_move_steps(int32_t steps);

/**
 * Move a fixed distance in centimeters (non-blocking).
 * Positive = forward, negative = reverse.
 * Uses config's microstepping and gear ratio for conversion.
 */
esp_err_t motion_move_cm(float cm);

/**
 * Emergency stop — immediate halt, no deceleration.
 */
esp_err_t motion_stop(void);

/**
 * Handle physical button press (called from button callback context).
 * FWD/REV = jog while held.
 */
void motion_on_button_press(button_state_t btn);

/**
 * Handle physical button release.
 */
void motion_on_button_release(button_state_t btn);
