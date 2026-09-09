# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.3] - 2026-09-09

### Fixed

- The 0.3.2 fix was incomplete on ESP-IDF 6.0. Dropping the certificate bundle for a
  `NULL` `cert_pem` left `cert_pem`, `use_global_ca_store` and `crt_bundle_attach` all
  unset, which is exactly the configuration `esp_https_ota_begin()` refuses: the update
  died with `No option for server verification is enabled in esp_http_client config.`
  before the TLS handshake, so esp-tls never got far enough to apply its global skip.
  With `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` the component now installs a
  `crt_bundle_attach` hook that clears that gate and then sets `MBEDTLS_SSL_VERIFY_NONE`,
  so a lab server with a self-signed certificate completes the download.
  `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` is **not** required — a plain `http://` URL is still
  refused without it. Builds without `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` are
  unchanged.

## [0.3.2] - 2026-09-09

### Fixed

- With `cert_pem` left `NULL`, the HTTP client always attached the trusted root
  certificate bundle, ignoring `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`. Against a
  server with a self-signed certificate the download failed at the TLS handshake
  (`esp-x509-crt-bundle: No matching trusted root certificate found`, surfacing as
  `ESP_ERR_HTTP_CONNECT`). When that option is set the component now leaves both
  `cert_pem` and `crt_bundle_attach` unset and forces `skip_cert_common_name_check`,
  so the handshake completes without a trust anchor. Builds without the option are
  unaffected — a `NULL` `cert_pem` still attaches the bundle.

## [0.3.1] - 2026-09-09

### Changed

- The Doxygen comments in `include/ota-api.h` are condensed to a one-line brief,
  a short body limited to safety-relevant behaviour, and terse `@param` entries.
  The enumerated `@return` error codes are kept. Comments only — no API or
  behaviour change.

## [0.3.0] - 2026-09-08

### Changed

- **Breaking.** `ota_api_config_t::progress_cb` and `ota_api_config_t::validate_cb`
  are replaced by a single `event_cb` of type `ota_api_event_cb_t`, invoked for
  all five `ota_api_event_id_t` values. Returning `ESP_OK` lets the update
  continue; any other value stops it and becomes the return value of
  `ota_api_update()`. `user_ctx` and `progress_interval_ms` are unchanged.
- The advanced example is rebuilt on `ota_api_start_task()` and the trial API: its
  OTA task, semaphore and trial state machine are gone, leaving one event
  callback plus the console commands. `EXAMPLE_TRIAL_TIMEOUT_S` is replaced by
  the component's `CONFIG_OTA_API_TRIAL_TIMEOUT_S`.
- The rollback example uses the trial API instead of calling `esp_ota_ops`
  directly.
- The implementation is split across `src/ota-api-state.c` (the update slot and
  its lock), `src/ota-api-http.c` (HTTP client setup), `src/ota-api-report.c`
  (reporting) and `src/ota-api-trial.c` (the trial run), leaving `src/ota-api.c`
  with the update state machine. Internal only — no effect on the public API.

### Added

- Plain `http://` URLs are recognised and no longer require a certificate or
  the trusted root bundle, which they had no use for: server verification is a
  TLS notion and there is no TLS on such a URL. Whether an unauthenticated
  transfer is acceptable is now decided by `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`,
  esp_https_ota's own option, instead of by a rule of this component's.

  **Behaviour change to check if you download over HTTP.** Previously, with
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` enabled, an `http://` URL worked without
  `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`: the component attached the bundle, which
  made esp_https_ota's `is_server_verification_enabled()` return true and
  skipped the safety gate even though nothing was ever verified. That accident
  is gone — an `http://` URL now needs `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` and
  is refused with `ESP_ERR_INVALID_ARG` otherwise.
- Trial run and rollback as public API: `ota_api_is_on_trial()`,
  `ota_api_trial_begin()`, `ota_api_trial_confirm()` and `ota_api_trial_reject()`,
  with `CONFIG_OTA_API_TRIAL_TIMEOUT_S` as the default deadline. This was
  previously hand-written in the advanced example; every project using the
  component now gets the safety net that stops a device from being stranded on a
  broken update. `confirm` and `reject` work without `begin`, so a self-test that
  finishes inside `app_main` needs no timer at all.
- A guarantee worth relying on: `event_cb` at `OTA_API_EVENT_SUCCEEDED` runs
  *before* `ota_api_start_task()` restarts the device, and — unlike the events
  that arrive mid-transfer — it may take its time. That is the hook an
  application needs to close connections or warn an operator between the image
  being written and the reboot, and it removes the reason to hand-roll an update
  task around the blocking `ota_api_update()`.
- `event_cb` can stop an update at `OTA_API_EVENT_STARTED` and
  `OTA_API_EVENT_PROGRESS`, which `validate_cb` could not: an update can now be
  refused before the header is read, or abandoned mid-download and at the final
  100% report, right up until `esp_https_ota_finish()` makes the image bootable.

### Removed

- `ota_api_progress_cb_t` and `ota_api_validate_cb_t`. See the migration below.
- **Breaking.** The `OTA_API_EVENT` event base and everything posted under it.
  `esp_https_ota` already posts its own `ESP_HTTPS_OTA_EVENT` to the default
  event loop during any update — `START`, `CONNECTED`, `GET_IMG_DESC`,
  `WRITE_FLASH`, `UPDATE_BOOT_PARTITION`, `FINISH`, `ABORT` and more — so this
  component was duplicating notifications onto the same loop. An application
  that watched `OTA_API_EVENT` should register its handler for
  `ESP_HTTPS_OTA_EVENT` instead. What that base cannot give is a percentage
  (its `WRITE_FLASH` reports bytes written with no total) or a way to stop an
  update; both are what `event_cb` is for.
- `esp_event` from the component's public `REQUIRES`, now that no event base is
  declared in `ota-api.h`. An application that relied on the header pulling in
  `esp_event.h` must include it itself.
- `ota_api_event_id_t` values are now callback arguments only; the enum stays,
  and the `OTA_API_EVENT_*` names are unchanged.

### Migration from 0.2.0

`ESP_ERR_OTA_VALIDATE_FAILED` is declared in `esp_ota_ops.h`, which the public
header deliberately does not pull in — include it where you return the constant.

An application that only watched `OTA_API_EVENT` on the default event loop
changes one line, the event base it registers for, and then switches on
`esp_https_ota_event_t` instead of `ota_api_event_id_t`:

```c
/* 0.2.0 */
esp_event_handler_register(OTA_API_EVENT, ESP_EVENT_ANY_ID, &handler, NULL);

/* 0.3.0 — same default event loop, base owned by esp_https_ota */
esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, &handler, NULL);
```

```c
/* 0.2.0 */
static void on_progress(const ota_api_progress_t *progress, void *user_ctx)
{
  draw_bar(progress->percent);
}

static bool on_validate(const esp_app_desc_t *new_app, void *user_ctx)
{
  return strcmp(new_app->version, esp_app_get_description()->version) != 0;
}

config.progress_cb = on_progress;
config.validate_cb = on_validate;
```

```c
/* 0.3.0 */
#include "esp_ota_ops.h"  // for ESP_ERR_OTA_VALIDATE_FAILED

static esp_err_t on_ota_event(ota_api_event_id_t event_id, const void *data, void *user_ctx)
{
  switch (event_id)
  {
    case OTA_API_EVENT_PROGRESS:
    {
      draw_bar(((const ota_api_progress_t *)data)->percent);
      break;
    }

    case OTA_API_EVENT_IMAGE_DESC:
    {
      const esp_app_desc_t *new_app = (const esp_app_desc_t *)data;
      if (strcmp(new_app->version, esp_app_get_description()->version) == 0)
        return ESP_ERR_OTA_VALIDATE_FAILED;  // what `return false` used to mean
      break;
    }

    default:
      break;
  }

  return ESP_OK;  // what `void` and `return true` used to mean
}

config.event_cb = on_ota_event;
```

## [0.2.0] - 2026-09-03

### Added

- Download progress reporting, available two ways: `ota_api_config_t::progress_cb`
  (with `progress_interval_ms` rate limiting and `user_ctx`) and `OTA_API_EVENT`
  events posted to the default event loop (`STARTED`, `IMAGE_DESC`, `PROGRESS`,
  `SUCCEEDED`, `FAILED`).
- `ota_api_config_t::validate_cb`: inspect the incoming image's `esp_app_desc_t`
  after its header is read and before anything is written, to refuse a version
  that is already installed.
- `ota_api_abort()` and `ota_api_is_running()`: cooperatively cancel an update
  in flight, leaving the running firmware untouched.
- `ota_api_config_t::partial_download` and `max_http_request_size`: fetch the
  image over several ranged HTTP requests, which survives links that drop long
  transfers. Requires `CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD`.
- `advanced` example: the full lifecycle under console control — live progress
  bar with rate and ETA, `abort`, version check, ranged download, and a trial
  run confirmed with `confirm` or reverted with `rollback`, plus automatic
  rollback when nobody confirms in time.
- `rollback` example: self-test of a newly booted image with
  `esp_ota_mark_app_valid_cancel_rollback()` and automatic rollback when the
  diagnostic fails, using the blocking `ota_api_update()`.
- `on_demand` example: update triggered by an `ota [url]` console command, with
  the reboot performed by the application after its own cleanup.
- `examples/common` with the HTTPS server, certificates and firmware images
  shared by all examples.
- `examples/common/ota_server.py` now generates its own self-signed certificate
  (with the server IP in `subjectAltName`) using the `cryptography` package, so
  no `openssl` binary is needed on Windows or Linux.

### Changed

- `ota_api_update()` now drives `esp_https_ota_begin`/`perform`/`finish` instead
  of the one-shot `esp_https_ota()`, which is what makes progress, validation
  and aborting possible. Existing callers are unaffected: the new fields default
  to off.
- `ota_api_update()` rejects a second concurrent update with
  `ESP_ERR_INVALID_STATE`.
- The component now publicly requires `esp_event` and `esp_app_format`.
- `examples/basic` reads its server certificate from `examples/common/certs/`.

### Fixed

- `examples/basic` was shipped to the Component Registry without the
  certificate it embeds at build time, so `create-project-from-example` produced
  a project that did not compile.
- The format CI job could never pass: `ubuntu-latest` installs clang-format 18,
  which cannot parse `AlignEscapedNewlines: LeftWithLastLine` in `.clang-format`
  and therefore rejected every file. The version is now pinned via pip.

## [0.1.0] - 2026-07-14

### Added

- Initial release, extracted from the ESP-IDF `simple_ota_example`.
- `ota_api_update()`: blocking HTTPS OTA update via `esp_https_ota`.
- `ota_api_start_task()`: background OTA task that reboots on success.
- `ota_api_config_t`: URL, custom PEM certificate or trusted root bundle,
  CN check skip, network interface binding, task stack/priority.
- `basic` example with Wi-Fi/Ethernet connection and embedded server certificate.
