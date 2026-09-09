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
 * image's description and the final outcome through a single callback
 * (ota_api_config_t::event_cb), whose return value also decides whether the
 * update carries on: anything but ESP_OK stops it. The callback is optional;
 * ignoring it keeps the original behaviour.
 *
 * Nothing is posted to the default event loop. esp_https_ota already posts
 * ESP_HTTPS_OTA_EVENT there on its own, so a task that only wants to watch an
 * update it did not start should handle that base rather than a duplicate of
 * it. What esp_https_ota cannot do is take an answer back — that is what
 * event_cb is for.
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
 * @brief What an update reports, delivered to ota_api_config_t::event_cb
 *
 * These are callback arguments, not event loop events: nothing is posted to
 * the default event loop. esp_https_ota already posts ESP_HTTPS_OTA_EVENT
 * there — START, CONNECTED, GET_IMG_DESC, WRITE_FLASH, FINISH, ABORT and the
 * rest — so an observer that did not start the update registers a handler for
 * that base instead of asking this component to duplicate it.
 *
 * What those events cannot carry is an answer back, nor a percentage: the
 * IDF's WRITE_FLASH event reports bytes written and no total. Both are what
 * event_cb adds.
 */
typedef enum
{
  OTA_API_EVENT_STARTED,    /**< Connected, download begins. data: NULL */
  OTA_API_EVENT_IMAGE_DESC, /**< New image header read.
                                 data: const esp_app_desc_t * */
  OTA_API_EVENT_PROGRESS,   /**< Bytes downloaded.
                                 data: const ota_api_progress_t * */
  OTA_API_EVENT_SUCCEEDED,  /**< Image written and set as boot partition.
                                 data: NULL. The update is already over, so an
                                 event_cb verdict here is ignored */
  OTA_API_EVENT_FAILED,     /**< Update failed or was aborted.
                                 data: const esp_err_t *. The update is already
                                 over, so an event_cb verdict here is
                                 ignored */
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
 * @brief Called for every event an update reports
 *
 * Runs on the task performing the update, in the middle of it: it must not
 * block, and it must not start another update. How often
 * OTA_API_EVENT_PROGRESS arrives is set by
 * ota_api_config_t::progress_interval_ms — a report the rate limit swallows
 * never reaches this callback, so OTA_API_EVENT_PROGRESS is not the place to
 * poll for a decision made elsewhere. ota_api_abort() is checked on every
 * chunk regardless of the rate limit and remains the reliable way to stop an
 * update from another task.
 *
 * This is the only path the component reports through. A task that merely
 * observes an update it did not start should handle ESP_HTTPS_OTA_EVENT on the
 * default event loop, which esp_https_ota posts by itself.
 *
 * @param event_id Which event fired, and therefore what @p data points to:
 *                 - OTA_API_EVENT_STARTED:    NULL
 *                 - OTA_API_EVENT_IMAGE_DESC: const esp_app_desc_t *
 *                 - OTA_API_EVENT_PROGRESS:   const ota_api_progress_t *
 *                 - OTA_API_EVENT_SUCCEEDED:  NULL
 *                 - OTA_API_EVENT_FAILED:     const esp_err_t *
 * @param data     Event payload. Valid only for the duration of the call
 * @param user_ctx ota_api_config_t::user_ctx as given
 * @return ESP_OK to let the update continue. Any other value stops it and
 *         becomes the return value of ota_api_update(), which unwinds and
 *         leaves the running firmware untouched — refusing at
 *         OTA_API_EVENT_PROGRESS therefore throws away everything downloaded
 *         so far. Refusing an image conventionally returns
 *         ESP_ERR_OTA_VALIDATE_FAILED, declared in esp_ota_ops.h.
 *
 *         Stopping only works while there is still something to stop:
 *         OTA_API_EVENT_SUCCEEDED and OTA_API_EVENT_FAILED are delivered once
 *         the update has already ended, so their return value is ignored —
 *         return ESP_OK there.
 */
typedef esp_err_t (*ota_api_event_cb_t)(ota_api_event_id_t event_id, const void *data, void *user_ctx);

/**
 * @brief OTA update configuration
 *
 * Pointers (url, cert_pem, bind_netif, user_ctx) are NOT copied: they must
 * remain valid until the update finishes — for ota_api_start_task, until the
 * device reboots or the task logs a failure and exits.
 */
typedef struct
{
  const char *url;               /**< Firmware image URL (required) */
  const char *cert_pem;          /**< Server certificate in PEM format.
                                      NULL = use the trusted root certificate
                                      bundle (requires
                                      MBEDTLS_CERTIFICATE_BUNDLE) */
  bool skip_common_name_check;   /**< Skip server certificate CN validation */
  esp_netif_t *bind_netif;       /**< Bind the HTTP connection to this network
                                      interface. NULL = any */
  uint32_t task_stack_size;      /**< ota_api_start_task only. 0 = use
                                      CONFIG_OTA_API_TASK_STACK_SIZE */
  UBaseType_t task_priority;     /**< ota_api_start_task only. 0 = use
                                      CONFIG_OTA_API_TASK_PRIORITY */
  ota_api_event_cb_t event_cb;   /**< Called for every event, and the only way
                                      to stop an update from inside it.
                                      NULL = report nothing */
  void *user_ctx;                /**< Passed unchanged to event_cb */
  uint32_t progress_interval_ms; /**< Minimum gap between progress reports,
                                      which is also how often event_cb gets a
                                      chance to stop the update at
                                      OTA_API_EVENT_PROGRESS.
                                      0 = report every chunk */
  bool partial_download;         /**< Fetch the image over several ranged HTTP
                                      requests, which survives links that drop
                                      long transfers. Requires
                                      CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD */
  size_t max_http_request_size;  /**< Bytes per request when partial_download
                                      is set. 0 = let esp_https_ota choose */
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
 * Downloads the image from config->url over HTTPS and writes it to the next
 * OTA partition, setting it as the boot partition. Does NOT restart the
 * device — the caller decides when to reboot into the new firmware.
 *
 * Blocks for the whole transfer, which is tens of seconds on a slow link: the
 * calling task does nothing else until it returns. A task that has to stay
 * responsive — a console, a UI, a protocol handler — must therefore not call
 * this directly, or its own work stalls for the duration of the download and
 * it cannot even service an ota_api_abort(). Two ways around it: let
 * ota_api_start_task() spawn the task, or keep a task of your own that blocks
 * on a queue or semaphore and calls this when asked. The on_demand example
 * takes the second route, which is what keeps its console usable mid-download.
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
 *         - ESP_ERR_OTA_VALIDATE_FAILED: event_cb refused the image, or the
 *           downloaded image is incomplete or invalid
 *         - ESP_ERR_NOT_FINISHED: Stopped by ota_api_abort()
 *         - Whatever else event_cb returned to stop the update
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
 * This is the shortest way to keep the blocking download off the caller's
 * task, at the cost of the reboot being decided here. An application that has
 * to act between the write and the restart — close a connection, warn an
 * operator, wait for an idle moment — should run ota_api_update() from a task
 * of its own instead.
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
