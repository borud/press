idf_component_register(
    SRCS "main.c" "stepper.c" "rmt_encoder.c" "buttons.c" "config.c" "motion.c"
         "wifi_prov.c" "webserver.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash
             esp_wifi esp_http_server wifi_provisioning bt esp_netif
             json esp_event
)
