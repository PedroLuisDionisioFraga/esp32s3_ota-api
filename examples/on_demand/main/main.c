/**
 * @file main.c
 * @brief On-demand OTA example using the ota-api component
 *
 * Instead of updating at boot, the device connects to the network and then
 * waits for an explicit command on the serial console:
 *
 *     ota [url]   download and install a firmware image
 *     version     print the running firmware version and partition
 *
 * The update runs through ota_api_update(), the blocking API, on a dedicated
 * worker task — the console task's stack is far too small for a TLS download.
 * That is the point of this example: a failed update comes back as an
 * esp_err_t, the application keeps running and another attempt can be made,
 * and on success the application shuts the network down itself before
 * rebooting. With ota_api_start_task(), used by the basic example, the
 * component owns that reboot.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota-api.h"
#include "protocol_examples_common.h"

#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#define OTA_URL_SIZE          256
#define OTA_WORKER_STACK_SIZE 8192
#define OTA_WORKER_PRIORITY   5

static const char *TAG = "ota_on_demand";

static SemaphoreHandle_t s_ota_request;
static char s_ota_url[OTA_URL_SIZE];
static volatile bool s_ota_running;

/**
 * @brief Runs the blocking update whenever the console asks for one
 *
 * A task of its own because ota_api_update() blocks for the whole download and
 * needs far more stack than the console REPL task provides.
 */
static void ota_worker_task(void *arg)
{
  ota_api_config_t ota_config = OTA_API_CONFIG_DEFAULT();
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
  ota_config.skip_common_name_check = true;
#endif

  while (1)
  {
    xSemaphoreTake(s_ota_request, portMAX_DELAY);

    ota_config.url = s_ota_url;
    ESP_LOGI(TAG, "Starting update from %s", ota_config.url);

    esp_err_t err = ota_api_update(&ota_config);
    if (err != ESP_OK)
    {
      // A failed update is just a return code here: the device stays up
      ESP_LOGE(TAG, "Update failed (%s). Still running, type 'ota <url>' to retry", esp_err_to_name(err));
      s_ota_running = false;
      continue;
    }

    /* The image is written and set as the boot partition. Nothing reboots
     * until we say so, which is the moment to stop peripherals, flush state
     * or tell a server the device is going down.
     */
    ESP_LOGI(TAG, "Update written, shutting the connection down before rebooting");
    example_disconnect();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
  }
}

static int cmd_ota(int argc, char **argv)
{
  const char *url = (argc > 1) ? argv[1] : CONFIG_EXAMPLE_DEFAULT_FIRMWARE_UPGRADE_URL;

  if (s_ota_running)
  {
    printf("An update is already running\n");
    return 1;
  }

  if (strlen(url) >= sizeof(s_ota_url))
  {
    printf("URL is longer than the %d character limit\n", (int)sizeof(s_ota_url) - 1);
    return 1;
  }

  snprintf(s_ota_url, sizeof(s_ota_url), "%s", url);
  printf("Requesting update from %s\n", s_ota_url);

  /* Claimed here rather than in the worker: the flag has to be set before this
   * command returns, or a second 'ota' typed immediately after would pass the
   * check above and overwrite the URL the worker is about to read.
   */
  s_ota_running = true;
  xSemaphoreGive(s_ota_request);
  return 0;
}

static int cmd_version(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_app_desc_t *app_desc = esp_app_get_description();

  printf("project     : %s\n", app_desc->project_name);
  printf("version     : %s\n", app_desc->version);
  printf("compiled    : %s %s\n", app_desc->date, app_desc->time);
  printf("idf version : %s\n", app_desc->idf_ver);
  printf("partition   : %s at offset 0x%08" PRIx32 "\n", running->label, running->address);
  return 0;
}

static void register_commands(void)
{
  const esp_console_cmd_t ota_cmd = {
    .command = "ota",
    .help = "Download and install a firmware image, using the configured URL when none is given",
    .hint = "[url]",
    .func = &cmd_ota,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&ota_cmd));

  const esp_console_cmd_t version_cmd = {
    .command = "version",
    .help = "Print the running firmware version and partition",
    .hint = NULL,
    .func = &cmd_version,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&version_cmd));

  ESP_ERROR_CHECK(esp_console_register_help_command());
}

void app_main(void)
{
  ESP_LOGI(TAG, "OTA on-demand example app_main start");

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

  s_ota_request = xSemaphoreCreateBinary();
  ESP_ERROR_CHECK(s_ota_request != NULL ? ESP_OK : ESP_ERR_NO_MEM);

  BaseType_t created =
    xTaskCreate(&ota_worker_task, "ota_worker", OTA_WORKER_STACK_SIZE, NULL, OTA_WORKER_PRIORITY, NULL);
  ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

  esp_console_repl_t *repl = NULL;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "ota>";
  esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

  register_commands();

  ESP_ERROR_CHECK(esp_console_start_repl(repl));

  ESP_LOGI(TAG, "Console ready. Type 'help' for the command list, or 'ota' to update now");
}
