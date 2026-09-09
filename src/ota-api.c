/**
 * @file ota-api.c
 * @brief HTTPS OTA firmware update implementation
 *
 * Drives the update itself: claim the slot, open the connection, read the
 * image header, download it and commit it. The supporting pieces live beside
 * this file — the run state in ota-api-state.c, the HTTP client setup in
 * ota-api-http.c, and the reporting in ota-api-report.c, all declared in
 * ota-api-private.h.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include "ota-api.h"

#include <stdlib.h>
#include <sys/socket.h>

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
// For ESP_ERR_OTA_VALIDATE_FAILED; callers that compare against it need this
// header too, so it stays out of the public API
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota-api-private.h"

static const char *TAG = "ota-api";

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
  ota_api_post_event(OTA_API_EVENT_IMAGE_DESC, &new_app, sizeof(new_app));

  if (config->validate_cb && !config->validate_cb(&new_app, config->user_ctx))
  {
    ESP_LOGW(TAG, "Update refused by validate_cb");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  return ESP_OK;
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

    if (ota_api_abort_requested())
    {
      ESP_LOGW(TAG, "Update stopped on request");
      return ESP_ERR_NOT_FINISHED;
    }

    ota_api_report_progress(handle, config, total_bytes, &last_report_us, false);
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

  ota_api_report_progress(handle, config, total_bytes, &last_report_us, true);
  return ESP_OK;
}

static esp_err_t run_update(const ota_api_config_t *config)
{
  esp_http_client_config_t http_config = {0};
  // Must outlive the update, which uses it for the whole download
  struct ifreq ifr = {0};

  esp_err_t err = ota_api_build_http_config(config, &http_config, &ifr);
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

  ota_api_post_event(OTA_API_EVENT_STARTED, NULL, 0);

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

  if (!ota_api_claim_update_slot())
  {
    ESP_LOGE(TAG, "Another update is already running");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = run_update(config);

  ota_api_release_update_slot();

  if (err == ESP_OK)
  {
    ESP_LOGI(TAG, "Update written, new firmware boots on next restart");
    ota_api_post_event(OTA_API_EVENT_SUCCEEDED, NULL, 0);
  }
  else
  {
    ESP_LOGE(TAG, "Update failed (%s)", esp_err_to_name(err));
    ota_api_post_event(OTA_API_EVENT_FAILED, &err, sizeof(err));
  }

  return err;
}

/**
 * @brief Body of the task spawned by ota_api_start_task()
 *
 * The whole point of the task is to own the blocking ota_api_update() call so
 * that the caller does not: the download would otherwise hold whichever task
 * asked for the update hostage until the last byte arrives.
 */
static void ota_task(void *arg)
{
  // Copied off the heap immediately so the config outlives the caller's frame
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
