/**
 * @file ota-api-report.c
 * @brief What an update tells the application, and what it hears back
 *
 * There is a single path here, ota_api_config_t::event_cb, and it is a plain
 * call rather than an event so that the application's answer can come back.
 * Observers that did not start the update are served by esp_https_ota itself,
 * which posts ESP_HTTPS_OTA_EVENT to the default event loop — which is why
 * nothing in this component touches that loop.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ota-api-private.h"

static const char *TAG = "ota-api";

esp_err_t ota_api_dispatch_event(const ota_api_config_t *config, ota_api_event_id_t event_id, const void *data)
{
  if (!config->event_cb)
    return ESP_OK;

  esp_err_t err = config->event_cb(event_id, data, config->user_ctx);

  /* Worded as what happened rather than what it caused: at SUCCEEDED and
   * FAILED the update is already over and the verdict goes nowhere.
   */
  if (err != ESP_OK)
    ESP_LOGW(TAG, "event_cb returned %s for event %d", esp_err_to_name(err), (int)event_id);

  return err;
}

esp_err_t ota_api_report_progress(esp_https_ota_handle_t handle, const ota_api_config_t *config, int total_bytes,
                                  int64_t *last_report_us, bool force)
{
  if (!force && config->progress_interval_ms)
  {
    int64_t now_us = esp_timer_get_time();
    // Throttled out: the callback never runs, so it cannot object either
    if (now_us - *last_report_us < (int64_t)config->progress_interval_ms * 1000)
      return ESP_OK;

    *last_report_us = now_us;
  }

  int read_bytes = esp_https_ota_get_image_len_read(handle);
  // Nothing to report is not a reason to stop the download
  if (read_bytes < 0)
    return ESP_OK;

  ota_api_progress_t progress = {
    .bytes_read = (size_t)read_bytes,
    .total_bytes = total_bytes > 0 ? (size_t)total_bytes : 0,
    .percent = total_bytes > 0 ? (int)((int64_t)read_bytes * 100 / total_bytes) : -1,
  };

  return ota_api_dispatch_event(config, OTA_API_EVENT_PROGRESS, &progress);
}
