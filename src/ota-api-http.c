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

#ifdef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
#include "mbedtls/ssl.h"
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

#ifdef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
/**
 * @brief Stand in for a certificate bundle without trusting anything
 *
 * esp_https_ota_begin() refuses a configuration in which cert_pem,
 * use_global_ca_store and crt_bundle_attach are all unset. It reads them off
 * esp_http_client_config_t before the client exists, so nothing downstream can
 * satisfy it and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY never gets to apply.
 *
 * A non-NULL crt_bundle_attach clears that gate. esp-tls then arms
 * MBEDTLS_SSL_VERIFY_REQUIRED and hands this hook the config it just armed,
 * which is where the verification comes back out: no CA chain is installed and
 * no verify callback is registered, so the handshake completes against an
 * unauthenticated peer -- which is what the option asked for.
 */
static esp_err_t skip_server_verify_attach(void *conf)
{
  mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_NONE);
  return ESP_OK;
}
#endif

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
    /* Not a bundle: a hook that satisfies esp_https_ota's server-verification
     * gate and then turns verification off (see skip_server_verify_attach).
     * With CONFIG_MBEDTLS_CERTIFICATE_BUNDLE disabled esp_http_client drops
     * the hook with a misleading "use_crt_bundle configured but not enabled"
     * error and esp-tls reaches the same place on its own, so the lab case
     * still works either way.
     */
    http_config->crt_bundle_attach = skip_server_verify_attach;
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
