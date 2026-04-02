#pragma once

#include "esp_err.h"

/**
 * Initialize and start the HTTP web server.
 * Serves static files from LittleFS and REST API endpoints.
 */
esp_err_t webserver_init(void);

/**
 * Stop the web server.
 */
esp_err_t webserver_stop(void);
