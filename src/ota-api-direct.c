/**
 * @file ota-api-direct.c
 * @brief Single-GET update that never follows a redirect
 *
 * esp_https_ota treats 301/302/303/307/308 as "process again" and calls
 * esp_http_client_set_redirection() itself, so disable_auto_redirect on the
 * HTTP client does not stop it from contacting the Location target. When the
 * caller forbids redirects this file takes over the whole transfer:
 *
 *   one GET -> status must be 200 -> image header checked (magic, chip,
 *   descriptor, event_cb veto) -> esp_ota_begin -> stream to flash while
 *   hashing -> completeness and SHA-256 -> esp_ota_end (validation) ->
 *   pre_activate_cb -> esp_ota_set_boot_partition
 *
 * Nothing is bootable until the very last step, so any failure leaves the
 * running firmware as it was.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "ota-api-private.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "ota-api";

#define DIRECT_CHUNK 4096
/* esp_image_header_t + first esp_image_segment_header_t + esp_app_desc_t */
#define IMAGE_PREFIX_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

typedef struct
{
  esp_http_client_handle_t client;
  esp_ota_handle_t ota;
  bool ota_open;
  psa_hash_operation_t hash;
  bool hash_open;
  uint8_t *buf;
} direct_ctx_t;

/** Read until @p len bytes or the end of the body; returns the count or a negative error. */
static int read_full(esp_http_client_handle_t client, uint8_t *dst, int len)
{
  int got = 0;
  while (got < len)
  {
    int n = esp_http_client_read(client, (char *)dst + got, len - got);
    if (n < 0)
      return n;
    if (n == 0)
      break;
    got += n;
  }
  return got;
}

static esp_err_t check_prefix(const ota_api_config_t *config, const uint8_t *prefix, int len)
{
  if (len < (int)IMAGE_PREFIX_LEN)
  {
    ESP_LOGE(TAG, "Image shorter than its header (%d bytes)", len);
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  const esp_image_header_t *hdr = (const esp_image_header_t *)prefix;
  if (hdr->magic != ESP_IMAGE_HEADER_MAGIC)
  {
    ESP_LOGE(TAG, "Not an application image (magic 0x%02x)", hdr->magic);
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }
  if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
  {
    // Same code esp_https_ota returns for a chip mismatch
    ESP_LOGE(TAG, "Mismatch chip id, expected %d, found %d", CONFIG_IDF_FIRMWARE_CHIP_ID, (int)hdr->chip_id);
    return ESP_ERR_INVALID_VERSION;
  }

  esp_app_desc_t desc;
  memcpy(&desc, prefix + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
  if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD)
  {
    ESP_LOGE(TAG, "Image has no application descriptor");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  ESP_LOGI(TAG, "New image: project '%s' version '%s'", desc.project_name, desc.version);
  return ota_api_dispatch_event(config, OTA_API_EVENT_IMAGE_DESC, &desc);
}

static esp_err_t write_chunk(direct_ctx_t *ctx, const uint8_t *data, int len, bool hashing)
{
  if (hashing && psa_hash_update(&ctx->hash, data, (size_t)len) != PSA_SUCCESS)
  {
    ESP_LOGE(TAG, "SHA-256 of the image failed");
    return ESP_FAIL;
  }
  esp_err_t err = esp_ota_write(ctx->ota, data, (size_t)len);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Flash write failed (%s)", esp_err_to_name(err));
  return err;
}

static esp_err_t transfer(const ota_api_config_t *config, direct_ctx_t *ctx)
{
  esp_err_t err = esp_http_client_open(ctx->client, 0);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Could not connect (%s)", esp_err_to_name(err));
    return err;
  }

  // The client was created with the connect timeout; the reads get their own
  if (config->connect_timeout_ms && config->timeout_ms)
    esp_http_client_set_timeout_ms(ctx->client, (int)config->timeout_ms);

  int64_t content_length = esp_http_client_fetch_headers(ctx->client);
  int status = esp_http_client_get_status_code(ctx->client);
  if (content_length < 0)
  {
    ESP_LOGE(TAG, "Could not read the response headers");
    return ESP_FAIL;
  }
  if (status != 200)
  {
    if (status >= 300 && status < 400)
      ESP_LOGE(TAG, "HTTP %d: redirects are not followed", status);
    else
      ESP_LOGE(TAG, "HTTP %d", status);
    return ESP_FAIL;
  }

  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (!part)
  {
    ESP_LOGE(TAG, "No OTA partition to write to");
    return ESP_ERR_NOT_FOUND;
  }
  int total = esp_http_client_is_chunked_response(ctx->client) ? 0 : (int)content_length;
  if (total > 0)
    ESP_LOGI(TAG, "Image size: %d bytes", total);
  else
    ESP_LOGI(TAG, "Image size unknown, progress will be reported without a percentage");
  if (total > 0 && (size_t)total > part->size)
  {
    // Refused before anything is erased
    ESP_LOGE(TAG, "Image of %d bytes does not fit the %u-byte partition", total, (unsigned)part->size);
    return ESP_ERR_INVALID_SIZE;
  }

  err = ota_api_dispatch_event(config, OTA_API_EVENT_STARTED, NULL);
  if (err != ESP_OK)
    return err;

  ctx->buf = malloc(DIRECT_CHUNK);
  if (!ctx->buf)
    return ESP_ERR_NO_MEM;

  int got = read_full(ctx->client, ctx->buf, IMAGE_PREFIX_LEN);
  if (got < 0)
  {
    ESP_LOGE(TAG, "Download failed while reading the image header");
    return ESP_FAIL;
  }
  if (got < (int)IMAGE_PREFIX_LEN && total > 0 && got < total)
  {
    // Cut before the header was whole: a broken transfer, the image was never seen
    ESP_LOGE(TAG, "Server closed the connection before sending the whole image (%d/%d bytes)", got, total);
    return ESP_ERR_INVALID_RESPONSE;
  }
  err = check_prefix(config, ctx->buf, got);
  if (err != ESP_OK)
    return err;

  const bool hashing = config->sha256 != NULL;
  if (hashing)
  {
    ctx->hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&ctx->hash, PSA_ALG_SHA_256) != PSA_SUCCESS)
    {
      ESP_LOGE(TAG, "Could not start the SHA-256 of the image");
      return ESP_FAIL;
    }
    ctx->hash_open = true;
  }

  ESP_LOGI(TAG, "Writing to <%s> partition at offset 0x%" PRIx32, part->label, part->address);
  err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ctx->ota);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
    return err;
  }
  ctx->ota_open = true;

  err = write_chunk(ctx, ctx->buf, got, hashing);
  int received = got;
  int64_t last_report_us = 0;

  while (err == ESP_OK)
  {
    if (ota_api_abort_requested())
    {
      ESP_LOGW(TAG, "Update stopped on request");
      return ESP_ERR_NOT_FINISHED;
    }

    int n = esp_http_client_read(ctx->client, (char *)ctx->buf, DIRECT_CHUNK);
    if (n < 0)
    {
      ESP_LOGE(TAG, "Download failed after %d bytes", received);
      return ESP_FAIL;
    }
    if (n == 0)
      break;
    if (total > 0 && received + n > total)
    {
      // A broken transfer, not a bad image: the caller may simply try again
      ESP_LOGE(TAG, "Server sent more than its Content-Length");
      return ESP_ERR_INVALID_RESPONSE;
    }
    if ((size_t)(received + n) > part->size)
    {
      ESP_LOGE(TAG, "Image larger than the partition");
      return ESP_ERR_INVALID_SIZE;
    }

    err = write_chunk(ctx, ctx->buf, n, hashing);
    received += n;
    if (err == ESP_OK)
      err = ota_api_report_progress(config, received, total, &last_report_us, false);
  }
  if (err != ESP_OK)
    return err;

  if (!esp_http_client_is_complete_data_received(ctx->client) || (total > 0 && received != total))
  {
    // Same here: ESP_ERR_OTA_VALIDATE_FAILED is kept for an image that is itself invalid
    ESP_LOGE(TAG, "Server closed the connection before sending the whole image (%d/%d bytes)", received, total);
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Forced so a 100% line always lands; also the last chance to refuse
  err = ota_api_report_progress(config, received, total, &last_report_us, true);
  if (err != ESP_OK)
    return err;

  if (hashing)
  {
    psa_status_t st = psa_hash_verify(&ctx->hash, config->sha256, OTA_API_SHA256_LEN);
    ctx->hash_open = st != PSA_SUCCESS;  // verify finishes the operation only on success
    if (st == PSA_ERROR_INVALID_SIGNATURE)
    {
      ESP_LOGE(TAG, "The downloaded image does not match the expected SHA-256");
      return ESP_ERR_INVALID_CRC;
    }
    if (st != PSA_SUCCESS)
    {
      ESP_LOGE(TAG, "Could not finish the SHA-256 of the image (%d)", (int)st);
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SHA-256 of the downloaded image matches (%d bytes)", received);
  }

  err = esp_ota_end(ctx->ota);
  ctx->ota_open = false;  // esp_ota_end frees the handle whatever it returns
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "The downloaded image did not pass validation (%s)", esp_err_to_name(err));
    return err == ESP_ERR_OTA_VALIDATE_FAILED ? err : ESP_ERR_OTA_VALIDATE_FAILED;
  }

  if (config->pre_activate_cb)
  {
    err = config->pre_activate_cb(config->user_ctx);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "pre_activate_cb refused activation (%s)", esp_err_to_name(err));
      return err;
    }
  }

  err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Could not set the boot partition (%s)", esp_err_to_name(err));
  return err;
}

esp_err_t ota_api_run_direct(const ota_api_config_t *config)
{
  esp_http_client_config_t http_config = {0};
  struct ifreq ifr = {0};

  esp_err_t err = ota_api_build_http_config(config, &http_config, &ifr);
  if (err != ESP_OK)
    return err;
  http_config.disable_auto_redirect = true;
  http_config.keep_alive_enable = false;
  if (config->connect_timeout_ms)
    http_config.timeout_ms = (int)config->connect_timeout_ms;

  if (config->partial_download)
    ESP_LOGW(TAG, "partial_download ignored: disable_auto_redirect downloads in a single GET");

  ESP_LOGI(TAG, "Downloading update from %s (single GET, no redirects)", config->url);

  direct_ctx_t ctx = {0};
  ctx.client = esp_http_client_init(&http_config);
  if (!ctx.client)
    return ESP_ERR_NO_MEM;

  err = transfer(config, &ctx);

  if (ctx.ota_open)
    esp_ota_abort(ctx.ota);
  if (ctx.hash_open)
    psa_hash_abort(&ctx.hash);
  free(ctx.buf);
  esp_http_client_close(ctx.client);
  esp_http_client_cleanup(ctx.client);
  return err;
}
