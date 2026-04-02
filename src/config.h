#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Mechanical constants
#define STEPPER_TEETH      12
#define ROLLER_TEETH       46
#define ROLLER_DIAMETER_MM 65.2f
#define STEPS_PER_REV      200  // Full steps per revolution (Nema 23)

typedef struct {
    uint32_t max_speed_hz;
    uint32_t accel_steps;
    uint32_t start_speed_hz;
    float    move_distance_cm;  // Distance for fixed moves (default 10.0)
    uint16_t microsteps;        // Microstepping divisor: 1, 2, 4, 8, 16, etc. (default 1)
    uint8_t  log_level;         // Global log level (esp_log_level_t): 0=NONE .. 5=VERBOSE (default 3=INFO)
} press_config_t;

/**
 * Initialize the NVS-backed configuration system.
 * Must be called after nvs_flash_init().
 */
esp_err_t config_init(void);

/**
 * Load configuration from NVS into the provided struct.
 */
esp_err_t config_load(press_config_t *cfg);

/**
 * Save configuration to NVS.
 */
esp_err_t config_save(const press_config_t *cfg);

/**
 * Get pointer to the live config.
 */
const press_config_t *config_get(void);

/**
 * Lock config for modification. Returns pointer to mutable config.
 * Must call config_unlock() after changes.
 */
press_config_t *config_lock(void);

/**
 * Unlock config after modification.
 */
void config_unlock(void);

/**
 * Convert centimeters to motor steps based on current microstepping and gear ratio.
 */
uint32_t config_cm_to_steps(float cm);

/**
 * Convert motor steps to centimeters.
 */
float config_steps_to_cm(uint32_t steps);
