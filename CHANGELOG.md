# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-07-14

### Added

- Initial release, extracted from the ESP-IDF `simple_ota_example`.
- `ota_api_update()`: blocking HTTPS OTA update via `esp_https_ota`.
- `ota_api_start_task()`: background OTA task that reboots on success.
- `ota_api_config_t`: URL, custom PEM certificate or trusted root bundle,
  CN check skip, network interface binding, task stack/priority.
- `basic` example with Wi-Fi/Ethernet connection and embedded server certificate.
