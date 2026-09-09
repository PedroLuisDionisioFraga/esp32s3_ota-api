/**
 * @file main.c
 * @brief Advanced OTA example using the ota-api component
 *
 * Puts the whole update lifecycle under console control, with the device
 * reporting what it is doing at every step:
 *
 *     ota [url] [force]   start an update, printing live download progress
 *     abort               cancel the update in flight
 *     confirm             keep the image currently on trial
 *     rollback            reject it and go back to the previous firmware
 *     version             running project, version, partition and trial state
 *     status              whether an update is running
 *
 * It combines what the other examples show separately and adds what only the
 * step-by-step esp_https_ota flow makes possible:
 *
 * - Live progress. A throttled callback prints a percentage bar with transfer
 *   rate and ETA, redrawn in place.
 * - Both reporting paths at once. The percentage, the image description and
 *   the version decision come from ota_api_config_t::event_cb, the single
 *   callback the component invokes for every event, while the lifecycle is
 *   also watched through ESP_HTTPS_OTA_EVENT — a base esp_https_ota posts to
 *   the default event loop by itself, with no help from this component. The
 *   two are shown side by side because they are good at different things: only
 *   the callback carries a payload and can answer back, only the event loop
 *   reaches code that did not start the update.
 * - Version check before writing. That same callback inspects the incoming
 *   image header at OTA_API_EVENT_IMAGE_DESC and returns
 *   ESP_ERR_OTA_VALIDATE_FAILED for firmware whose version matches the one
 *   already running, unless 'force' is passed.
 * - Abort mid-download. 'abort' asks the component to unwind cleanly, leaving
 *   the running firmware untouched.
 * - Ranged downloads. The image is fetched over several HTTP requests so a
 *   link that drops long transfers can still complete an update.
 * - Operator-driven rollback. After booting a new image the device does NOT
 *   confirm itself: it waits for 'confirm'. Without it, an unattended device
 *   rolls back automatically once the trial window expires.
 *
 * Requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and
 * CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD, both set in sdkconfig.defaults.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
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
#define PROGRESS_BAR_WIDTH    30

static const char *TAG = "ota_advanced";

static SemaphoreHandle_t s_ota_request;
static char s_ota_url[OTA_URL_SIZE];
static volatile bool s_ota_requested;
static volatile bool s_force_update;

/* Set while this boot is a trial run that nobody has confirmed yet */
static volatile bool s_awaiting_confirmation;
static esp_timer_handle_t s_trial_timer;

/* Download rate is measured against the moment the transfer started */
static int64_t s_download_start_us;

/* ========================================================================== */
/*                        PROGRESS AND VETO VIA CALLBACK                      */
/* ========================================================================== */

static void print_progress_bar(const ota_api_progress_t *progress)
{
  double elapsed_s = (esp_timer_get_time() - s_download_start_us) / 1000000.0;
  double rate_kbs = elapsed_s > 0 ? (progress->bytes_read / 1024.0) / elapsed_s : 0;

  if (progress->percent < 0)
  {
    // No Content-Length from the server, so there is no percentage to show
    printf("\r  downloading %u KB at %.1f KB/s      ", (unsigned)(progress->bytes_read / 1024), rate_kbs);
    fflush(stdout);
    return;
  }

  char bar[PROGRESS_BAR_WIDTH + 1];
  int filled = progress->percent * PROGRESS_BAR_WIDTH / 100;
  memset(bar, '#', filled);
  memset(bar + filled, '.', PROGRESS_BAR_WIDTH - filled);
  bar[PROGRESS_BAR_WIDTH] = '\0';

  double remaining_s = rate_kbs > 0 ? ((progress->total_bytes - progress->bytes_read) / 1024.0) / rate_kbs : 0;

  printf("\r  [%s] %3d%%  %u/%u KB  %.1f KB/s  ETA %ds   ",
         bar,
         progress->percent,
         (unsigned)(progress->bytes_read / 1024),
         (unsigned)(progress->total_bytes / 1024),
         rate_kbs,
         (int)remaining_s);
  fflush(stdout);
}

/**
 * @brief Single entry point for everything the component reports
 *
 * Runs on the OTA worker task in the middle of the download, so it may only
 * format a line and return — no blocking work belongs here. What makes it more
 * than a notification is the return value: ESP_OK lets the update carry on,
 * anything else stops it and comes straight back out of ota_api_update().
 *
 * Lifecycle logging deliberately does NOT happen here. on_https_ota_event()
 * does it from the IDF's own event loop, which keeps both mechanisms visible
 * side by side without the console printing the same thing twice.
 */
static esp_err_t on_ota_event_cb(ota_api_event_id_t event_id, const void *data, void *user_ctx)
{
  (void)user_ctx;

  switch (event_id)
  {
    case OTA_API_EVENT_IMAGE_DESC:
    {
      const esp_app_desc_t *new_app = (const esp_app_desc_t *)data;
      const esp_app_desc_t *running = esp_app_get_description();

      /* The description only reaches the application here: the IDF's own
       * GET_IMG_DESC event announces the step but carries no payload.
       */
      printf("\n  offered : %s version %s (built %s %s)\n",
             new_app->project_name,
             new_app->version,
             new_app->date,
             new_app->time);
      printf("  running : %s version %s\n", running->project_name, running->version);

      if (!s_force_update && strncmp(new_app->version, running->version, sizeof(new_app->version)) == 0)
      {
        printf("  same version already running, refusing. Use 'ota <url> force' to install anyway.\n");
        // Costs nothing: only the header has been downloaded at this point
        return ESP_ERR_OTA_VALIDATE_FAILED;
      }
      break;
    }

    case OTA_API_EVENT_PROGRESS:
    {
      print_progress_bar((const ota_api_progress_t *)data);
      break;
    }

    default:
      break;
  }

  return ESP_OK;
}

/* ========================================================================== */
/*                     LIFECYCLE VIA THE IDF'S OWN EVENT LOOP                 */
/* ========================================================================== */

/**
 * @brief Handles ESP_HTTPS_OTA_EVENT from the default event loop
 *
 * This base belongs to esp_https_ota, not to this component: the IDF posts it
 * on its own during any update, so following an update costs nothing but a
 * handler registration. That is what lets unrelated parts of an application —
 * a display task, an MQTT reporter — watch an update they did not start,
 * without the component having to duplicate the notifications.
 *
 * What these events cannot do is carry a decision back, or a percentage:
 * WRITE_FLASH reports bytes written with no total. Both of those come from
 * on_ota_event_cb() instead, which is the point of having the two side by
 * side.
 */
static void on_https_ota_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
  (void)arg;
  (void)base;
  (void)event_data;

  switch (event_id)
  {
    case ESP_HTTPS_OTA_START:
    {
      ESP_LOGI(TAG, "event: update started");
      break;
    }

    case ESP_HTTPS_OTA_CONNECTED:
    {
      ESP_LOGI(TAG, "event: connected to the server");
      break;
    }

    case ESP_HTTPS_OTA_GET_IMG_DESC:
    {
      // The description itself reaches on_ota_event_cb(); this is just the step
      ESP_LOGI(TAG, "event: reading the image header");
      break;
    }

    case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION:
    {
      printf("\n");
      ESP_LOGI(TAG, "event: boot partition updated");
      break;
    }

    case ESP_HTTPS_OTA_FINISH:
    {
      ESP_LOGI(TAG, "event: update finished");
      break;
    }

    case ESP_HTTPS_OTA_ABORT:
    {
      printf("\n");
      ESP_LOGW(TAG, "event: update aborted");
      break;
    }

    default:
      break;
  }
}

/* ========================================================================== */
/*                          TRIAL RUN AND ROLLBACK                            */
/* ========================================================================== */

/**
 * @brief Fires when nobody confirmed the image in time
 *
 * An unattended device must not be left waiting forever: an image nobody
 * vouches for goes back to the previous firmware on its own.
 */
static void trial_timeout(void *arg)
{
  (void)arg;

  if (!s_awaiting_confirmation)
    return;

  ESP_LOGE(TAG, "No confirmation within %d s, rolling back", CONFIG_EXAMPLE_TRIAL_TIMEOUT_S);
  esp_ota_mark_app_invalid_rollback_and_reboot();  // Does not return
}

static void start_trial_window(void)
{
  s_awaiting_confirmation = true;

  printf("\n");
  printf("  ==========================================================\n");
  printf("  This firmware is ON TRIAL and has not been confirmed yet.\n");
  printf("  Type 'confirm' to keep it, or 'rollback' to go back now.\n");
  printf("  Without a confirmation it rolls back in %d seconds.\n", CONFIG_EXAMPLE_TRIAL_TIMEOUT_S);
  printf("  ==========================================================\n\n");

  const esp_timer_create_args_t timer_args = {
    .callback = &trial_timeout,
    .name = "ota_trial",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_trial_timer));
  ESP_ERROR_CHECK(esp_timer_start_once(s_trial_timer, (uint64_t)CONFIG_EXAMPLE_TRIAL_TIMEOUT_S * 1000000));
}

/* ========================================================================== */
/*                               OTA WORKER                                   */
/* ========================================================================== */

static void ota_worker_task(void *arg)
{
  (void)arg;

  ota_api_config_t ota_config = OTA_API_CONFIG_DEFAULT();
  ota_config.event_cb = on_ota_event_cb;
  ota_config.progress_interval_ms = CONFIG_EXAMPLE_PROGRESS_INTERVAL_MS;
  // Ranged requests keep a flaky link from costing the whole transfer
  ota_config.partial_download = true;
  ota_config.max_http_request_size = CONFIG_EXAMPLE_HTTP_REQUEST_SIZE;
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
  ota_config.skip_common_name_check = true;
#endif

  while (1)
  {
    xSemaphoreTake(s_ota_request, portMAX_DELAY);

    ota_config.url = s_ota_url;
    ESP_LOGI(TAG, "Update requested: %s", ota_config.url);

    /* Stamped here rather than from the STARTED event: events are delivered
     * asynchronously, so the first progress callbacks can arrive before the
     * handler runs and would compute the rate against an unset timestamp.
     */
    s_download_start_us = esp_timer_get_time();

    esp_err_t err = ota_api_update(&ota_config);

    s_ota_requested = false;
    s_force_update = false;

    if (err != ESP_OK)
    {
      // The device is untouched and still serving the console: retry at will
      printf("  update did not complete: %s\n", esp_err_to_name(err));
      continue;
    }

    printf("  update stored. Rebooting into it for its trial run.\n");
    example_disconnect();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
  }
}

/* ========================================================================== */
/*                             CONSOLE COMMANDS                               */
/* ========================================================================== */

static int cmd_ota(int argc, char **argv)
{
  const char *url = CONFIG_EXAMPLE_DEFAULT_FIRMWARE_UPGRADE_URL;
  bool force = false;

  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "force") == 0)
      force = true;
    else
      url = argv[i];
  }

  if (s_ota_requested || ota_api_is_running())
  {
    printf("An update is already running. Use 'abort' to stop it.\n");
    return 1;
  }

  if (strlen(url) >= sizeof(s_ota_url))
  {
    printf("URL is longer than the %d character limit\n", (int)sizeof(s_ota_url) - 1);
    return 1;
  }

  snprintf(s_ota_url, sizeof(s_ota_url), "%s", url);
  printf("Starting update from %s%s\n", s_ota_url, force ? " (forced)" : "");

  /* Claimed before returning: otherwise a second 'ota' typed immediately after
   * would pass the check above and overwrite the URL the worker is reading.
   */
  s_force_update = force;
  s_ota_requested = true;
  xSemaphoreGive(s_ota_request);
  return 0;
}

static int cmd_abort(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  esp_err_t err = ota_api_abort();
  if (err != ESP_OK)
  {
    printf("No update is running\n");
    return 1;
  }

  printf("Stop requested, unwinding...\n");
  return 0;
}

static int cmd_confirm(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  if (!s_awaiting_confirmation)
  {
    printf("This firmware is not on trial, nothing to confirm\n");
    return 1;
  }

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK)
  {
    printf("Could not confirm the image: %s\n", esp_err_to_name(err));
    return 1;
  }

  s_awaiting_confirmation = false;
  esp_timer_stop(s_trial_timer);
  printf("Image confirmed, rollback cancelled\n");
  return 0;
}

static int cmd_rollback(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  printf("Rolling back to the previous firmware...\n");
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();

  // Only reached when there is no previous image to go back to
  printf("Rollback not possible: %s\n", esp_err_to_name(err));
  return 1;
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

  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK)
    printf("state       : %s\n", state == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY (on trial)" : "confirmed");
  else
    printf("state       : not tracked (factory partition)\n");

  return 0;
}

static int cmd_status(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  printf("update running    : %s\n", ota_api_is_running() ? "yes" : "no");
  printf("awaiting confirm  : %s\n", s_awaiting_confirmation ? "yes" : "no");
  return 0;
}

static void register_commands(void)
{
  const esp_console_cmd_t commands[] = {
    {
      .command = "ota",
      .help = "Start an update: ota [url] [force]. Without a url the configured one is used",
      .hint = "[url] [force]",
      .func = &cmd_ota,
    },
    {
      .command = "abort",
      .help = "Cancel the update in progress",
      .hint = NULL,
      .func = &cmd_abort,
    },
    {
      .command = "confirm",
      .help = "Keep the firmware currently on trial",
      .hint = NULL,
      .func = &cmd_confirm,
    },
    {
      .command = "rollback",
      .help = "Reject this firmware and reboot into the previous one",
      .hint = NULL,
      .func = &cmd_rollback,
    },
    {
      .command = "version",
      .help = "Print the running firmware version, partition and trial state",
      .hint = NULL,
      .func = &cmd_version,
    },
    {
      .command = "status",
      .help = "Print whether an update is running or awaiting confirmation",
      .hint = NULL,
      .func = &cmd_status,
    },
  };

  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
  {
    ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
  }

  ESP_ERROR_CHECK(esp_console_register_help_command());
}

/* ========================================================================== */
/*                                  STARTUP                                   */
/* ========================================================================== */

void app_main(void)
{
  ESP_LOGI(TAG, "OTA advanced example app_main start");

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
  /* esp_https_ota posts this base by itself during any update, so watching one
   * costs nothing more than registering here.
   */
  ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, &on_https_ota_event, NULL));

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

  /* Deliberately last: the trial banner is the first thing the operator should
   * see, and the console has to be up to accept 'confirm'.
   */
  esp_ota_img_states_t ota_state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY)
  {
    start_trial_window();
  }
  else
  {
    ESP_LOGI(TAG, "Console ready. Type 'help' for the command list");
  }
}
