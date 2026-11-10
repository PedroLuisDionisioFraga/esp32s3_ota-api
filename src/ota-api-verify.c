/**
 * @file ota-api-verify.c
 * @brief Checking the downloaded image against the SHA-256 the caller expects
 *
 * esp_https_ota hands each chunk straight to flash and keeps no running hash of
 * the file, so the check reads the staging partition back once the download is
 * complete, before esp_https_ota_finish() makes it bootable. A mismatch costs a
 * flash read and an aborted update, never a boot into the wrong image.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include <sys/param.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "ota-api-private.h"
#include "psa/crypto.h"

static const char *TAG = "ota-api";

/* Read-back granularity: the update task's stack holds one of these */
#define VERIFY_CHUNK 512

esp_err_t ota_api_verify_sha256(esp_https_ota_handle_t handle, const uint8_t *expected)
{
  int image_len = esp_https_ota_get_image_len_read(handle);
  /* ota-api never names a staging partition, so esp_https_ota_begin() took
   * this one. With flash encryption, esp_partition_read() decrypts, and an app
   * image is 16-byte aligned, so no tail is still waiting in esp_ota's
   * partial-block buffer at this point.
   */
  const esp_partition_t *staging = esp_ota_get_next_update_partition(NULL);
  if (image_len <= 0 || !staging)
  {
    ESP_LOGE(TAG, "No downloaded image to check the SHA-256 of");
    return ESP_ERR_INVALID_STATE;
  }

  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS)
  {
    ESP_LOGE(TAG, "Could not start the SHA-256 of the image");
    return ESP_FAIL;
  }

  uint8_t chunk[VERIFY_CHUNK];
  for (size_t offset = 0; offset < (size_t)image_len;)
  {
    size_t len = MIN(sizeof(chunk), (size_t)image_len - offset);
    esp_err_t err = esp_partition_read(staging, offset, chunk, len);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Could not read the image back at offset %u (%s)", (unsigned)offset, esp_err_to_name(err));
      psa_hash_abort(&op);
      return err;
    }

    if (psa_hash_update(&op, chunk, len) != PSA_SUCCESS)
    {
      ESP_LOGE(TAG, "SHA-256 of the image failed at offset %u", (unsigned)offset);
      psa_hash_abort(&op);
      return ESP_FAIL;
    }

    offset += len;
  }

  // Compares in constant time; on any failure the operation still needs an abort
  psa_status_t status = psa_hash_verify(&op, expected, OTA_API_SHA256_LEN);
  if (status != PSA_SUCCESS)
  {
    psa_hash_abort(&op);
    if (status == PSA_ERROR_INVALID_SIGNATURE)
    {
      ESP_LOGE(TAG, "The downloaded image does not match the expected SHA-256");
      return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGE(TAG, "Could not finish the SHA-256 of the image (%d)", (int)status);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "SHA-256 of the downloaded image matches (%d bytes)", image_len);
  return ESP_OK;
}
