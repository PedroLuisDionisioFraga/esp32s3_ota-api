/**
 * @file main.c
 * @brief Basic OTA example using the ota-api component
 *
 * Connects to the network (Wi-Fi or Ethernet, selected via menuconfig),
 * prints the SHA-256 of the running partitions and starts an OTA update
 * from CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL. On success the device reboots
 * into the new firmware.
 */

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota-api.h"
#include "protocol_examples_common.h"

#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#define HASH_LEN 32
#define OTA_URL_SIZE 256

static const char *TAG = "ota_example";

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
/* The interface name value can refer to if_desc in esp_netif_defaults.h */
#if CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_ETH
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_ETH;
#elif CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_STA
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_STA;
#elif CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_THREAD
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_THREAD;
#endif
#endif

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

/* Passed to ota_api_start_task: must outlive the OTA task, so keep it static */
static ota_api_config_t ota_config = OTA_API_CONFIG_DEFAULT();
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN
static char url_buf[OTA_URL_SIZE];
#endif

static void print_sha256(const uint8_t *image_hash, const char *label)
{
  char hash_print[HASH_LEN * 2 + 1];
  hash_print[HASH_LEN * 2] = 0;
  for (int i = 0; i < HASH_LEN; ++i)
  {
    sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
  }
  ESP_LOGI(TAG, "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void)
{
  uint8_t sha_256[HASH_LEN] = {0};
  esp_partition_t partition;

  // get sha256 digest for bootloader
  partition.address = ESP_BOOTLOADER_OFFSET;
  partition.size = ESP_PARTITION_TABLE_OFFSET;
  partition.type = ESP_PARTITION_TYPE_APP;
  esp_partition_get_sha256(&partition, sha_256);
  print_sha256(sha_256, "SHA-256 for bootloader: ");

  // get sha256 digest for running partition
  esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
  print_sha256(sha_256, "SHA-256 for current firmware: ");
}

void app_main(void)
{
  ESP_LOGI(TAG, "OTA example app_main start");
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

  get_sha256_of_partitions();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* This helper function configures Wi-Fi or Ethernet, as selected in
   * menuconfig. Read "Establishing Wi-Fi or Ethernet Connection" section in
   * examples/protocols/README.md for more information about this function.
   */
  ESP_ERROR_CHECK(example_connect());

#if CONFIG_EXAMPLE_CONNECT_WIFI
  /* Ensure to disable any WiFi power save mode, this allows best throughput
   * and hence timings for overall OTA operation.
   */
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif  // CONFIG_EXAMPLE_CONNECT_WIFI

  ota_config.url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL;
#ifndef CONFIG_EXAMPLE_USE_CERT_BUNDLE
  ota_config.cert_pem = (const char *)server_cert_pem_start;
#endif
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
  ota_config.skip_common_name_check = true;
#endif

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
  ota_config.bind_netif = get_example_netif_from_desc(bind_interface_name);
  if (ota_config.bind_netif == NULL)
  {
    ESP_LOGE(TAG, "Can't find netif from interface description");
    abort();
  }
#endif

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN
  if (strcmp(ota_config.url, "FROM_STDIN") == 0)
  {
    example_configure_stdin_stdout();
    fgets(url_buf, OTA_URL_SIZE, stdin);
    int len = strlen(url_buf);
    url_buf[len - 1] = '\0';
    ota_config.url = url_buf;
  }
  else
  {
    ESP_LOGE(TAG, "Configuration mismatch: wrong firmware upgrade image url");
    abort();
  }
#endif

  ESP_ERROR_CHECK(ota_api_start_task(&ota_config));

  while (1)
  {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
