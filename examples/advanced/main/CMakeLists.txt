idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES app_update console esp_app_format esp_event esp_https_ota esp_netif esp_timer
                                  nvs_flash esp_wifi)
