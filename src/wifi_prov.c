#include "wifi_prov.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char* TAG = "wifi_prov";

#define WIFI_CONNECTED_BIT BIT0

static struct {
  EventGroupHandle_t  event_group;
  char                ip_str[16];
  bool                connected;
  int                 retry_count;
  wifi_connected_cb_t on_connected_cb;
} s_wifi;

#define MAX_RETRY_BACKOFF 30  // Max 30s between retries

static esp_err_t init_mdns(void);

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_PROV_EVENT) {
    switch (event_id) {
      case WIFI_PROV_START:
        ESP_LOGI(TAG, "provisioning started");
        break;
      case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t* cfg = (wifi_sta_config_t*)event_data;
        ESP_LOGI(TAG, "received WiFi credentials for SSID: %s", (char*)cfg->ssid);
        break;
      }
      case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t* reason = (wifi_prov_sta_fail_reason_t*)event_data;
        ESP_LOGE(TAG, "provisioning failed: %s", (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "auth error" : "AP not found");
        break;
      }
      case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "provisioning successful");
        break;
      case WIFI_PROV_END:
        wifi_prov_mgr_deinit();
        ESP_LOGI(TAG, "provisioning manager deinitialized");
        break;
      default:
        break;
    }
    return;
  }

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        s_wifi.connected = false;
        xEventGroupClearBits(s_wifi.event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, retrying (attempt %d)...", s_wifi.retry_count + 1);
        int delay_s = (1 << s_wifi.retry_count);
        if (delay_s > MAX_RETRY_BACKOFF) {
          delay_s = MAX_RETRY_BACKOFF;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        s_wifi.retry_count++;
        esp_wifi_connect();
        break;
      default:
        break;
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    snprintf(s_wifi.ip_str, sizeof(s_wifi.ip_str), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "got IP: %s", s_wifi.ip_str);
    s_wifi.connected   = true;
    s_wifi.retry_count = 0;
    init_mdns();
    xEventGroupSetBits(s_wifi.event_group, WIFI_CONNECTED_BIT);
    if (s_wifi.on_connected_cb) {
      s_wifi.on_connected_cb();
    }
  }
}

static void get_device_service_name(char* service_name, size_t max) {
  uint8_t mac[6];

  esp_wifi_get_mac(WIFI_IF_STA, mac);
  snprintf(service_name, max, "PROV_PRESS_%02X%02X", mac[4], mac[5]);
}

static esp_err_t init_mdns(void) {
  esp_err_t ret = mdns_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
    return ret;
  }
  mdns_hostname_set("press");
  mdns_instance_name_set("Press Stepper Controller");
  mdns_service_add("Press Controller", "_press-controller", "_tcp", 80, NULL, 0);
  ESP_LOGI(TAG, "mDNS started: press.local");
  return ESP_OK;
}

esp_err_t wifi_prov_init(void) {
  s_wifi.event_group = xEventGroupCreate();
  if (!s_wifi.event_group) {
    ESP_LOGE(TAG, "failed to create event group");
    return ESP_ERR_NO_MEM;
  }

  // Initialize networking stack
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");

  // Create default WiFi station
  esp_netif_create_default_wifi_sta();

  // Register event handlers
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL),
      TAG,
      "failed to register prov event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL),
      TAG,
      "failed to register wifi event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL),
      TAG,
      "failed to register ip event handler");

  // Initialize WiFi
  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init failed");

  // Initialize provisioning manager
  wifi_prov_mgr_config_t prov_cfg = {
      .scheme               = wifi_prov_scheme_ble,
      .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
  };
  ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(prov_cfg), TAG, "prov mgr init failed");

  // Check if already provisioned
  bool provisioned = false;
  ESP_RETURN_ON_ERROR(wifi_prov_mgr_is_provisioned(&provisioned), TAG, "failed to check provisioning state");

  if (provisioned) {
    ESP_LOGI(TAG, "already provisioned, connecting to WiFi");
    wifi_prov_mgr_deinit();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
  } else {
    ESP_LOGI(TAG, "device not provisioned, starting BLE provisioning");

    char service_name[16];
    get_device_service_name(service_name, sizeof(service_name));
    ESP_LOGI(TAG, "BLE device name: %s", service_name);

    ESP_LOGI(TAG, "Proof of Possession (PoP): %s", CONFIG_PRESS_PROV_POP);

    ESP_RETURN_ON_ERROR(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, CONFIG_PRESS_PROV_POP, service_name, NULL),
        TAG,
        "failed to start provisioning");
  }

  // Wait for connection (with timeout)
  ESP_LOGI(TAG, "waiting for WiFi connection...");
  EventBits_t bits = xEventGroupWaitBits(s_wifi.event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000));
  if (!(bits & WIFI_CONNECTED_BIT)) {
    ESP_LOGW(TAG, "WiFi connection timeout — will keep trying in background");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "WiFi connected: %s", s_wifi.ip_str);
  return ESP_OK;
}

bool wifi_is_connected(void) { return s_wifi.connected; }

esp_err_t wifi_get_ip(char* buf, size_t len) {
  if (!s_wifi.connected) {
    return ESP_ERR_INVALID_STATE;
  }
  strncpy(buf, s_wifi.ip_str, len);
  buf[len - 1] = '\0';
  return ESP_OK;
}

esp_err_t wifi_on_connected(wifi_connected_cb_t cb) {
  s_wifi.on_connected_cb = cb;
  // If already connected, fire immediately
  if (s_wifi.connected && cb) {
    cb();
  }
  return ESP_OK;
}
