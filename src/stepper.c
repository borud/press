#include "stepper.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pin_defs.h"
#include "rmt_encoder.h"

static const char* TAG = "stepper";

static struct {
  rmt_channel_handle_t     rmt_channel;
  rmt_encoder_handle_t     accel_encoder;
  rmt_encoder_handle_t     uniform_encoder;
  rmt_encoder_handle_t     decel_encoder;
  stepper_config_t         config;
  stepper_motion_params_t  motion_params;
  volatile stepper_state_t state;
  volatile bool            enabled;
  volatile bool            profiled_move;  // true = fixed-step move with accel/decel
  volatile uint32_t        uniform_steps;  // steps for uniform phase in profiled move
  stepper_move_done_cb_t   move_done_cb;
  SemaphoreHandle_t        done_sem;
  SemaphoreHandle_t        api_mutex;  // serializes public API calls across tasks
  TaskHandle_t             state_task;
} s_stepper;

static esp_err_t create_encoders(void);
static void      delete_encoders(void);

// Called from ISR when RMT transmission completes
static bool IRAM_ATTR on_trans_done(
    rmt_channel_handle_t channel, const rmt_tx_done_event_data_t* edata, void* user_ctx) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (s_stepper.state == STEPPER_STATE_DECELERATING) {
    s_stepper.state = STEPPER_STATE_IDLE;
  }

  xSemaphoreGiveFromISR(s_stepper.done_sem, &xHigherPriorityTaskWoken);
  return xHigherPriorityTaskWoken == pdTRUE;
}

// Helper function to start RMT transmission and handle errors
static esp_err_t start_transmission(rmt_encoder_handle_t encoder, stepper_state_t new_state) {
  rmt_encoder_reset(encoder);
  rmt_transmit_config_t tx_config = {.loop_count = 0};
  uint32_t              dummy     = 0;
  esp_err_t             ret       = rmt_transmit(s_stepper.rmt_channel, encoder, &dummy, sizeof(dummy), &tx_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to start transmission for state %d: %s", new_state, esp_err_to_name(ret));
    s_stepper.state = STEPPER_STATE_IDLE;
    return ret;
  }
  s_stepper.state = new_state;
  return ESP_OK;
}

// Helper function to handle profiled move completion
static void handle_profiled_move_completion(void) {
  if (s_stepper.state == STEPPER_STATE_IDLE && s_stepper.profiled_move) {
    s_stepper.profiled_move = false;
    delete_encoders();
    esp_err_t ret = create_encoders();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "failed to restore encoders after profiled move: %s", esp_err_to_name(ret));
    }
    if (s_stepper.move_done_cb) {
      s_stepper.move_done_cb();
    }
  }
}

// Helper function to handle state transitions
static void handle_state_transition(void) {
  switch (s_stepper.state) {
    case STEPPER_STATE_ACCELERATING:
      if (s_stepper.profiled_move && s_stepper.uniform_steps == 0) {
        start_transmission(s_stepper.decel_encoder, STEPPER_STATE_DECELERATING);
        return;
      }
      // Transition to uniform running
      uint32_t steps = s_stepper.profiled_move ? s_stepper.uniform_steps : UINT32_MAX;
      stepper_uniform_encoder_set_steps(s_stepper.uniform_encoder, steps);
      start_transmission(s_stepper.uniform_encoder, STEPPER_STATE_RUNNING);
      return;

    case STEPPER_STATE_RUNNING:
      if (!s_stepper.profiled_move) {
        return;  // No action needed for continuous running
      }
      // Uniform phase of profiled move completed — start decel
      start_transmission(s_stepper.decel_encoder, STEPPER_STATE_DECELERATING);
      return;

    default:
      if (s_stepper.profiled_move) {
        handle_profiled_move_completion();
        return;
      }
      ESP_LOGI(TAG, "deceleration complete, stepper stopped");
      return;
  }
}

// Task that manages state transitions for continuous mode
static void stepper_state_task(void* arg) {
  esp_err_t wdt_ret = esp_task_wdt_add(NULL);
  if (wdt_ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to subscribe stepper_state_task to TWDT: %s", esp_err_to_name(wdt_ret));
  }

  while (true) {
    // Wait for a transmission to complete (use timeout so we can feed the watchdog)
    if (xSemaphoreTake(s_stepper.done_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
      handle_state_transition();
    }
    esp_task_wdt_reset();
  }
}

static esp_err_t create_encoders(void) {
  // Accel encoder
  stepper_scurve_encoder_config_t accel_cfg = {
      .resolution_hz  = s_stepper.config.resolution_hz,
      .start_freq_hz  = s_stepper.motion_params.start_speed_hz,
      .target_freq_hz = s_stepper.motion_params.max_speed_hz,
      .accel_steps    = s_stepper.motion_params.accel_steps,
      .pulse_ticks    = s_stepper.config.pulse_ticks,
      .reverse        = false,
  };
  ESP_RETURN_ON_ERROR(
      rmt_new_stepper_scurve_encoder(&accel_cfg, &s_stepper.accel_encoder), TAG, "failed to create accel encoder");

  // Uniform encoder
  stepper_uniform_encoder_config_t uniform_cfg = {
      .resolution_hz = s_stepper.config.resolution_hz,
      .freq_hz       = s_stepper.motion_params.max_speed_hz,
      .pulse_ticks   = s_stepper.config.pulse_ticks,
  };
  ESP_RETURN_ON_ERROR(rmt_new_stepper_uniform_encoder(&uniform_cfg, &s_stepper.uniform_encoder),
      TAG,
      "failed to create uniform encoder");

  // Decel encoder
  stepper_scurve_encoder_config_t decel_cfg = {
      .resolution_hz  = s_stepper.config.resolution_hz,
      .start_freq_hz  = s_stepper.motion_params.start_speed_hz,
      .target_freq_hz = s_stepper.motion_params.max_speed_hz,
      .accel_steps    = s_stepper.motion_params.accel_steps,
      .pulse_ticks    = s_stepper.config.pulse_ticks,
      .reverse        = true,
  };
  ESP_RETURN_ON_ERROR(
      rmt_new_stepper_scurve_encoder(&decel_cfg, &s_stepper.decel_encoder), TAG, "failed to create decel encoder");

  return ESP_OK;
}

static void delete_encoders(void) {
  if (s_stepper.accel_encoder) {
    rmt_del_encoder(s_stepper.accel_encoder);
    s_stepper.accel_encoder = NULL;
  }
  if (s_stepper.uniform_encoder) {
    rmt_del_encoder(s_stepper.uniform_encoder);
    s_stepper.uniform_encoder = NULL;
  }
  if (s_stepper.decel_encoder) {
    rmt_del_encoder(s_stepper.decel_encoder);
    s_stepper.decel_encoder = NULL;
  }
}

esp_err_t stepper_init(const stepper_config_t* config) {
  ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

  s_stepper.config = *config;
  s_stepper.state  = STEPPER_STATE_IDLE;

  // Default motion parameters
  s_stepper.motion_params.max_speed_hz   = 800;
  s_stepper.motion_params.start_speed_hz = 100;
  s_stepper.motion_params.accel_steps    = 200;

  s_stepper.done_sem = xSemaphoreCreateBinary();
  ESP_RETURN_ON_FALSE(s_stepper.done_sem, ESP_ERR_NO_MEM, TAG, "failed to create semaphore");

  s_stepper.api_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_stepper.api_mutex, ESP_ERR_NO_MEM, TAG, "failed to create API mutex");

  // Configure DIR pin
  gpio_config_t dir_conf = {
      .pin_bit_mask = (1ULL << config->dir_gpio),
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&dir_conf), TAG, "failed to configure DIR gpio %d", config->dir_gpio);

  // Configure ENABLE pin (start enabled)
  gpio_config_t en_conf = {
      .pin_bit_mask = (1ULL << config->enable_gpio),
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&en_conf), TAG, "failed to configure ENABLE gpio %d", config->enable_gpio);
  gpio_set_level(config->enable_gpio, config->invert_signals ? 1 : 0);  // Disabled (active LOW, inverted if ULN2003)
  s_stepper.enabled = true;

  // Create RMT TX channel on STEP pin
  rmt_tx_channel_config_t rmt_config = {
      .gpio_num          = config->step_gpio,
      .clk_src           = RMT_CLK_SRC_DEFAULT,
      .resolution_hz     = config->resolution_hz,
      .mem_block_symbols = 64,
      .trans_queue_depth = 4,
  };
  ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&rmt_config, &s_stepper.rmt_channel),
      TAG,
      "failed to create RMT TX channel on gpio %d",
      config->step_gpio);

  // Register transmission-done callback
  rmt_tx_event_callbacks_t cbs = {
      .on_trans_done = on_trans_done,
  };
  ESP_RETURN_ON_ERROR(rmt_tx_register_event_callbacks(s_stepper.rmt_channel, &cbs, NULL),
      TAG,
      "failed to register RMT event callbacks");

  // Enable the RMT channel
  ESP_RETURN_ON_ERROR(rmt_enable(s_stepper.rmt_channel), TAG, "failed to enable RMT channel");

  // Create the encoders
  ESP_RETURN_ON_ERROR(create_encoders(), TAG, "failed to create encoders");

  // Create the state management task
  BaseType_t task_ret = xTaskCreate(stepper_state_task, "stepper_state", 4096, NULL, 5, &s_stepper.state_task);
  ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create stepper state task");

  ESP_LOGI(TAG,
      "stepper initialized: STEP=%d DIR=%d EN=%d res=%luHz invert=%s",
      config->step_gpio,
      config->dir_gpio,
      config->enable_gpio,
      (unsigned long)config->resolution_hz,
      config->invert_signals ? "yes" : "no");
  ESP_LOGI(TAG,
      "motion params: start=%luHz max=%luHz accel_steps=%lu",
      (unsigned long)s_stepper.motion_params.start_speed_hz,
      (unsigned long)s_stepper.motion_params.max_speed_hz,
      (unsigned long)s_stepper.motion_params.accel_steps);

  return ESP_OK;
}

esp_err_t stepper_enable(void) {
  gpio_set_level(s_stepper.config.enable_gpio, s_stepper.config.invert_signals ? 1 : 0);
  s_stepper.enabled = true;
  ESP_LOGD(TAG, "stepper enabled");
  return ESP_OK;
}

esp_err_t stepper_disable(void) {
  gpio_set_level(s_stepper.config.enable_gpio, s_stepper.config.invert_signals ? 0 : 1);
  s_stepper.enabled = false;
  ESP_LOGD(TAG, "stepper disabled");
  return ESP_OK;
}

bool stepper_is_enabled(void) { return s_stepper.enabled; }

esp_err_t stepper_set_direction(stepper_dir_t dir) {
  uint32_t level = (uint32_t)dir ^ (s_stepper.config.invert_signals ? 1 : 0);
  gpio_set_level(s_stepper.config.dir_gpio, level);
  ESP_LOGD(TAG, "direction set to %s", dir == STEPPER_DIR_FORWARD ? "FWD" : "REV");
  return ESP_OK;
}

esp_err_t stepper_set_motion_params(const stepper_motion_params_t* params) {
  ESP_RETURN_ON_FALSE(params, ESP_ERR_INVALID_ARG, TAG, "params is NULL");
  ESP_RETURN_ON_FALSE(params->max_speed_hz > params->start_speed_hz,
      ESP_ERR_INVALID_ARG,
      TAG,
      "max_speed (%lu) must be > start_speed (%lu)",
      (unsigned long)params->max_speed_hz,
      (unsigned long)params->start_speed_hz);
  ESP_RETURN_ON_FALSE(params->accel_steps > 0, ESP_ERR_INVALID_ARG, TAG, "accel_steps must be > 0");

  xSemaphoreTake(s_stepper.api_mutex, portMAX_DELAY);

  if (s_stepper.state != STEPPER_STATE_IDLE) {
    ESP_LOGW(TAG, "cannot change motion params while running");
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  s_stepper.motion_params = *params;

  // Recreate encoders with new params
  delete_encoders();
  esp_err_t ret = create_encoders();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to recreate encoders: %s", esp_err_to_name(ret));
    xSemaphoreGive(s_stepper.api_mutex);
    return ret;
  }

  ESP_LOGI(TAG,
      "motion params updated: start=%luHz max=%luHz accel_steps=%lu",
      (unsigned long)params->start_speed_hz,
      (unsigned long)params->max_speed_hz,
      (unsigned long)params->accel_steps);
  xSemaphoreGive(s_stepper.api_mutex);
  return ESP_OK;
}

const stepper_motion_params_t* stepper_get_motion_params(void) { return &s_stepper.motion_params; }

esp_err_t stepper_run_steps(uint32_t steps) {
  ESP_RETURN_ON_FALSE(steps > 0, ESP_ERR_INVALID_ARG, TAG, "steps must be > 0");

  xSemaphoreTake(s_stepper.api_mutex, portMAX_DELAY);

  if (s_stepper.state != STEPPER_STATE_IDLE) {
    ESP_LOGW(TAG, "stepper not idle, ignoring run_steps");
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  // For simple step runs, use the uniform encoder directly
  stepper_uniform_encoder_set_steps(s_stepper.uniform_encoder, steps);
  rmt_encoder_reset(s_stepper.uniform_encoder);

  rmt_transmit_config_t tx_config = {.loop_count = 0};
  uint32_t              dummy     = 0;

  s_stepper.state = STEPPER_STATE_RUNNING;
  esp_err_t ret   = rmt_transmit(s_stepper.rmt_channel, s_stepper.uniform_encoder, &dummy, sizeof(dummy), &tx_config);
  if (ret != ESP_OK) {
    s_stepper.state = STEPPER_STATE_IDLE;
    ESP_LOGE(TAG, "failed to start RMT transmission: %s", esp_err_to_name(ret));
    xSemaphoreGive(s_stepper.api_mutex);
    return ret;
  }

  ESP_LOGI(
      TAG, "running %lu steps at %luHz", (unsigned long)steps, (unsigned long)s_stepper.motion_params.max_speed_hz);
  xSemaphoreGive(s_stepper.api_mutex);
  return ESP_OK;
}

esp_err_t stepper_run_profiled(uint32_t steps) {
  ESP_RETURN_ON_FALSE(steps > 0, ESP_ERR_INVALID_ARG, TAG, "steps must be > 0");

  xSemaphoreTake(s_stepper.api_mutex, portMAX_DELAY);

  if (s_stepper.state != STEPPER_STATE_IDLE) {
    ESP_LOGW(TAG, "stepper not idle, ignoring run_profiled");
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t accel_steps = s_stepper.motion_params.accel_steps;
  uint32_t decel_steps = accel_steps;

  // If total steps can't fit full accel + decel, split evenly
  if (steps < accel_steps + decel_steps) {
    accel_steps = steps / 2;
    decel_steps = steps - accel_steps;
  }

  uint32_t uniform_steps = steps - accel_steps - decel_steps;

  // Recreate accel/decel encoders if ramp was shortened
  if (accel_steps != s_stepper.motion_params.accel_steps) {
    if (s_stepper.accel_encoder) rmt_del_encoder(s_stepper.accel_encoder);
    if (s_stepper.decel_encoder) rmt_del_encoder(s_stepper.decel_encoder);

    stepper_scurve_encoder_config_t accel_cfg = {
        .resolution_hz  = s_stepper.config.resolution_hz,
        .start_freq_hz  = s_stepper.motion_params.start_speed_hz,
        .target_freq_hz = s_stepper.motion_params.max_speed_hz,
        .accel_steps    = accel_steps,
        .pulse_ticks    = s_stepper.config.pulse_ticks,
        .reverse        = false,
    };
    esp_err_t ret = rmt_new_stepper_scurve_encoder(&accel_cfg, &s_stepper.accel_encoder);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "failed to create shortened accel encoder: %s", esp_err_to_name(ret));
      xSemaphoreGive(s_stepper.api_mutex);
      return ret;
    }

    stepper_scurve_encoder_config_t decel_cfg = {
        .resolution_hz  = s_stepper.config.resolution_hz,
        .start_freq_hz  = s_stepper.motion_params.start_speed_hz,
        .target_freq_hz = s_stepper.motion_params.max_speed_hz,
        .accel_steps    = decel_steps,
        .pulse_ticks    = s_stepper.config.pulse_ticks,
        .reverse        = true,
    };
    ret = rmt_new_stepper_scurve_encoder(&decel_cfg, &s_stepper.decel_encoder);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "failed to create shortened decel encoder: %s", esp_err_to_name(ret));
      xSemaphoreGive(s_stepper.api_mutex);
      return ret;
    }
  }

  s_stepper.profiled_move = true;
  s_stepper.uniform_steps = uniform_steps;

  // Start with acceleration phase
  rmt_encoder_reset(s_stepper.accel_encoder);
  rmt_transmit_config_t tx_config = {.loop_count = 0};
  uint32_t              dummy     = 0;

  s_stepper.state = STEPPER_STATE_ACCELERATING;
  esp_err_t ret   = rmt_transmit(s_stepper.rmt_channel, s_stepper.accel_encoder, &dummy, sizeof(dummy), &tx_config);
  if (ret != ESP_OK) {
    s_stepper.state = STEPPER_STATE_IDLE;
    ESP_LOGE(TAG, "failed to start profiled move: %s", esp_err_to_name(ret));
    xSemaphoreGive(s_stepper.api_mutex);
    return ret;
  }

  ESP_LOGI(TAG,
      "profiled move: %lu steps (accel=%lu uniform=%lu decel=%lu)",
      (unsigned long)steps,
      (unsigned long)accel_steps,
      (unsigned long)uniform_steps,
      (unsigned long)decel_steps);
  xSemaphoreGive(s_stepper.api_mutex);
  return ESP_OK;
}

esp_err_t stepper_run_continuous(void) {
  xSemaphoreTake(s_stepper.api_mutex, portMAX_DELAY);

  if (s_stepper.state != STEPPER_STATE_IDLE) {
    ESP_LOGW(TAG, "stepper not idle, ignoring run_continuous");
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  s_stepper.profiled_move = false;

  // Start with acceleration phase
  rmt_encoder_reset(s_stepper.accel_encoder);

  rmt_transmit_config_t tx_config = {.loop_count = 0};
  uint32_t              dummy     = 0;

  s_stepper.state = STEPPER_STATE_ACCELERATING;
  esp_err_t ret   = rmt_transmit(s_stepper.rmt_channel, s_stepper.accel_encoder, &dummy, sizeof(dummy), &tx_config);
  if (ret != ESP_OK) {
    s_stepper.state = STEPPER_STATE_IDLE;
    ESP_LOGE(TAG, "failed to start acceleration: %s", esp_err_to_name(ret));
    xSemaphoreGive(s_stepper.api_mutex);
    return ret;
  }

  ESP_LOGI(
      TAG, "starting continuous run with S-curve accel to %luHz", (unsigned long)s_stepper.motion_params.max_speed_hz);
  xSemaphoreGive(s_stepper.api_mutex);
  return ESP_OK;
}

// Abort current RMT transmission and start deceleration.
// Caller must hold api_mutex.
static esp_err_t abort_and_decelerate(void) {
  ESP_RETURN_ON_ERROR(rmt_disable(s_stepper.rmt_channel), TAG, "failed to disable RMT");
  ESP_RETURN_ON_ERROR(rmt_enable(s_stepper.rmt_channel), TAG, "failed to re-enable RMT");

  // Drain any semaphore given by the aborted transmission's ISR
  xSemaphoreTake(s_stepper.done_sem, 0);

  s_stepper.state = STEPPER_STATE_DECELERATING;
  rmt_encoder_reset(s_stepper.decel_encoder);

  rmt_transmit_config_t tx_config = {.loop_count = 0};
  uint32_t              dummy     = 0;
  ESP_RETURN_ON_ERROR(rmt_transmit(s_stepper.rmt_channel, s_stepper.decel_encoder, &dummy, sizeof(dummy), &tx_config),
      TAG,
      "failed to start deceleration");

  ESP_LOGI(TAG, "decelerating to stop");
  return ESP_OK;
}

esp_err_t stepper_ramp_stop(void) {
  xSemaphoreTake(s_stepper.api_mutex, portMAX_DELAY);

  if (s_stepper.state == STEPPER_STATE_IDLE) {
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_OK;
  }

  if (s_stepper.state == STEPPER_STATE_DECELERATING) {
    ESP_LOGD(TAG, "already decelerating");
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_OK;
  }

  // Both ACCELERATING and RUNNING: abort current transmission, start decel
  esp_err_t ret = abort_and_decelerate();
  xSemaphoreGive(s_stepper.api_mutex);
  return ret;
}

esp_err_t stepper_stop(void) {
  xSemaphoreTake(s_stepper.api_mutex, portMAX_DELAY);

  if (s_stepper.state == STEPPER_STATE_IDLE) {
    xSemaphoreGive(s_stepper.api_mutex);
    return ESP_OK;
  }

  esp_err_t ret = rmt_disable(s_stepper.rmt_channel);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to disable RMT: %s", esp_err_to_name(ret));
    xSemaphoreGive(s_stepper.api_mutex);
    return ret;
  }
  ret = rmt_enable(s_stepper.rmt_channel);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to re-enable RMT: %s", esp_err_to_name(ret));
    xSemaphoreGive(s_stepper.api_mutex);
    return ret;
  }
  // Drain any semaphore given by the aborted transmission's ISR
  xSemaphoreTake(s_stepper.done_sem, 0);
  s_stepper.state = STEPPER_STATE_IDLE;
  ESP_LOGI(TAG, "stepper stopped (immediate)");
  xSemaphoreGive(s_stepper.api_mutex);
  return ESP_OK;
}

bool stepper_is_running(void) { return s_stepper.state != STEPPER_STATE_IDLE || s_stepper.profiled_move; }

stepper_state_t stepper_get_state(void) { return s_stepper.state; }

void stepper_set_move_done_callback(stepper_move_done_cb_t cb) { s_stepper.move_done_cb = cb; }
