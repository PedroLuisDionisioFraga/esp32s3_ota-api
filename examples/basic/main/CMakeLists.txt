# Embed the server root certificate into the final binary
idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES app_update esp_netif nvs_flash esp_wifi esp_psram
                    EMBED_TXTFILES ${project_dir}/certs/ca_cert.pem)
