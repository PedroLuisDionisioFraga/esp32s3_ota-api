/**
 * @file ota-api-state.c
 * @brief The single update slot and the lock guarding it
 *
 * Every function that touches the run state lives here and nowhere else, so
 * the invariant that two updates never overlap can be checked by reading one
 * file rather than by trusting the rest of the component.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#include "freertos/FreeRTOS.h"
#include "ota-api-private.h"
#include "ota-api.h"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_running;
static bool s_abort_requested;

bool ota_api_claim_update_slot(void)
{
  bool claimed = false;

  portENTER_CRITICAL(&s_state_lock);
  if (!s_running)
  {
    s_running = true;
    s_abort_requested = false;
    claimed = true;
  }
  portEXIT_CRITICAL(&s_state_lock);

  return claimed;
}

void ota_api_release_update_slot(void)
{
  portENTER_CRITICAL(&s_state_lock);
  s_running = false;
  s_abort_requested = false;
  portEXIT_CRITICAL(&s_state_lock);
}

bool ota_api_abort_requested(void)
{
  portENTER_CRITICAL(&s_state_lock);
  bool requested = s_abort_requested;
  portEXIT_CRITICAL(&s_state_lock);

  return requested;
}

esp_err_t ota_api_abort(void)
{
  esp_err_t err = ESP_ERR_INVALID_STATE;

  portENTER_CRITICAL(&s_state_lock);
  if (s_running)
  {
    s_abort_requested = true;
    err = ESP_OK;
  }
  portEXIT_CRITICAL(&s_state_lock);

  return err;
}

bool ota_api_is_running(void)
{
  portENTER_CRITICAL(&s_state_lock);
  bool running = s_running;
  portEXIT_CRITICAL(&s_state_lock);

  return running;
}
