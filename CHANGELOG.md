# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
