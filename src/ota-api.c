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

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "ota-api";

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
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
               evt->header_value);
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

esp_err_t ota_api_update(const ota_api_config_t *config)
{
  if (!config || !config->url)
    return ESP_ERR_INVALID_ARG;

  esp_http_client_config_t http_config = {
    .url = config->url,
    .event_handler = http_event_handler,
    .keep_alive_enable = true,
    .skip_cert_common_name_check = config->skip_common_name_check,
  };

  if (config->cert_pem)
  {
    http_config.cert_pem = config->cert_pem;
  }
  else
  {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
#else
    ESP_LOGE(TAG,
             "cert_pem is NULL and MBEDTLS_CERTIFICATE_BUNDLE is disabled");
    return ESP_ERR_INVALID_ARG;
#endif
  }

  // Must outlive esp_https_ota(), which uses it for the whole download
  struct ifreq ifr = {0};
  if (config->bind_netif)
  {
    esp_netif_get_netif_impl_name(config->bind_netif, ifr.ifr_name);
    http_config.if_name = &ifr;
    ESP_LOGI(TAG, "Binding OTA connection to interface %s", ifr.ifr_name);
  }

  esp_https_ota_config_t ota_config = {
    .http_config = &http_config,
  };

  ESP_LOGI(TAG, "Downloading update from %s", config->url);
  esp_err_t err = esp_https_ota(&ota_config);
  if (err == ESP_OK)
    ESP_LOGI(TAG, "Update written, new firmware boots on next restart");
  else
    ESP_LOGE(TAG, "Update failed (%s)", esp_err_to_name(err));

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

  uint32_t stack_size = config->task_stack_size ? config->task_stack_size
                                                : CONFIG_OTA_API_TASK_STACK_SIZE;
  UBaseType_t priority =
    config->task_priority ? config->task_priority : CONFIG_OTA_API_TASK_PRIORITY;

  if (xTaskCreate(&ota_task, "ota_api_task", stack_size, copy, priority,
                  NULL) != pdPASS)
  {
    free(copy);
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}
