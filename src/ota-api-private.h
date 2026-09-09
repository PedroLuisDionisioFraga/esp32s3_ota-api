/**
 * @file ota-api-private.h
 * @brief Declarations shared between the component's source files
 *
 * This header lives in src/, which is not one of the component's
 * INCLUDE_DIRS, so nothing outside the component can reach it. The ota_api_
 * prefix on these names exists only to keep them from colliding at link time
 * now that they are no longer static — none of this is public API, and none
 * of it is covered by the compatibility promises of ota-api.h.
 *
 * @author Pedro Luis Dionisio Fraga
 * @date 2026
 */

#ifndef OTA_API_PRIVATE_H
#define OTA_API_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "ota-api.h"

/* ========================================================================== */
/*                       ota-api-state.c: THE UPDATE SLOT                     */
/* ========================================================================== */

/**
 * @brief Take ownership of the single update slot
 *
 * @return true if this caller may proceed, false if an update already runs
 */
bool ota_api_claim_update_slot(void);

/**
 * @brief Hand the update slot back and clear any pending abort request
 */
void ota_api_release_update_slot(void);

/**
 * @brief Whether ota_api_abort() has been called since the slot was claimed
 */
bool ota_api_abort_requested(void);

/* ========================================================================== */
/*                       ota-api-http.c: HTTP CLIENT SETUP                    */
/* ========================================================================== */

/**
 * @brief Build the HTTP client configuration shared by every update
 *
 * @param ifr Storage for the bound interface name; must outlive the update
 * @return ESP_ERR_INVALID_ARG when no way to validate the server is available
 */
esp_err_t ota_api_build_http_config(const ota_api_config_t *config, esp_http_client_config_t *http_config,
                                    struct ifreq *ifr);

/* ========================================================================== */
/*                        ota-api-report.c: REPORTING                         */
/* ========================================================================== */

/**
 * @brief Hand one event to the application callback, if it set one
 *
 * @return ESP_OK to carry on, or whatever verdict event_cb returned
 */
esp_err_t ota_api_dispatch_event(const ota_api_config_t *config, ota_api_event_id_t event_id, const void *data);

/**
 * @brief Report progress through the event loop and the callback
 *
 * @param last_report_us Timestamp of the previous report, updated in place
 * @param force Report regardless of progress_interval_ms
 * @return ESP_OK to keep downloading, or event_cb's verdict to stop
 */
esp_err_t ota_api_report_progress(esp_https_ota_handle_t handle, const ota_api_config_t *config, int total_bytes,
                                  int64_t *last_report_us, bool force);

#endif /* OTA_API_PRIVATE_H */
