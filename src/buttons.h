#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    BTN_NONE = 0,
    BTN_FWD,
    BTN_REV,
} button_state_t;

typedef void (*button_callback_t)(button_state_t state);

/**
 * Initialize button input with debounced polling.
 * Buttons use external pull-ups and are active LOW.
 *
 * @param on_press  Called when a button is pressed (from timer ISR context)
 * @param on_release Called when a button is released (from timer ISR context)
 */
esp_err_t buttons_init(button_callback_t on_press, button_callback_t on_release);

/**
 * Get the current debounced button state.
 */
button_state_t buttons_get_state(void);
