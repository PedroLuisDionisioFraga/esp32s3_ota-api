idf_component_register(SRCS "src/ota-api.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_netif esp_event esp_app_format
                    PRIV_REQUIRES esp_https_ota esp_http_client app_update mbedtls esp_timer)
