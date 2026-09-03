/**
 * @file ota-api.c
 * @brief HTTPS OTA firmware update implementation
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include "ota-api.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
// For ESP_ERR_OTA_VALIDATE_FAILED; callers that compare against it need this
// header too, so it stays out of the public API
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "ota-api";

ESP_EVENT_DEFINE_BASE(OTA_API_EVENT);

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_running;
static bool s_abort_requested;

/**
 * @brief Take ownership of the single update slot
 *
 * @return true if this caller may proceed, false if an update already runs
 */
static bool claim_update_slot(void)
{
  bool claimed = false;

  portENTER_CRITICAL(&s_state_lock);
  if (!s_running)
  {
    s_running = true;
    s_abort_requested = false;
    claimed = true;
  }
  portEXIT_CRITICAL(&s_state_lock);

  return claimed;
}

static void release_update_slot(void)
{
  portENTER_CRITICAL(&s_state_lock);
  s_running = false;
  s_abort_requested = false;
  portEXIT_CRITICAL(&s_state_lock);
}

static bool abort_requested(void)
{
  portENTER_CRITICAL(&s_state_lock);
  bool requested = s_abort_requested;
  portEXIT_CRITICAL(&s_state_lock);

  return requested;
}

/**
 * @brief Post an event, tolerating the absence of a default event loop
 *
 * An application that never creates the default loop simply does not get
 * events; that is not an error worth failing an update over.
 */
static void post_event(ota_api_event_id_t event_id, const void *data, size_t data_size)
{
  esp_err_t err = esp_event_post(OTA_API_EVENT, event_id, (void *)data, data_size, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
  {
    ESP_LOGD(TAG, "Could not post event %d (%s)", (int)event_id, esp_err_to_name(err));
  }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
  switch (evt->event_id)
  {
    case HTTP_EVENT_ERROR:
      ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
      break;
    case HTTP_EVENT_ON_HEADERS_COMPLETE:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
      break;
    case HTTP_EVENT_ON_DATA:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      break;
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
      break;
    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      break;
    default:
      break;
  }
  return ESP_OK;
}

/**
 * @brief Build the HTTP client configuration shared by every update
 *
 * @param ifr Storage for the bound interface name; must outlive the update
 * @return ESP_ERR_INVALID_ARG when no way to validate the server is available
 */
static esp_err_t build_http_config(const ota_api_config_t *config, esp_http_client_config_t *http_config,
                                   struct ifreq *ifr)
{
  http_config->url = config->url;
  http_config->event_handler = http_event_handler;
  http_config->keep_alive_enable = true;
  http_config->skip_cert_common_name_check = config->skip_common_name_check;

  if (config->cert_pem)
  {
    http_config->cert_pem = config->cert_pem;
  }
  else
  {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    http_config->crt_bundle_attach = esp_crt_bundle_attach;
#else
    ESP_LOGE(TAG, "cert_pem is NULL and MBEDTLS_CERTIFICATE_BUNDLE is disabled");
    return ESP_ERR_INVALID_ARG;
#endif
  }

  if (config->bind_netif)
  {
    esp_netif_get_netif_impl_name(config->bind_netif, ifr->ifr_name);
    http_config->if_name = ifr;
    ESP_LOGI(TAG, "Binding OTA connection to interface %s", ifr->ifr_name);
  }

  return ESP_OK;
}

/**
 * @brief Read the incoming image header and let the application veto it
 */
static esp_err_t check_incoming_image(esp_https_ota_handle_t handle, const ota_api_config_t *config)
{
  esp_app_desc_t new_app;

  esp_err_t err = esp_https_ota_get_img_desc(handle, &new_app);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Could not read the new image description (%s)", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "New image: project '%s' version '%s'", new_app.project_name, new_app.version);
  post_event(OTA_API_EVENT_IMAGE_DESC, &new_app, sizeof(new_app));

  if (config->validate_cb && !config->validate_cb(&new_app, config->user_ctx))
  {
    ESP_LOGW(TAG, "Update refused by validate_cb");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  return ESP_OK;
}

/**
 * @brief Report progress through the callback and the event loop
 *
 * @param last_report_us Timestamp of the previous report, updated in place
 * @param force Report regardless of progress_interval_ms
 */
static void report_progress(esp_https_ota_handle_t handle, const ota_api_config_t *config, int total_bytes,
                            int64_t *last_report_us, bool force)
{
  if (!force && config->progress_interval_ms)
  {
    int64_t now_us = esp_timer_get_time();
    if (now_us - *last_report_us < (int64_t)config->progress_interval_ms * 1000)
    {
      return;
    }
    *last_report_us = now_us;
  }

  int read_bytes = esp_https_ota_get_image_len_read(handle);
  if (read_bytes < 0)
  {
    return;
  }

  ota_api_progress_t progress = {
    .bytes_read = (size_t)read_bytes,
    .total_bytes = total_bytes > 0 ? (size_t)total_bytes : 0,
    .percent = total_bytes > 0 ? (int)((int64_t)read_bytes * 100 / total_bytes) : -1,
  };

  if (config->progress_cb)
  {
    config->progress_cb(&progress, config->user_ctx);
  }
  post_event(OTA_API_EVENT_PROGRESS, &progress, sizeof(progress));
}

/**
 * @brief Drive the download to completion
 *
 * @return ESP_OK when the whole image has been received
 */
static esp_err_t download_image(esp_https_ota_handle_t handle, const ota_api_config_t *config)
{
  int total_bytes = esp_https_ota_get_image_size(handle);
  int64_t last_report_us = 0;
  esp_err_t err;

  if (total_bytes > 0)
    ESP_LOGI(TAG, "Image size: %d bytes", total_bytes);
  else
    ESP_LOGI(TAG, "Image size unknown, progress will be reported without a percentage");

  while (1)
  {
    err = esp_https_ota_perform(handle);
    if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
      break;

    if (abort_requested())
    {
      ESP_LOGW(TAG, "Update stopped on request");
      return ESP_ERR_NOT_FINISHED;
    }

    report_progress(handle, config, total_bytes, &last_report_us, false);
  }

  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Download failed (%s)", esp_err_to_name(err));
    return err;
  }

  // The loop can end with ESP_OK on a truncated response, so check explicitly
  if (!esp_https_ota_is_complete_data_received(handle))
  {
    ESP_LOGE(TAG, "Server closed the connection before sending the whole image");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  report_progress(handle, config, total_bytes, &last_report_us, true);
  return ESP_OK;
}

static esp_err_t run_update(const ota_api_config_t *config)
{
  esp_http_client_config_t http_config = {0};
  // Must outlive the update, which uses it for the whole download
  struct ifreq ifr = {0};

  esp_err_t err = build_http_config(config, &http_config, &ifr);
  if (err != ESP_OK)
    return err;

  esp_https_ota_config_t ota_config = {
    .http_config = &http_config,
  };
#ifdef CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
  ota_config.partial_http_download = config->partial_download;
  ota_config.max_http_request_size = (int)config->max_http_request_size;
#else
  if (config->partial_download)
  {
    ESP_LOGW(TAG, "partial_download ignored: CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD is disabled");
  }
#endif

  ESP_LOGI(TAG, "Downloading update from %s", config->url);

  esp_https_ota_handle_t handle = NULL;
  err = esp_https_ota_begin(&ota_config, &handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Could not start the update (%s)", esp_err_to_name(err));
    return err;
  }

  post_event(OTA_API_EVENT_STARTED, NULL, 0);

  err = check_incoming_image(handle, config);
  if (err == ESP_OK)
    err = download_image(handle, config);

  if (err != ESP_OK)
  {
    // Discards what was written and frees the handle
    esp_https_ota_abort(handle);
    return err;
  }

  err = esp_https_ota_finish(handle);
  if (err != ESP_OK)
  {
    if (err == ESP_ERR_OTA_VALIDATE_FAILED)
      ESP_LOGE(TAG, "The downloaded image did not pass validation");
    else
      ESP_LOGE(TAG, "Could not finish the update (%s)", esp_err_to_name(err));
  }

  return err;
}

esp_err_t ota_api_update(const ota_api_config_t *config)
{
  if (!config || !config->url)
    return ESP_ERR_INVALID_ARG;

  if (!claim_update_slot())
  {
    ESP_LOGE(TAG, "Another update is already running");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = run_update(config);

  release_update_slot();

  if (err == ESP_OK)
  {
    ESP_LOGI(TAG, "Update written, new firmware boots on next restart");
    post_event(OTA_API_EVENT_SUCCEEDED, NULL, 0);
  }
  else
  {
    ESP_LOGE(TAG, "Update failed (%s)", esp_err_to_name(err));
    post_event(OTA_API_EVENT_FAILED, &err, sizeof(err));
  }

  return err;
}

static void ota_task(void *arg)
{
  ota_api_config_t config = *(ota_api_config_t *)arg;
  free(arg);

  if (ota_api_update(&config) == ESP_OK)
  {
    ESP_LOGI(TAG, "OTA succeeded, rebooting...");
    esp_restart();
  }

  ESP_LOGE(TAG, "OTA task finished with error");
  vTaskDelete(NULL);
}

esp_err_t ota_api_start_task(const ota_api_config_t *config)
{
  if (!config || !config->url)
    return ESP_ERR_INVALID_ARG;

  ota_api_config_t *copy = malloc(sizeof(*copy));
  if (!copy)
    return ESP_ERR_NO_MEM;
  *copy = *config;

  uint32_t stack_size = config->task_stack_size ? config->task_stack_size : CONFIG_OTA_API_TASK_STACK_SIZE;
  UBaseType_t priority = config->task_priority ? config->task_priority : CONFIG_OTA_API_TASK_PRIORITY;

  if (xTaskCreate(&ota_task, "ota_api_task", stack_size, copy, priority, NULL) != pdPASS)
  {
    free(copy);
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t ota_api_abort(void)
{
  esp_err_t err = ESP_ERR_INVALID_STATE;

  portENTER_CRITICAL(&s_state_lock);
  if (s_running)
  {
    s_abort_requested = true;
    err = ESP_OK;
  }
  portEXIT_CRITICAL(&s_state_lock);

  return err;
}

bool ota_api_is_running(void)
{
  portENTER_CRITICAL(&s_state_lock);
  bool running = s_running;
  portEXIT_CRITICAL(&s_state_lock);

  return running;
}
