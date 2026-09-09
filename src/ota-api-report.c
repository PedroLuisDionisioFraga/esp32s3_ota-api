/**
 * @file ota-api-report.c
 * @brief Progress and lifecycle reporting
 *
 * Holds the two paths an application can follow an update through: the
 * OTA_API_EVENT event loop, for observers that did not start the update, and
 * the direct callback, for the caller that did.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ota-api-private.h"

static const char *TAG = "ota-api";

ESP_EVENT_DEFINE_BASE(OTA_API_EVENT);

void ota_api_post_event(ota_api_event_id_t event_id, const void *data, size_t data_size)
{
  esp_err_t err = esp_event_post(OTA_API_EVENT, event_id, (void *)data, data_size, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    ESP_LOGD(TAG, "Could not post event %d (%s)", (int)event_id, esp_err_to_name(err));
}

void ota_api_report_progress(esp_https_ota_handle_t handle, const ota_api_config_t *config, int total_bytes,
                             int64_t *last_report_us, bool force)
{
  if (!force && config->progress_interval_ms)
  {
    int64_t now_us = esp_timer_get_time();
    if (now_us - *last_report_us < (int64_t)config->progress_interval_ms * 1000)
      return;

    *last_report_us = now_us;
  }

  int read_bytes = esp_https_ota_get_image_len_read(handle);
  if (read_bytes < 0)
    return;

  ota_api_progress_t progress = {
    .bytes_read = (size_t)read_bytes,
    .total_bytes = total_bytes > 0 ? (size_t)total_bytes : 0,
    .percent = total_bytes > 0 ? (int)((int64_t)read_bytes * 100 / total_bytes) : -1,
  };

  if (config->progress_cb)
    config->progress_cb(&progress, config->user_ctx);

  ota_api_post_event(OTA_API_EVENT_PROGRESS, &progress, sizeof(progress));
}
