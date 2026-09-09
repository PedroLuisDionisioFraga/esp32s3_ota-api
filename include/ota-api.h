/**
 * @file ota-api.h
 * @brief Simplified API for HTTPS OTA firmware updates in ESP-IDF
 *
 * Wraps esp_https_ota behind one configuration struct: set the firmware URL and
 * an optional server certificate, then update synchronously (ota_api_update) or
 * in a background task that reboots on success (ota_api_start_task). A single
 * callback (ota_api_config_t::event_cb) reports progress and the outcome and
 * may stop the update by returning non-ESP_OK. Nothing is posted to the default
 * event loop. See README.md for the rationale.
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
 * @brief What an update reports to ota_api_config_t::event_cb
 *
 * Callback arguments, not event-loop events. The type after each value is what
 * @p data points to in the callback.
 */
typedef enum
{
  OTA_API_EVENT_STARTED,    /**< Download begins. NULL */
  OTA_API_EVENT_IMAGE_DESC, /**< Image header read. const esp_app_desc_t * */
  OTA_API_EVENT_PROGRESS,   /**< Bytes downloaded. const ota_api_progress_t * */
  OTA_API_EVENT_SUCCEEDED,  /**< Image written and set as boot partition. NULL */
  OTA_API_EVENT_FAILED,     /**< Update failed or aborted. const esp_err_t * */
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
  size_t total_bytes; /**< Total image size, or 0 when the server sends no
                           Content-Length */
  int percent;        /**< Completion 0..100, or -1 when total_bytes is 0 */
} ota_api_progress_t;

/**
 * @brief Called for every event an update reports
 *
 * Runs synchronously on the update task. During the transfer, blocking stalls
 * the download and delays ota_api_abort(); do not start another update from
 * here.
 *
 * @param event_id Which event fired
 * @param data     Payload for it (type per event in ota_api_event_id_t), valid
 *                 only for the duration of the call
 * @param user_ctx ota_api_config_t::user_ctx as given
 * @return ESP_OK to continue. Any other value stops the update, leaves the
 *         running firmware untouched, and becomes ota_api_update()'s return
 *         value (conventionally ESP_ERR_OTA_VALIDATE_FAILED to refuse an
 *         image). Ignored at OTA_API_EVENT_SUCCEEDED and OTA_API_EVENT_FAILED.
 */
typedef esp_err_t (*ota_api_event_cb_t)(ota_api_event_id_t event_id, const void *data, void *user_ctx);

/**
 * @brief OTA update configuration
 *
 * Pointer fields are not copied and must stay valid until the update finishes
 * (for ota_api_start_task, until the device reboots or the task exits).
 */
typedef struct
{
  const char *url;               /**< Firmware image URL (required) */
  const char *cert_pem;          /**< Server certificate, PEM. NULL = trusted
                                      root bundle (needs
                                      MBEDTLS_CERTIFICATE_BUNDLE), or no server
                                      verification when
                                      CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is
                                      set. Ignored for an http:// URL */
  bool skip_common_name_check;   /**< Skip server certificate CN validation */
  esp_netif_t *bind_netif;       /**< Bind the connection to this interface.
                                      NULL = any */
  uint32_t task_stack_size;      /**< ota_api_start_task only. 0 =
                                      CONFIG_OTA_API_TASK_STACK_SIZE */
  UBaseType_t task_priority;     /**< ota_api_start_task only. 0 =
                                      CONFIG_OTA_API_TASK_PRIORITY */
  ota_api_event_cb_t event_cb;   /**< Per-event callback, and the only way to
                                      stop an update from inside it.
                                      NULL = report nothing */
  void *user_ctx;                /**< Passed unchanged to event_cb */
  uint32_t progress_interval_ms; /**< Minimum gap between progress reports.
                                      0 = every chunk */
  bool partial_download;         /**< Fetch the image over several ranged
                                      requests, surviving links that drop long
                                      transfers. Needs
                                      CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD */
  size_t max_http_request_size;  /**< Bytes per request when partial_download is
                                      set. 0 = esp_https_ota chooses */
} ota_api_config_t;

/**
 * @brief Initialize ota_api_config_t with default values
 */
#define OTA_API_CONFIG_DEFAULT()     \
  {                                  \
    .url = NULL,                     \
    .cert_pem = NULL,                \
    .skip_common_name_check = false, \
    .bind_netif = NULL,              \
    .task_stack_size = 0,            \
    .task_priority = 0,              \
    .event_cb = NULL,                \
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
 * Downloads config->url over HTTPS to the next OTA partition and sets it as the
 * boot partition; does not restart. One update runs at a time.
 *
 * @param config Update configuration (url is required)
 * @return
 *         - ESP_OK: new firmware written; boots on next restart
 *         - ESP_ERR_INVALID_ARG: NULL config/url; cert_pem NULL with neither the
 *           certificate bundle nor CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
 *           enabled; or http:// URL with CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP disabled
 *         - ESP_ERR_INVALID_STATE: another update is already running
 *         - ESP_ERR_OTA_VALIDATE_FAILED: event_cb refused the image, or it is
 *           incomplete or invalid
 *         - ESP_ERR_NOT_FINISHED: stopped by ota_api_abort()
 *         - any other value event_cb returned, or an error from esp_https_ota
 */
esp_err_t ota_api_update(const ota_api_config_t *config);

/**
 * @brief Run the update in a background task and reboot on success
 *
 * Spawns a FreeRTOS task that calls ota_api_update(); on success it restarts,
 * on failure it logs the error and exits.
 *
 * @param config Update configuration (url is required). The struct is copied;
 *               the pointers inside must stay valid (see ota_api_config_t).
 * @return
 *         - ESP_OK: task created
 *         - ESP_ERR_INVALID_ARG: NULL config or url
 *         - ESP_ERR_NO_MEM: task or config-copy allocation failed
 */
esp_err_t ota_api_start_task(const ota_api_config_t *config);

/**
 * @brief Ask the running update to stop
 *
 * Cooperative and non-blocking: ESP_OK means the request was accepted, not that
 * the update has stopped. Safe from any task.
 *
 * @return
 *         - ESP_OK: stop requested
 *         - ESP_ERR_INVALID_STATE: no update is running
 */
esp_err_t ota_api_abort(void);

/**
 * @brief Whether an update is currently running
 *
 * A snapshot, not a lock. Under ota_api_start_task() it is already false while
 * event_cb handles OTA_API_EVENT_SUCCEEDED, before the restart.
 *
 * @return true while an update is in progress
 */
bool ota_api_is_running(void);

/* ========================================================================== */
/*                           TRIAL RUN AND ROLLBACK                           */
/* ========================================================================== */

/*
 * There is no self-test callback: whether the new firmware works can only be
 * answered after the reboot, by the new image, and what "works" means is the
 * product's to define. The application calls these functions; the component
 * does not call back. See README.md, "Why the self-test is yours to write".
 */

/**
 * @brief Whether the running image still has to prove itself
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a new image boots on trial and the
 * next reset reverts unless it is confirmed. Read live from the partition table.
 *
 * @return true while the running image is on trial; false once confirmed, when
 *         running from the factory partition, or in a build without rollback
 *         support
 */
bool ota_api_is_on_trial(void);

/**
 * @brief Start the countdown that rolls back an image nobody confirms
 *
 * Arms a one-shot timer that calls
 * esp_ota_mark_app_invalid_rollback_and_reboot() on expiry. A synchronous
 * self-test can skip this and call ota_api_trial_confirm() / _reject() directly.
 *
 * @param timeout_s Seconds to wait for a verdict. 0 =
 *                  CONFIG_OTA_API_TRIAL_TIMEOUT_S
 * @return
 *         - ESP_OK: countdown started
 *         - ESP_ERR_INVALID_STATE: the image is not on trial, or a countdown is
 *           already running
 *         - any error from esp_timer
 */
esp_err_t ota_api_trial_begin(uint32_t timeout_s);

/**
 * @brief Keep the image on trial and cancel the rollback
 *
 * Marks the running image valid and stops any ota_api_trial_begin() countdown.
 *
 * @return
 *         - ESP_OK: image confirmed; the device boots it from now on
 *         - ESP_ERR_INVALID_STATE: the running image is not on trial
 *         - any error from esp_ota_mark_app_valid_cancel_rollback()
 */
esp_err_t ota_api_trial_confirm(void);

/**
 * @brief Reject the running image and reboot into the previous firmware
 *
 * Does not return on success; comes back only when there is nothing to roll
 * back to. Unlike ota_api_trial_confirm() it does not require a trial image.
 *
 * @return
 *         - ESP_ERR_OTA_ROLLBACK_FAILED: no valid image to go back to
 *         - ESP_FAIL: running from the factory partition, which cannot be
 *           rolled back
 *         - any other error from esp_ota_mark_app_invalid_rollback_and_reboot()
 */
esp_err_t ota_api_trial_reject(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_API_H */
