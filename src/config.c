#include "config.h"

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NVS_NAMESPACE    "press_cfg"
#define KEY_CONFIG_BLOB  "cfg_v4"

// Old keys to clean up on boot
static const char *s_stale_keys[] = {
    "cfg_v1", "cfg_v2", "cfg_v3",
    "max_spd", "start_spd", "accel_st",
    "endpt_a", "endpt_b", "endpt_set",
    "cfg_blob",
};

static struct {
    press_config_t cfg;
    SemaphoreHandle_t mutex;
    bool initialized;
} s_config;

static void set_defaults(press_config_t *cfg)
{
    cfg->max_speed_hz   = CONFIG_PRESS_DEFAULT_MAX_SPEED_HZ;
    cfg->start_speed_hz = CONFIG_PRESS_DEFAULT_START_SPEED_HZ;
    cfg->accel_steps    = CONFIG_PRESS_DEFAULT_ACCEL_STEPS;
    cfg->move_distance_cm = CONFIG_PRESS_DEFAULT_MOVE_DISTANCE_MM / 10.0f;
    cfg->microsteps     = CONFIG_PRESS_DEFAULT_MICROSTEPS;
    cfg->log_level      = ESP_LOG_INFO;
}

esp_err_t config_init(void)
{
    s_config.mutex = xSemaphoreCreateMutex();
    if (!s_config.mutex) {
        ESP_LOGE(TAG, "failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    set_defaults(&s_config.cfg);

    esp_err_t ret = config_load(&s_config.cfg);
    switch (ret) {
    case ESP_OK:
        break;
    case ESP_ERR_NVS_NOT_FOUND:
        ESP_LOGI(TAG, "no saved config found, using defaults");
        ret = config_save(&s_config.cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "failed to save default config: %s", esp_err_to_name(ret));
        }
        break;
    default:
        ESP_LOGW(TAG, "error loading config (%s), using defaults", esp_err_to_name(ret));
        set_defaults(&s_config.cfg);
        break;
    }

    // Erase stale config keys from previous versions
    nvs_handle_t cleanup;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &cleanup) == ESP_OK) {
        for (int i = 0; i < sizeof(s_stale_keys) / sizeof(s_stale_keys[0]); i++) {
            nvs_erase_key(cleanup, s_stale_keys[i]);
        }
        nvs_commit(cleanup);
        nvs_close(cleanup);
    }

    s_config.initialized = true;
    ESP_LOGI(TAG, "config: max_speed=%lu start_speed=%lu accel_steps=%lu distance=%.1fcm microsteps=%u",
             (unsigned long)s_config.cfg.max_speed_hz,
             (unsigned long)s_config.cfg.start_speed_hz,
             (unsigned long)s_config.cfg.accel_steps,
             s_config.cfg.move_distance_cm,
             s_config.cfg.microsteps);
    return ESP_OK;
}

esp_err_t config_load(press_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    size_t required_size = sizeof(press_config_t);
    ret = nvs_get_blob(handle, KEY_CONFIG_BLOB, cfg, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "failed to get config blob: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}

esp_err_t config_save(const press_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, KEY_CONFIG_BLOB, cfg, sizeof(press_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to write config blob: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}

const press_config_t *config_get(void) { return &s_config.cfg; }

press_config_t *config_lock(void)
{
    xSemaphoreTake(s_config.mutex, portMAX_DELAY);
    return &s_config.cfg;
}

void config_unlock(void) { xSemaphoreGive(s_config.mutex); }

// Compute motor steps per centimeter based on gear ratio and microstepping.
// gear_ratio = stepper_teeth / roller_teeth
// distance_per_rev = PI * roller_diameter_mm * gear_ratio (in mm)
// steps_per_cm = (steps_per_rev * microsteps) / (distance_per_rev / 10.0)
static float calc_steps_per_cm(void)
{
    float gear_ratio = (float)STEPPER_TEETH / (float)ROLLER_TEETH;
    float dist_per_rev_mm = (float)M_PI * ROLLER_DIAMETER_MM * gear_ratio;
    float dist_per_rev_cm = dist_per_rev_mm / 10.0f;
    return (float)(STEPS_PER_REV * s_config.cfg.microsteps) / dist_per_rev_cm;
}

uint32_t config_cm_to_steps(float cm)
{
    return (uint32_t)(cm * calc_steps_per_cm() + 0.5f);
}

float config_steps_to_cm(uint32_t steps)
{
    return (float)steps / calc_steps_per_cm();
}

float config_get_steps_per_cm(void)
{
    return calc_steps_per_cm();
}
