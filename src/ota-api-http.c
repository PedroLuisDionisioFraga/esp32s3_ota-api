/**
 * @file ota-api-http.c
 * @brief HTTP client setup for the update download
 *
 * Everything the component knows about esp_http_client is confined here: how
 * the server is authenticated, which interface the connection is bound to,
 * and the tracing of the transfer's own events.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include <strings.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ota-api-private.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "ota-api";

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
  switch (evt->event_id)
  {
    case HTTP_EVENT_ERROR:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
      break;
    }

    case HTTP_EVENT_ON_CONNECTED:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    }

    case HTTP_EVENT_HEADER_SENT:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    }

    case HTTP_EVENT_ON_HEADER:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
      break;
    }

    case HTTP_EVENT_ON_HEADERS_COMPLETE:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
      break;
    }

    case HTTP_EVENT_ON_DATA:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      break;
    }

    case HTTP_EVENT_ON_FINISH:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;
    }

    case HTTP_EVENT_DISCONNECTED:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
      break;
    }

    case HTTP_EVENT_REDIRECT:
    {
      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      break;
    }

    default:
      break;
  }

  return ESP_OK;
}

/**
 * @brief Whether the URL asks for plain HTTP rather than HTTPS
 *
 * Scheme names are case-insensitive, so this cannot be a plain strncmp.
 */
static bool is_plain_http(const char *url)
{
  return strncasecmp(url, "http://", 7) == 0;
}

esp_err_t ota_api_build_http_config(const ota_api_config_t *config, esp_http_client_config_t *http_config,
                                    struct ifreq *ifr)
{
  http_config->url = config->url;
  http_config->event_handler = http_event_handler;
  http_config->keep_alive_enable = true;
  http_config->skip_cert_common_name_check = config->skip_common_name_check;

  if (is_plain_http(config->url))
  {
    /* Nothing to configure: server verification is a TLS notion and there is
     * no TLS here. Demanding a certificate for an http:// URL would reject a
     * transfer that is insecure rather than misconfigured — whether insecure
     * is acceptable is esp_https_ota's call, not this component's.
     */
#ifdef CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
    ESP_LOGW(TAG, "Plain HTTP: the image is neither encrypted nor authenticated in transit");
#else
    ESP_LOGE(TAG, "URL is plain HTTP but CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP is disabled");
    return ESP_ERR_INVALID_ARG;
#endif
  }
  else if (config->cert_pem)
  {
    http_config->cert_pem = config->cert_pem;
  }
  else
  {
#ifdef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    /* No certificate given, but esp-tls is globally configured to skip server
     * verification (a lab-only option). Attaching the root bundle here would
     * arm a verify callback that rejects a self-signed server before that
     * global skip takes effect, so leave cert_pem and crt_bundle_attach unset
     * and let the handshake run without a trust anchor.
     */
    http_config->skip_cert_common_name_check = true;
    ESP_LOGW(TAG, "cert_pem is NULL and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is set; OTA server not authenticated");
#elif defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
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
