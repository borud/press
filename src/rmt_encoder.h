#pragma once

#include "driver/rmt_encoder.h"
#include "esp_err.h"

/**
 * Configuration for the uniform (constant-speed) stepper encoder.
 */
typedef struct {
    uint32_t resolution_hz;  // RMT channel resolution (e.g. 1 MHz)
    uint32_t freq_hz;        // Step frequency in Hz
    uint32_t pulse_ticks;    // Pulse high duration in RMT ticks
} stepper_uniform_encoder_config_t;

/**
 * Create a uniform stepper encoder that produces constant-frequency step pulses.
 */
esp_err_t rmt_new_stepper_uniform_encoder(const stepper_uniform_encoder_config_t *config,
                                          rmt_encoder_handle_t *ret_encoder);

/**
 * Set the number of steps to encode before completion.
 */
void stepper_uniform_encoder_set_steps(rmt_encoder_handle_t encoder, uint32_t steps);

/**
 * Configuration for the S-curve acceleration encoder.
 */
typedef struct {
    uint32_t resolution_hz;   // RMT channel resolution
    uint32_t start_freq_hz;   // Starting frequency (e.g. 100 Hz)
    uint32_t target_freq_hz;  // Target frequency (e.g. 2000 Hz)
    uint32_t accel_steps;     // Number of steps for the ramp
    uint32_t pulse_ticks;     // Pulse high duration in RMT ticks
    bool     reverse;         // true = deceleration (target -> start)
} stepper_scurve_encoder_config_t;

/**
 * Create an S-curve encoder using smoothstep: f(t) = 3t^2 - 2t^3.
 * Produces accel_steps pulses with frequencies interpolated along the curve.
 * If reverse=true, frequencies go from target_freq down to start_freq (decel).
 */
esp_err_t rmt_new_stepper_scurve_encoder(const stepper_scurve_encoder_config_t *config,
                                         rmt_encoder_handle_t *ret_encoder);
