/**
 * @file ota-api-trial.c
 * @brief The trial run a freshly booted image has to survive
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the bootloader starts a newly
 * written image in ESP_OTA_IMG_PENDING_VERIFY and waits to be told the firmware
 * works. Nothing here downloads anything: this is what happens after the reboot
 * an update caused, which is why it keeps its own state in its own file rather
 * than sharing the update slot in ota-api-state.c — different lifetime,
 * different invariant, no overlap.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ota-api.h"

static const char *TAG = "ota-api";

#define TRIAL_VER_MAX 32
#define NVS_NS        "ota_api"
#define KEY_RB_N      "trial_rb_n"
#define KEY_RB_VER    "trial_rb_ver"

/* Created by the first ota_api_trial_begin() and then kept for the life of the
 * boot. Stopping it is enough; deleting it would open a use-after-free against
 * a callback that may already be running, and one esp_timer is a rounding error
 * next to that risk.
 */
static esp_timer_handle_t s_trial_timer;
static ota_api_trial_expiry_cb_t s_expiry_cb;
static void *s_expiry_ctx;
static uint32_t s_trial_timeout_s;

static esp_err_t _rb_nvs_increment_expiry(const char *version)
{
  if (version == NULL || version[0] == '\0')
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return err;

  char stored[TRIAL_VER_MAX] = "";
  size_t len = sizeof(stored);
  uint8_t count = 0;

  if (nvs_get_str(handle, KEY_RB_VER, stored, &len) == ESP_OK && strcmp(stored, version) == 0)
    (void)nvs_get_u8(handle, KEY_RB_N, &count);
  else
    count = 0;

  if (count < 255)
    count++;

  err = nvs_set_str(handle, KEY_RB_VER, version);
  if (err == ESP_OK)
    err = nvs_set_u8(handle, KEY_RB_N, count);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static void _rb_nvs_clear(void)
{
  nvs_handle_t handle;
  if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK)
    return;
  (void)nvs_erase_key(handle, KEY_RB_N);
  (void)nvs_erase_key(handle, KEY_RB_VER);
  (void)nvs_commit(handle);
  nvs_close(handle);
}

/**
 * @brief Fires when nobody vouched for the image in time
 *
 * An unattended device must not be stranded on a build nobody confirmed, so the
 * absence of an answer is itself an answer: go back.
 */
static void trial_timeout(void *arg)
{
  (void)arg;

  /* Re-read the partition state rather than trust a flag: the image may have
   * been confirmed straight through esp_ota_mark_app_valid_cancel_rollback(),
   * or by ota_api_trial_confirm() in the instant before this was dispatched.
   * That instant is not fully closed — a confirm arriving after this check
   * still loses — but the worst outcome is a rollback an operator narrowly
   * missed preventing, and a real lock would only make confirm block behind a
   * task that is already rebooting.
   */
  if (!ota_api_is_on_trial())
    return;

  const esp_app_desc_t *app = esp_app_get_description();
  const char *ver = (app != NULL && app->version[0] != '\0') ? app->version : "?";

  bool count_expiry = false;
  if (s_expiry_cb != NULL)
  {
    ota_api_trial_expiry_info_t info = {
      .version = ver,
      .timeout_s = s_trial_timeout_s != 0 ? s_trial_timeout_s : CONFIG_OTA_API_TRIAL_TIMEOUT_S,
    };
    count_expiry = s_expiry_cb(&info, s_expiry_ctx);
  }

  if (count_expiry)
  {
    esp_err_t nvs_err = _rb_nvs_increment_expiry(ver);
    if (nvs_err != ESP_OK)
      ESP_LOGW(TAG, "trial expiry counter NVS increment failed: %s", esp_err_to_name(nvs_err));
  }

  ESP_LOGE(TAG, "Trial window expired with no confirmation, rolling back");
  esp_ota_mark_app_invalid_rollback_and_reboot();  // Does not return
}

bool ota_api_is_on_trial(void)
{
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();

  /* A factory partition is not an OTA slot and the call fails with
   * ESP_ERR_NOT_SUPPORTED. That is not an error, only an image that was never
   * on trial to begin with.
   */
  if (esp_ota_get_state_partition(running, &state) != ESP_OK)
    return false;

  return state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t ota_api_trial_begin(uint32_t timeout_s)
{
  /* Refused rather than ignored so a caller can tell the two apart, but a
   * caller that does not care may drop the return value: an image that is
   * already confirmed is never put back on the clock.
   */
  if (!ota_api_is_on_trial())
    return ESP_ERR_INVALID_STATE;

  if (!timeout_s)
    timeout_s = CONFIG_OTA_API_TRIAL_TIMEOUT_S;

  if (!s_trial_timer)
  {
    /* dispatch_method is left at its default, ESP_TIMER_TASK: the callback
     * rewrites otadata and reboots, and neither may happen in an ISR.
     */
    const esp_timer_create_args_t timer_args = {
      .callback = &trial_timeout,
      .name = "ota_api_trial",
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_trial_timer);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Could not create the trial timer (%s)", esp_err_to_name(err));
      return err;
    }
  }

  /* Returned as it comes: esp_timer_start_once() answers ESP_ERR_INVALID_STATE
   * when a window is already counting down, which is exactly what a second
   * ota_api_trial_begin() means.
   */
  s_trial_timeout_s = timeout_s;
  esp_err_t err = esp_timer_start_once(s_trial_timer, (uint64_t)timeout_s * 1000000);
  if (err != ESP_OK)
    return err;

  ESP_LOGI(TAG, "Image is on trial, rolling back in %" PRIu32 " s unless confirmed", timeout_s);
  return ESP_OK;
}

esp_err_t ota_api_trial_confirm(void)
{
  if (!ota_api_is_on_trial())
    return ESP_ERR_INVALID_STATE;

  /* Stopped before the image is marked valid, not after: the other order leaves
   * a window in which the timeout fires against an image that is already good.
   * The result is ignored because a caller that never armed a window still has
   * every right to confirm.
   */
  if (s_trial_timer)
    (void)esp_timer_stop(s_trial_timer);

  _rb_nvs_clear();

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Could not confirm the running image (%s)", esp_err_to_name(err));
  else
    ESP_LOGI(TAG, "Image confirmed, rollback cancelled");

  return err;
}

esp_err_t ota_api_trial_reject(void)
{
  /* Deliberately not gated on ota_api_is_on_trial(): going back to the previous
   * firmware is a legitimate thing to ask of a confirmed image too. Only the
   * absence of something to go back to can refuse it, and the IDF decides that.
   *
   * The timer is left alone as well. On success nothing survives to see it; on
   * failure it will expire and fail in the same way, which is more honest than
   * quietly disarming the safety net after a rollback that did not happen.
   */
  ESP_LOGW(TAG, "Rejecting the running image, rebooting into the previous firmware");

  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();

  // Reached only when there was no valid image to return to
  ESP_LOGE(TAG, "Rollback refused (%s), still running the current image", esp_err_to_name(err));
  return err;
}

esp_err_t ota_api_set_trial_expiry_cb(ota_api_trial_expiry_cb_t fn, void *ctx)
{
  if (fn == NULL)
    return ESP_ERR_INVALID_ARG;
  s_expiry_cb = fn;
  s_expiry_ctx = ctx;
  return ESP_OK;
}

esp_err_t ota_api_get_trial_expiry_rollback_count(const char *ver, uint8_t *out_n)
{
  if (out_n == NULL)
    return ESP_ERR_INVALID_ARG;
  *out_n = 0;
  if (ver == NULL || ver[0] == '\0')
    return ESP_OK;

  nvs_handle_t handle;
  if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK)
    return ESP_OK;

  char stored[TRIAL_VER_MAX] = "";
  size_t len = sizeof(stored);
  uint8_t count = 0;
  esp_err_t err = nvs_get_str(handle, KEY_RB_VER, stored, &len);
  if (err == ESP_OK && strcmp(stored, ver) == 0)
    (void)nvs_get_u8(handle, KEY_RB_N, &count);
  nvs_close(handle);

  *out_n = count;
  return ESP_OK;
}
