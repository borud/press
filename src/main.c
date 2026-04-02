#include "buttons.h"
#include "config.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motion.h"
#include "nvs_flash.h"
#include "pin_defs.h"
#include "stepper.h"
#include "webserver.h"
#include "wifi_prov.h"

static const char* TAG = "main";

static void on_wifi_connected(void) {
  ESP_LOGI(TAG, "WiFi connected, starting web server");
  esp_err_t ret = webserver_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to start web server: %s", esp_err_to_name(ret));
  }
}

static void on_wifi_disconnected(void) {
  ESP_LOGW(TAG, "WiFi disconnected, stopping web server");
  webserver_stop();
}

void app_main(void) {
  ESP_LOGI(TAG, "Press stepper controller starting");

  // Subscribe main task to Task Watchdog Timer
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
  esp_task_wdt_reset();

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition truncated, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  esp_task_wdt_reset();

  // Initialize config from NVS
  ret = config_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize config: %s", esp_err_to_name(ret));
    return;
  }

  // Apply saved global log level
  esp_log_level_set("*", config_get()->log_level);

  // Initialize stepper driver
  stepper_config_t stepper_cfg = {
      .step_gpio = PIN_STEP,
      .dir_gpio = PIN_DIR,
      .enable_gpio = PIN_ENABLE,
      .resolution_hz = STEP_RESOLUTION_HZ,
      .pulse_ticks = STEP_PULSE_TICKS,
  };
  ret = stepper_init(&stepper_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize stepper: %s", esp_err_to_name(ret));
    return;
  }
  esp_task_wdt_reset();

  // Initialize motion controller (applies config to stepper)
  ret = motion_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize motion: %s", esp_err_to_name(ret));
    return;
  }
  esp_task_wdt_reset();

  // Initialize buttons with motion callbacks
  ret = buttons_init(motion_on_button_press, motion_on_button_release);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize buttons: %s", esp_err_to_name(ret));
    return;
  }
  esp_task_wdt_reset();

  // Initialize WiFi with BLE provisioning
  ret = wifi_prov_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize WiFi: %s", esp_err_to_name(ret));
    // Continue without WiFi — motor control still works
  }

  esp_task_wdt_reset();

  // Start web server when WiFi connects, stop on disconnect
  wifi_on_connected(on_wifi_connected);
  wifi_on_disconnected(on_wifi_disconnected);

  ESP_LOGI(TAG, "ready");

  // Unsubscribe main task from TWDT — init is complete, app_main returns
  esp_task_wdt_delete(NULL);
}
