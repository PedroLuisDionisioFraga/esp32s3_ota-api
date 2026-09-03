/**
 * @file ota-api.h
 * @brief Simplified API for HTTPS OTA firmware updates in ESP-IDF
 *
 * This API wraps esp_https_ota behind a small configuration struct: provide
 * the firmware URL and an optional server certificate, then run the update
 * synchronously (ota_api_update) or in a background task that reboots the
 * device on success (ota_api_start_task).
 *
 * While an update runs the component reports download progress, the new
 * image's description and the final outcome. Progress is delivered two ways —
 * a direct callback (ota_api_config_t::progress_cb) and events posted to the
 * default event loop (OTA_API_EVENT) — so an application can take whichever
 * fits. Both are optional; ignoring them keeps the original behaviour.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#ifndef OTA_API_H
#define OTA_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ========================================================================== */
/*                                  EVENTS                                    */
/* ========================================================================== */

/**
 * @brief Event base for updates reported to the default event loop
 *
 * Events are posted only when a default event loop exists; if the application
 * never calls esp_event_loop_create_default() they are silently skipped.
 */
ESP_EVENT_DECLARE_BASE(OTA_API_EVENT);

/**
 * @brief Events posted under OTA_API_EVENT
 */
typedef enum
{
  OTA_API_EVENT_STARTED,    /**< Connected, download begins. event_data: NULL */
  OTA_API_EVENT_IMAGE_DESC, /**< New image header read.
                                 event_data: const esp_app_desc_t * */
  OTA_API_EVENT_PROGRESS,   /**< Bytes downloaded.
                                 event_data: const ota_api_progress_t * */
  OTA_API_EVENT_SUCCEEDED,  /**< Image written and set as boot partition.
                                 event_data: NULL */
  OTA_API_EVENT_FAILED,     /**< Update failed or was aborted.
                                 event_data: const esp_err_t * */
} ota_api_event_id_t;

/* ========================================================================== */
/*                           TYPES AND STRUCTURES                             */
/* ========================================================================== */

/**
 * @brief Download progress snapshot
 */
typedef struct
{
  size_t bytes_read;  /**< Image bytes downloaded so far */
  size_t total_bytes; /**< Total image size, or 0 when the server does not
                           report a Content-Length */
  int percent;        /**< Completion in 0..100, or -1 when total_bytes is 0 */
} ota_api_progress_t;

/**
 * @brief Called as the image downloads
 *
 * Runs on the task performing the update, in the middle of the download: it
 * must not block or start another update. Rate is limited by
 * ota_api_config_t::progress_interval_ms.
 *
 * @param progress Current progress. Valid only for the duration of the call
 * @param user_ctx ota_api_config_t::user_ctx as given
 */
typedef void (*ota_api_progress_cb_t)(const ota_api_progress_t *progress, void *user_ctx);

/**
 * @brief Called once the new image's header has been read, before it is written
 *
 * Lets the application inspect the incoming firmware — typically to refuse a
 * version it already runs — while only the image header has been downloaded.
 *
 * @param new_app  Description of the image being offered
 * @param user_ctx ota_api_config_t::user_ctx as given
 * @return true to continue the update, false to stop it. Stopping makes the
 *         update return ESP_ERR_OTA_VALIDATE_FAILED
 */
typedef bool (*ota_api_validate_cb_t)(const esp_app_desc_t *new_app, void *user_ctx);

/**
 * @brief OTA update configuration
 *
 * Pointers (url, cert_pem, bind_netif, user_ctx) are NOT copied: they must
 * remain valid until the update finishes — for ota_api_start_task, until the
 * device reboots or the task logs a failure and exits.
 */
typedef struct
{
  const char *url;                   /**< Firmware image URL (required) */
  const char *cert_pem;              /**< Server certificate in PEM format.
                                          NULL = use the trusted root
                                          certificate bundle (requires
                                          MBEDTLS_CERTIFICATE_BUNDLE) */
  bool skip_common_name_check;       /**< Skip server certificate CN
                                          validation */
  esp_netif_t *bind_netif;           /**< Bind the HTTP connection to this
                                          network interface. NULL = any */
  uint32_t task_stack_size;          /**< ota_api_start_task only. 0 = use
                                          CONFIG_OTA_API_TASK_STACK_SIZE */
  UBaseType_t task_priority;         /**< ota_api_start_task only. 0 = use
                                          CONFIG_OTA_API_TASK_PRIORITY */
  ota_api_progress_cb_t progress_cb; /**< Download progress callback.
                                          NULL = no callback (events are
                                          still posted) */
  ota_api_validate_cb_t validate_cb; /**< Accept or refuse the incoming image.
                                          NULL = always accept */
  void *user_ctx;                    /**< Passed unchanged to both callbacks */
  uint32_t progress_interval_ms;     /**< Minimum gap between progress
                                          reports. 0 = report every chunk */
  bool partial_download;             /**< Fetch the image over several ranged
                                          HTTP requests, which survives links
                                          that drop long transfers. Requires
                                          CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD */
  size_t max_http_request_size;      /**< Bytes per request when
                                          partial_download is set. 0 = let
                                          esp_https_ota choose */
} ota_api_config_t;

/**
 * @brief Macro to initialize ota_api_config_t with default values
 */
#define OTA_API_CONFIG_DEFAULT()     \
  {                                  \
    .url = NULL,                     \
    .cert_pem = NULL,                \
    .skip_common_name_check = false, \
    .bind_netif = NULL,              \
    .task_stack_size = 0,            \
    .task_priority = 0,              \
    .progress_cb = NULL,             \
    .validate_cb = NULL,             \
    .user_ctx = NULL,                \
    .progress_interval_ms = 0,       \
    .partial_download = false,       \
    .max_http_request_size = 0,      \
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
 * Only one update may run at a time, whether started here or by
 * ota_api_start_task().
 *
 * @param config Update configuration (url is required)
 * @return esp_err_t
 *         - ESP_OK: New firmware written; boots on next restart
 *         - ESP_ERR_INVALID_ARG: NULL config/url, or cert_pem is NULL while
 *           the certificate bundle is disabled
 *         - ESP_ERR_INVALID_STATE: Another update is already running
 *         - ESP_ERR_OTA_VALIDATE_FAILED: validate_cb refused the image, or the
 *           downloaded image is incomplete or invalid
 *         - ESP_ERR_NOT_FINISHED: Stopped by ota_api_abort()
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

/**
 * @brief Ask the running update to stop
 *
 * Cooperative: it raises a flag that the update notices between downloaded
 * chunks, then unwinds cleanly and leaves the current firmware untouched. The
 * update call returns ESP_ERR_NOT_FINISHED shortly after, so this function
 * returning ESP_OK means the request was accepted, not that the update has
 * already stopped. Safe to call from any task.
 *
 * @return esp_err_t
 *         - ESP_OK: Stop requested
 *         - ESP_ERR_INVALID_STATE: No update is running
 */
esp_err_t ota_api_abort(void);

/**
 * @brief Whether an update is currently running
 *
 * @return true while an update is in progress
 */
bool ota_api_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_API_H */
