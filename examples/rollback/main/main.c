/**
 * @file main.c
 * @brief OTA rollback example using the ota-api component
 *
 * Shows the self-test flow that protects a device from a broken update. When
 * the bootloader starts a freshly written image for the first time it leaves
 * it in ESP_OTA_IMG_PENDING_VERIFY: the application must run a diagnostic and
 * then either confirm the image with esp_ota_mark_app_valid_cancel_rollback()
 * or reject it with esp_ota_mark_app_invalid_rollback_and_reboot(), which
 * reboots back into the previous firmware. Without that confirmation the
 * bootloader rolls back on the next reset.
 *
 * The diagnostic used here is whether the device manages to join the network.
 * Replace run_diagnostic() with whatever "this build actually works" means for
 * your product: a sensor answering, a server handshake, a GPIO jumper.
 *
 * Unlike the basic example, this one calls ota_api_update() — the blocking
 * API — so the application decides when to reboot instead of the component.
 *
 * Requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, set in sdkconfig.defaults.
 */

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota-api.h"
#include "protocol_examples_common.h"

#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

static const char *TAG = "ota_rollback";

/**
 * @brief Decide whether the running image is healthy
 *
 * Called only while the image is in ESP_OTA_IMG_PENDING_VERIFY. Returning
 * false makes the device roll back to the previous firmware.
 */
static bool run_diagnostic(esp_err_t connect_err)
{
  if (connect_err != ESP_OK)
  {
    ESP_LOGE(TAG, "Diagnostic FAILED: could not join the network (%s)", esp_err_to_name(connect_err));
    return false;
  }

  ESP_LOGI(TAG, "Diagnostic PASSED: network is up");
  return true;
}

static void log_running_image(void)
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_app_desc_t *app_desc = esp_app_get_description();

  ESP_LOGI(TAG,
           "Running '%s' version '%s' from partition '%s'",
           app_desc->project_name,
           app_desc->version,
           running->label);
}

void app_main(void)
{
  ESP_LOGI(TAG, "OTA rollback example app_main start");

  // Initialize NVS.
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    // 1.OTA app partition table has a smaller NVS partition size than the
    // non-OTA partition table. This size mismatch may cause NVS initialization
    // to fail.
    // 2.NVS partition contains data in new format and cannot be recognized by
    // this version of code. If this happens, we erase NVS partition and
    // initialize NVS again.
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  log_running_image();

  /* An image booted for the first time stays in PENDING_VERIFY until the
   * application confirms it. Running from the factory partition, or from a
   * build without CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, there is no such
   * state and esp_ota_get_state_partition() fails — that is not an error.
   */
  bool pending_verify = false;
  esp_ota_img_states_t ota_state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
  {
    pending_verify = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    ESP_LOGI(TAG, "Partition state: %s", pending_verify ? "PENDING_VERIFY, self-test required" : "already confirmed");
  }
  else
  {
    ESP_LOGI(TAG, "Partition has no OTA state, self-test not required");
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* This helper function configures Wi-Fi or Ethernet, as selected in
   * menuconfig. Read "Establishing Wi-Fi or Ethernet Connection" section in
   * examples/protocols/README.md for more information about this function.
   * The return value is not checked here: it is the diagnostic input below.
   */
  esp_err_t connect_err = example_connect();

  if (pending_verify)
  {
    if (run_diagnostic(connect_err))
    {
      ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
      ESP_LOGI(TAG, "Image confirmed, rollback cancelled");
    }
    else
    {
      ESP_LOGE(TAG, "Rejecting this image and rebooting into the previous firmware");
      esp_ota_mark_app_invalid_rollback_and_reboot();  // Does not return
    }
  }

  // Reached only when the image is confirmed, so a failed connection is fatal
  ESP_ERROR_CHECK(connect_err);

#if CONFIG_EXAMPLE_CONNECT_WIFI
  /* Ensure to disable any WiFi power save mode, this allows best throughput
   * and hence timings for overall OTA operation.
   */
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif  // CONFIG_EXAMPLE_CONNECT_WIFI

  /* ota_api_update() is synchronous, so the config only has to outlive this
   * call — a local is enough. The task mode used by the basic example is the
   * one that needs a static config.
   */
  ota_api_config_t ota_config = OTA_API_CONFIG_DEFAULT();
  ota_config.url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL;
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
  ota_config.skip_common_name_check = true;
#endif

  ESP_LOGI(TAG, "Starting update from %s", ota_config.url);
  err = ota_api_update(&ota_config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Update failed (%s), staying on the current firmware", esp_err_to_name(err));
    while (1)
    {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }

  /* The image is written and set as the boot partition, but not yet confirmed:
   * the run after this reboot is the one that has to pass the self-test above.
   */
  ESP_LOGI(TAG, "Update written. Rebooting into the new image for its self-test");
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  esp_restart();
}
