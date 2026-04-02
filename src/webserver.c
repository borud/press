#include "webserver.h"
#include "config.h"
#include "motion.h"
#include "stepper.h"
#include "wifi_prov.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "webserver";

static httpd_handle_t s_server = NULL;

// ============================================================================
// Helpers
// ============================================================================

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialization failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return ESP_OK;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg);
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return ESP_OK;
}

static cJSON *parse_json_body(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1024) {
        return NULL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) return NULL;

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

// ============================================================================
// Static file serving from LittleFS
// ============================================================================

static const char *get_content_type(const char *path)
{
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".css"))  return "text/css";
    if (strstr(path, ".js"))   return "application/javascript";
    if (strstr(path, ".json")) return "application/json";
    if (strstr(path, ".ico"))  return "image/x-icon";
    return "text/plain";
}

static esp_err_t serve_static_file(httpd_req_t *req, const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ============================================================================
// Route handlers
// ============================================================================

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return serve_static_file(req, "/littlefs/index.html");
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "/littlefs%.117s", req->uri);
    return serve_static_file(req, filepath);
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    const press_config_t *cfg = config_get();
    cJSON *json = cJSON_CreateObject();

    cJSON_AddStringToObject(json, "activity", motion_get_activity());
    cJSON_AddBoolToObject(json, "running", stepper_is_running());

    cJSON_AddNumberToObject(json, "max_speed_hz", cfg->max_speed_hz);
    cJSON_AddNumberToObject(json, "start_speed_hz", cfg->start_speed_hz);
    cJSON_AddNumberToObject(json, "accel_steps", cfg->accel_steps);
    cJSON_AddNumberToObject(json, "move_distance_cm", cfg->move_distance_cm);
    cJSON_AddNumberToObject(json, "microsteps", cfg->microsteps);

    cJSON_AddBoolToObject(json, "wifi_connected", wifi_is_connected());
    char ip[16] = "N/A";
    wifi_get_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(json, "ip", ip);

    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());

    esp_err_t ret = send_json_response(req, json);
    cJSON_Delete(json);
    return ret;
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    const press_config_t *cfg = config_get();
    cJSON *json = cJSON_CreateObject();

    cJSON_AddNumberToObject(json, "max_speed_hz", cfg->max_speed_hz);
    cJSON_AddNumberToObject(json, "start_speed_hz", cfg->start_speed_hz);
    cJSON_AddNumberToObject(json, "accel_steps", cfg->accel_steps);
    cJSON_AddNumberToObject(json, "move_distance_cm", cfg->move_distance_cm);
    cJSON_AddNumberToObject(json, "microsteps", cfg->microsteps);
    cJSON_AddNumberToObject(json, "log_level", cfg->log_level);

    esp_err_t ret = send_json_response(req, json);
    cJSON_Delete(json);
    return ret;
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (!json) {
        return send_error(req, 400, "invalid JSON body");
    }

    press_config_t *cfg = config_lock();
    bool changed = false;

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "max_speed_hz")) && cJSON_IsNumber(item)) {
        uint32_t val = (uint32_t)item->valuedouble;
        if (val > 0 && val <= 100000) {
            cfg->max_speed_hz = val;
            changed = true;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "start_speed_hz")) && cJSON_IsNumber(item)) {
        uint32_t val = (uint32_t)item->valuedouble;
        if (val > 0 && val < cfg->max_speed_hz) {
            cfg->start_speed_hz = val;
            changed = true;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "accel_steps")) && cJSON_IsNumber(item)) {
        uint32_t val = (uint32_t)item->valuedouble;
        if (val > 0 && val <= 10000) {
            cfg->accel_steps = val;
            changed = true;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "move_distance_cm")) && cJSON_IsNumber(item)) {
        float val = (float)item->valuedouble;
        if (val > 0.0f && val <= 1000.0f) {
            cfg->move_distance_cm = val;
            changed = true;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "microsteps")) && cJSON_IsNumber(item)) {
        uint16_t val = (uint16_t)item->valuedouble;
        if (val == 1 || val == 2 || val == 4 || val == 8 || val == 16 || val == 32) {
            cfg->microsteps = val;
            changed = true;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "log_level")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val < 0 || val > 5) {
            config_unlock();
            cJSON_Delete(json);
            return send_error(req, 400, "log_level must be 0-5");
        }
        cfg->log_level = (uint8_t)val;
        esp_log_level_set("*", cfg->log_level);
        changed = true;
    }

    if (changed) {
        esp_err_t save_ret = config_save(cfg);
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to save config: %s", esp_err_to_name(save_ret));
        }

        if (!stepper_is_running()) {
            stepper_motion_params_t params = {
                .max_speed_hz = cfg->max_speed_hz,
                .start_speed_hz = cfg->start_speed_hz,
                .accel_steps = cfg->accel_steps,
            };
            stepper_set_motion_params(&params);
        }
    }

    config_unlock();
    cJSON_Delete(json);

    return api_config_get_handler(req);
}

static esp_err_t api_move_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (!json) {
        return send_error(req, 400, "invalid JSON body");
    }

    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        return send_error(req, 400, "missing 'action' field");
    }

    esp_err_t ret = ESP_OK;
    const char *act = action->valuestring;

    if (strcmp(act, "jog-fwd") == 0) {
        ret = motion_jog_start(BTN_FWD);
    } else if (strcmp(act, "jog-rev") == 0) {
        ret = motion_jog_start(BTN_REV);
    } else if (strcmp(act, "jog-stop") == 0) {
        ret = motion_jog_stop();
    } else if (strcmp(act, "move-fwd") == 0) {
        float cm = config_get()->move_distance_cm;
        ret = motion_move_cm(cm);
    } else if (strcmp(act, "move-rev") == 0) {
        float cm = config_get()->move_distance_cm;
        ret = motion_move_cm(-cm);
    } else if (strcmp(act, "stop") == 0) {
        ret = motion_stop();
    } else {
        cJSON_Delete(json);
        return send_error(req, 400, "unknown action");
    }

    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", ret == ESP_OK ? "ok" : "error");
    esp_err_t send_ret = send_json_response(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t api_firmware_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "build_date", __DATE__ " " __TIME__);
    cJSON_AddStringToObject(json, "idf_version", esp_get_idf_version());
    esp_err_t ret = send_json_response(req, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Server init/stop
// ============================================================================

static esp_err_t init_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "LittleFS: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

esp_err_t webserver_init(void)
{
    if (s_server) {
        ESP_LOGI(TAG, "web server already running, skipping init");
        return ESP_OK;
    }

    esp_err_t ret = init_littlefs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed, static files won't be served");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t routes[] = {
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = api_status_handler },
        { .uri = "/api/config",      .method = HTTP_GET,  .handler = api_config_get_handler },
        { .uri = "/api/config",      .method = HTTP_POST, .handler = api_config_post_handler },
        { .uri = "/api/move",        .method = HTTP_POST, .handler = api_move_handler },
        { .uri = "/api/firmware",    .method = HTTP_GET,  .handler = api_firmware_handler },
        { .uri = "/",                .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/*",               .method = HTTP_GET,  .handler = static_get_handler },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t reg_ret = httpd_register_uri_handler(s_server, &routes[i]);
        if (reg_ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to register handler for %s: %s",
                     routes[i].uri, esp_err_to_name(reg_ret));
        }
    }

    ESP_LOGI(TAG, "web server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "web server stopped");
    }
    return ESP_OK;
}
