idf_component_register(SRCS "src/ota-api.c"
                            "src/ota-api-http.c"
                            "src/ota-api-report.c"
                            "src/ota-api-state.c"
                            "src/ota-api-trial.c"
                            "src/ota-api-verify.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_netif esp_app_format
                    PRIV_REQUIRES esp_https_ota esp_http_client app_update esp_partition mbedtls esp_timer)
