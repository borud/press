#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize WiFi with BLE provisioning support.
 * On first boot, starts BLE provisioning (visible as "PRESS_XXXX").
 * On subsequent boots, connects using saved credentials.
 * Also sets up mDNS as "press.local".
 */
esp_err_t wifi_prov_init(void);

/**
 * Check if WiFi is connected and has an IP address.
 */
bool wifi_is_connected(void);

/**
 * Get the current IP address as a string.
 * Returns ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t wifi_get_ip(char *buf, size_t len);

/**
 * Register a callback to be called when WiFi connects (gets IP).
 * If already connected, the callback is called immediately.
 */
typedef void (*wifi_connected_cb_t)(void);
esp_err_t wifi_on_connected(wifi_connected_cb_t cb);

/**
 * Register a callback to be called when WiFi disconnects.
 */
typedef void (*wifi_disconnected_cb_t)(void);
esp_err_t wifi_on_disconnected(wifi_disconnected_cb_t cb);
