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
 * @brief Post an event, tolerating the absence of a default event loop
 *
 * An application that never creates the default loop simply does not get
 * events; that is not an error worth failing an update over.
 */
void ota_api_post_event(ota_api_event_id_t event_id, const void *data, size_t data_size);

/**
 * @brief Report progress through the callback and the event loop
 *
 * @param last_report_us Timestamp of the previous report, updated in place
 * @param force Report regardless of progress_interval_ms
 */
void ota_api_report_progress(esp_https_ota_handle_t handle, const ota_api_config_t *config, int total_bytes,
                             int64_t *last_report_us, bool force);

#endif /* OTA_API_PRIVATE_H */
