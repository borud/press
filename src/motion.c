#include "motion.h"
#include "config.h"
#include "stepper.h"
#include "webserver.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>

static const char *TAG = "motion";

#define POSITION_UPDATE_INTERVAL_US  50000  // 50ms

typedef enum {
    ACTIVITY_IDLE = 0,
    ACTIVITY_JOG_FWD,
    ACTIVITY_JOG_REV,
    ACTIVITY_MOVE_FWD,
    ACTIVITY_MOVE_REV,
} activity_t;

static struct {
    volatile activity_t activity;
    int64_t jog_start_time_us;
    esp_timer_handle_t pos_timer;
} s_motion;

// Called from stepper state task when a profiled move completes
static void on_move_done(void)
{
    s_motion.activity = ACTIVITY_IDLE;
    webserver_broadcast_status();
    ESP_LOGI(TAG, "move complete");
}

// Dummy position timer callback (kept for potential future use)
static void position_update_cb(void *arg)
{
    (void)arg;
}

esp_err_t motion_init(void)
{
    s_motion.activity = ACTIVITY_IDLE;

    const press_config_t *cfg = config_get();

    stepper_motion_params_t params = {
        .max_speed_hz = cfg->max_speed_hz,
        .start_speed_hz = cfg->start_speed_hz,
        .accel_steps = cfg->accel_steps,
    };
    esp_err_t ret = stepper_set_motion_params(&params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to set motion params: %s", esp_err_to_name(ret));
        return ret;
    }

    stepper_set_move_done_callback(on_move_done);

    esp_timer_create_args_t timer_args = {
        .callback = position_update_cb,
        .name = "pos_update",
    };
    ret = esp_timer_create(&timer_args, &s_motion.pos_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create position timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "motion initialized");
    return ESP_OK;
}

const char *motion_get_activity(void)
{
    switch (s_motion.activity) {
    case ACTIVITY_JOG_FWD:  return "jog forward";
    case ACTIVITY_JOG_REV:  return "jog reverse";
    case ACTIVITY_MOVE_FWD: return "move forward";
    case ACTIVITY_MOVE_REV: return "move reverse";
    default:                return "idle";
    }
}

esp_err_t motion_jog_start(button_state_t dir)
{
    // Wait for any ongoing deceleration to finish
    int wait_ms = 0;
    while (stepper_is_running() && wait_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (stepper_is_running()) {
        ESP_LOGW(TAG, "jog: stepper still running after 2s, forcing stop");
        stepper_stop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    stepper_dir_t sdir = (dir == BTN_FWD) ? STEPPER_DIR_FORWARD : STEPPER_DIR_REVERSE;
    s_motion.activity = (dir == BTN_FWD) ? ACTIVITY_JOG_FWD : ACTIVITY_JOG_REV;

    if (!stepper_is_enabled()) {
        ESP_LOGW(TAG, "jog: stepper not armed");
        s_motion.activity = ACTIVITY_IDLE;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = stepper_set_direction(sdir);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jog: failed to set direction: %s", esp_err_to_name(ret));
        s_motion.activity = ACTIVITY_IDLE;
        return ret;
    }

    ret = stepper_run_continuous(config_get()->max_speed_hz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jog: failed to start: %s", esp_err_to_name(ret));
        s_motion.activity = ACTIVITY_IDLE;
        return ret;
    }

    webserver_broadcast_status();
    return ESP_OK;
}

esp_err_t motion_jog_stop(void)
{
    s_motion.activity = ACTIVITY_IDLE;
    webserver_broadcast_status();
    return stepper_ramp_stop();
}

esp_err_t motion_move_steps(int32_t steps)
{
    if (steps == 0) {
        return ESP_OK;
    }

    if (stepper_is_running()) {
        ESP_LOGW(TAG, "move: stepper busy");
        return ESP_ERR_INVALID_STATE;
    }

    stepper_dir_t dir = (steps > 0) ? STEPPER_DIR_FORWARD : STEPPER_DIR_REVERSE;
    s_motion.activity = (steps > 0) ? ACTIVITY_MOVE_FWD : ACTIVITY_MOVE_REV;

    if (!stepper_is_enabled()) {
        ESP_LOGW(TAG, "move: stepper not armed");
        s_motion.activity = ACTIVITY_IDLE;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = stepper_set_direction(dir);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "move: failed to set direction: %s", esp_err_to_name(ret));
        s_motion.activity = ACTIVITY_IDLE;
        return ret;
    }

    ret = stepper_run_profiled((uint32_t)abs(steps));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "move: failed to start: %s", esp_err_to_name(ret));
        s_motion.activity = ACTIVITY_IDLE;
        return ret;
    }

    webserver_broadcast_status();
    ESP_LOGI(TAG, "moving %ld steps", (long)steps);
    return ESP_OK;
}

esp_err_t motion_move_cm(float cm)
{
    uint32_t steps = config_cm_to_steps(cm > 0 ? cm : -cm);
    int32_t signed_steps = (cm > 0) ? (int32_t)steps : -(int32_t)steps;
    ESP_LOGI(TAG, "move %.1f cm = %ld steps", cm, (long)signed_steps);
    return motion_move_steps(signed_steps);
}

esp_err_t motion_stop(void)
{
    s_motion.activity = ACTIVITY_IDLE;
    esp_err_t ret = stepper_stop();
    webserver_broadcast_status();
    return ret;
}

void motion_on_button_press(button_state_t btn)
{
    if (!stepper_is_enabled()) {
        return;
    }
    if (btn == BTN_FWD || btn == BTN_REV) {
        motion_jog_start(btn);
    }
}

void motion_on_button_release(button_state_t btn)
{
    if (s_motion.activity == ACTIVITY_JOG_FWD || s_motion.activity == ACTIVITY_JOG_REV) {
        motion_jog_stop();
    }
}
