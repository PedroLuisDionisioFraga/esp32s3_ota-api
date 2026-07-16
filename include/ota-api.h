/**
 * @file ota-api.h
 * @brief Simplified API for HTTPS OTA firmware updates in ESP-IDF
 *
 * This API wraps esp_https_ota behind a small configuration struct: provide
 * the firmware URL and an optional server certificate, then run the update
 * synchronously (ota_api_update) or in a background task that reboots the
 * device on success (ota_api_start_task).
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#ifndef OTA_API_H
#define OTA_API_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                           TYPES AND STRUCTURES                             */
/* ========================================================================== */

/**
 * @brief OTA update configuration
 *
 * Pointers (url, cert_pem, bind_netif) are NOT copied: they must remain valid
 * until the update finishes — for ota_api_start_task, until the device
 * reboots or the task logs a failure and exits.
 */
typedef struct
{
  const char *url;             /**< Firmware image URL (required) */
  const char *cert_pem;        /**< Server certificate in PEM format. NULL =
                                    use the trusted root certificate bundle
                                    (requires MBEDTLS_CERTIFICATE_BUNDLE) */
  bool skip_common_name_check; /**< Skip server certificate CN validation */
  esp_netif_t *bind_netif;     /**< Bind the HTTP connection to this network
                                    interface. NULL = any interface */
  uint32_t task_stack_size;    /**< ota_api_start_task only. 0 = use
                                    CONFIG_OTA_API_TASK_STACK_SIZE */
  UBaseType_t task_priority;   /**< ota_api_start_task only. 0 = use
                                    CONFIG_OTA_API_TASK_PRIORITY */
} ota_api_config_t;

/**
 * @brief Macro to initialize ota_api_config_t with default values
 */
#define OTA_API_CONFIG_DEFAULT()  \
  {                               \
    .url = NULL,                  \
    .cert_pem = NULL,             \
    .skip_common_name_check = false, \
    .bind_netif = NULL,           \
    .task_stack_size = 0,         \
    .task_priority = 0,           \
  }

/* ========================================================================== */
/*                                 FUNCTIONS                                  */
/* ========================================================================== */

/**
 * @brief Download and write a firmware update (blocking)
 *
 * Downloads the image from config->url over HTTPS and writes it to the next
 * OTA partition, setting it as the boot partition. Does NOT restart the
 * device — the caller decides when to reboot into the new firmware.
 *
 * @param config Update configuration (url is required)
 * @return esp_err_t
 *         - ESP_OK: New firmware written; boots on next restart
 *         - ESP_ERR_INVALID_ARG: NULL config/url, or cert_pem is NULL while
 *           the certificate bundle is disabled
 *         - Any error propagated from esp_https_ota
 */
esp_err_t ota_api_update(const ota_api_config_t *config);

/**
 * @brief Run the OTA update in a background task and reboot on success
 *
 * Spawns a FreeRTOS task that calls ota_api_update(). On success the device
 * restarts into the new firmware; on failure the task logs the error and
 * deletes itself.
 *
 * @param config Update configuration (url is required). The struct itself is
 *               copied, but the pointers inside must stay valid (see
 *               ota_api_config_t).
 * @return esp_err_t
 *         - ESP_OK: Task created
 *         - ESP_ERR_INVALID_ARG: NULL config or url
 *         - ESP_ERR_NO_MEM: Task or config copy allocation failed
 */
esp_err_t ota_api_start_task(const ota_api_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* OTA_API_H */
