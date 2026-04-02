#include "buttons.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pin_defs.h"

static const char* TAG = "buttons";

#define DEBOUNCE_INTERVAL_US 10000  // 10 ms polling interval
#define DEBOUNCE_COUNT 3            // 3 consecutive readings = 30 ms

static struct {
  button_callback_t  on_press;
  button_callback_t  on_release;
  esp_timer_handle_t timer;
  button_state_t     current_state;
  button_state_t     candidate_state;
  int                stable_count;
} s_buttons;

static button_state_t read_buttons(void) {
  // Active LOW: pressed = 0
  if (gpio_get_level(PIN_BTN_FWD) == 0) {
    return BTN_FWD;
  }
  if (gpio_get_level(PIN_BTN_REV) == 0) {
    return BTN_REV;
  }
  return BTN_NONE;
}

static void debounce_timer_cb(void* arg) {
  button_state_t sample = read_buttons();

  // If the current sample matches the candidate state, increments the stability counter.
  // If the sample differs from the candidate state, resets to a new candidate state
  // and initializes the stability counter to 1.
  if (sample == s_buttons.candidate_state) {
    s_buttons.stable_count++;
  } else {
    s_buttons.candidate_state = sample;
    s_buttons.stable_count    = 1;
  }

  if (s_buttons.stable_count >= DEBOUNCE_COUNT && s_buttons.candidate_state != s_buttons.current_state) {
    button_state_t old_state = s_buttons.current_state;
    s_buttons.current_state  = s_buttons.candidate_state;

    // Fire callbacks
    if (old_state != BTN_NONE && s_buttons.on_release) {
      s_buttons.on_release(old_state);
    }
    if (s_buttons.current_state != BTN_NONE && s_buttons.on_press) {
      s_buttons.on_press(s_buttons.current_state);
    }
  }
}

esp_err_t buttons_init(button_callback_t on_press, button_callback_t on_release) {
  s_buttons.on_press        = on_press;
  s_buttons.on_release      = on_release;
  s_buttons.current_state   = BTN_NONE;
  s_buttons.candidate_state = BTN_NONE;
  s_buttons.stable_count    = 0;

  // Configure button GPIOs as inputs (external pull-ups)
  gpio_config_t btn_conf = {
      .pin_bit_mask = (1ULL << PIN_BTN_FWD) | (1ULL << PIN_BTN_REV),
      .mode         = GPIO_MODE_INPUT,
      .pull_up_en   = GPIO_PULLUP_ENABLE,  // Use internal pull-ups
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  esp_err_t ret = gpio_config(&btn_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to configure button GPIOs: %s", esp_err_to_name(ret));
    return ret;
  }

  // Create periodic debounce timer
  esp_timer_create_args_t timer_args = {
      .callback        = debounce_timer_cb,
      .name            = "btn_debounce",
      .dispatch_method = ESP_TIMER_TASK,
  };
  ret = esp_timer_create(&timer_args, &s_buttons.timer);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to create debounce timer: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_timer_start_periodic(s_buttons.timer, DEBOUNCE_INTERVAL_US);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to start debounce timer: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "buttons initialized: FWD=GPIO%d REV=GPIO%d", PIN_BTN_FWD, PIN_BTN_REV);
  return ESP_OK;
}

button_state_t buttons_get_state(void) { return s_buttons.current_state; }
