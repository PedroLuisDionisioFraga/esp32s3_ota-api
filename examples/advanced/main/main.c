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
 * Everything the application does about an update lives in one callback,
 * on_ota_event_cb(), and everything the operator does lives in the console
 * commands. There is no OTA task, no semaphore and no trial state machine here:
 * ota_api_start_task() owns the first and ota_api_trial_begin() the last, which
 * is what makes this file short enough to lift into a real project.
 *
 * - Live progress. A throttled callback prints a percentage bar with transfer
 *   rate and ETA, redrawn in place.
 * - Version check before writing. The same callback inspects the incoming image
 *   header at OTA_API_EVENT_IMAGE_DESC and returns ESP_ERR_OTA_VALIDATE_FAILED
 *   for firmware whose version matches the one already running, unless 'force'
 *   is passed.
 * - Acting between the write and the reboot. At OTA_API_EVENT_SUCCEEDED the
 *   transfer is over but the restart has not happened yet, so that is where the
 *   network is shut down cleanly and the operator is told what comes next.
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
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota-api.h"
#include "protocol_examples_common.h"

#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#define OTA_URL_SIZE       256
#define PROGRESS_BAR_WIDTH 30

static const char *TAG = "ota_advanced";

static char s_ota_url[OTA_URL_SIZE];
static volatile bool s_force_update;

/* Raised from the moment 'ota' is accepted until the update is done with the
 * device. ota_api_is_running() cannot stand in for this: it goes false as soon
 * as the update ends, which under ota_api_start_task() is a whole second before
 * the reboot — the SUCCEEDED handler below disconnects and waits in there — and
 * it is not yet true in the instant between ota_api_start_task() returning and
 * the task it spawned claiming the slot. Both windows would let a second 'ota'
 * rewrite s_ota_url under a download that is still reading it.
 */
static volatile bool s_ota_pending;

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
 * @brief Everything the application does about an update, in one function
 *
 * Runs on the OTA task. During the transfer — STARTED, IMAGE_DESC, PROGRESS —
 * it may only format a line and return, because the download waits for it. At
 * SUCCEEDED and FAILED the transfer is over and it may take its time, which is
 * what lets this example use ota_api_start_task() and still control what
 * happens between the image being written and the reboot.
 *
 * The return value is what makes it more than a notification: ESP_OK lets the
 * update carry on, anything else stops it.
 */
static esp_err_t on_ota_event_cb(ota_api_event_id_t event_id, const void *data, void *user_ctx)
{
  (void)user_ctx;

  switch (event_id)
  {
    case OTA_API_EVENT_STARTED:
    {
      /* The connection is up and the first byte is about to arrive, which is
       * where a download rate should be measured from — the TLS handshake that
       * preceded it is not download time. Safe to stamp here because event_cb
       * is a direct call: STARTED is dispatched on this task before any
       * OTA_API_EVENT_PROGRESS can be.
       */
      s_download_start_us = esp_timer_get_time();
      break;
    }

    case OTA_API_EVENT_IMAGE_DESC:
    {
      const esp_app_desc_t *new_app = (const esp_app_desc_t *)data;
      const esp_app_desc_t *running = esp_app_get_description();

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

    case OTA_API_EVENT_SUCCEEDED:
    {
      /* Taking a second here is deliberate. The transfer is over, the socket is
       * closed and the update slot is already released; the only thing still
       * pending is ota_api_start_task()'s esp_restart(), which does not happen
       * until this returns. That makes this callback the hook the example needs
       * to shut the network down and say what is about to happen — the reason
       * it no longer keeps an OTA worker task of its own.
       *
       * The leading newline closes the progress bar, which stops without one.
       */
      printf("\n  update stored. Rebooting into it for its trial run.\n");
      example_disconnect();
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      break;
    }

    case OTA_API_EVENT_FAILED:
    {
      /* Reached by this callback's own verdict too: the
       * ESP_ERR_OTA_VALIDATE_FAILED returned at OTA_API_EVENT_IMAGE_DESC above
       * comes back here as the outcome of the update it stopped.
       */
      s_ota_pending = false;
      printf("\n  update did not complete: %s\n", esp_err_to_name(*(const esp_err_t *)data));
      break;
    }

    default:
      break;
  }

  return ESP_OK;
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

  if (s_ota_pending || ota_api_is_running())
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
  s_force_update = force;
  s_ota_pending = true;

  /* ota_api_start_task() copies the struct, so this local dies here safely. It
   * does not copy what the pointers point at, which is why url is the file
   * scope s_ota_url and not a stack buffer.
   */
  ota_api_config_t ota_config = OTA_API_CONFIG_DEFAULT();
  ota_config.url = s_ota_url;
  ota_config.event_cb = on_ota_event_cb;
  ota_config.progress_interval_ms = CONFIG_EXAMPLE_PROGRESS_INTERVAL_MS;
  // Ranged requests keep a flaky link from costing the whole transfer
  ota_config.partial_download = true;
  ota_config.max_http_request_size = CONFIG_EXAMPLE_HTTP_REQUEST_SIZE;
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
  ota_config.skip_common_name_check = true;
#endif

  esp_err_t err = ota_api_start_task(&ota_config);
  if (err != ESP_OK)
  {
    s_ota_pending = false;
    printf("Could not start the update: %s\n", esp_err_to_name(err));
    return 1;
  }

  printf("Starting update from %s%s\n", s_ota_url, force ? " (forced)" : "");
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

  // Stops the countdown as well, so nothing is left to disarm here
  esp_err_t err = ota_api_trial_confirm();
  if (err != ESP_OK)
  {
    printf("Could not confirm the image: %s\n", esp_err_to_name(err));
    return 1;
  }

  printf("Image confirmed, rollback cancelled\n");
  return 0;
}

static int cmd_rollback(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  printf("Rolling back to the previous firmware...\n");
  esp_err_t err = ota_api_trial_reject();

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
  printf("awaiting confirm  : %s\n", ota_api_is_on_trial() ? "yes" : "no");
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

  esp_console_repl_t *repl = NULL;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "ota>";
  esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

  register_commands();

  ESP_ERROR_CHECK(esp_console_start_repl(repl));

  /* Deliberately last: the banner is the first thing the operator should see,
   * and the console has to be up to accept 'confirm'. No state check is needed
   * around it — ota_api_trial_begin() refuses an image that is not on trial.
   */
  if (ota_api_trial_begin(0) == ESP_OK)
  {
    printf("\n");
    printf("  ==========================================================\n");
    printf("  This firmware is ON TRIAL and has not been confirmed yet.\n");
    printf("  Type 'confirm' to keep it, or 'rollback' to go back now.\n");
    printf("  Without a confirmation it rolls back in %d seconds.\n", CONFIG_OTA_API_TRIAL_TIMEOUT_S);
    printf("  ==========================================================\n\n");
  }
  else
  {
    ESP_LOGI(TAG, "Console ready. Type 'help' for the command list");
  }
}
