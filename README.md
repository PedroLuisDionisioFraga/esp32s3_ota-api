# OTA API
[![Component Registry](https://components.espressif.com/components/pedroluisdionisiofraga/ota-api/badge.svg)](https://components.espressif.com/components/pedroluisdionisiofraga/ota-api)
[![Build Examples](https://img.shields.io/github/actions/workflow/status/PedroLuisDionisioFraga/esp32s3_ota-api/build.yml?branch=main&label=builds)](https://github.com/PedroLuisDionisioFraga/esp32s3_ota-api/actions/workflows/build.yml)
![GitHub repo size](https://img.shields.io/github/repo-size/PedroLuisDionisioFraga/esp32s3_ota-api)
[![License](https://img.shields.io/github/license/PedroLuisDionisioFraga/esp32s3_ota-api)](LICENSE)
![Targets](https://img.shields.io/badge/targets-ESP32%20%7C%20S2%20%7C%20S3%20%7C%20C3%20%7C%20C5%20%7C%20C6%20%7C%20H2%20%7C%20P4-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-%E2%89%A56.0-orange)

OTA API is a component for ESP-IDF that simplifies HTTPS over-the-air firmware updates for ESP32-based applications. Configure the firmware URL and an optional server certificate in a single struct, then run the update synchronously or in a background task that automatically reboots into the new firmware.

## Features

- Single-struct configuration (`ota_api_config_t`) — no `esp_http_client`/`esp_https_ota` boilerplate.
- Synchronous mode (`ota_api_update`) for callers that decide when to reboot.
- Background task mode (`ota_api_start_task`) that reboots into the new firmware on success.
- Download progress reported two ways: a direct callback and `OTA_API_EVENT` events on the default event loop.
- Inspect the incoming image before it is written (`validate_cb`) — refuse a version already installed without downloading it.
- Cancel an update in flight with `ota_api_abort()`, leaving the running firmware untouched.
- Optional ranged downloads (`partial_download`) for links that drop long transfers.
- Server validation via the trusted root certificate bundle (default) or a custom PEM certificate.
- Optional binding of the OTA connection to a specific network interface (Wi-Fi STA, Ethernet, Thread).
- Task stack size and priority configurable per call or via `menuconfig` (`OTA API Configuration`).

## Installation

Add the component to your project from the [ESP Component Registry](https://components.espressif.com/components/pedroluisdionisiofraga/ota-api):

```bash
idf.py add-dependency "pedroluisdionisiofraga/ota-api"
```

Or add it manually to your `main/idf_component.yml`:

```yaml
dependencies:
  pedroluisdionisiofraga/ota-api: "*"
```

The application must use an OTA-enabled partition table (e.g. `CONFIG_PARTITION_TABLE_TWO_OTA=y`).

## Usage

```c
#include "ota-api.h"

/* Must outlive the OTA task */
static ota_api_config_t ota_config = OTA_API_CONFIG_DEFAULT();

void app_main(void)
{
  // ... connect to Wi-Fi or Ethernet first ...

  ota_config.url = "https://example.com/firmware.bin";
  // ota_config.cert_pem = my_server_cert;  // NULL = trusted root bundle

  // Downloads the image, writes it to the next OTA partition and reboots.
  ESP_ERROR_CHECK(ota_api_start_task(&ota_config));
}
```

For full control over the reboot, call `ota_api_update(&ota_config)` instead: it blocks until the download finishes and returns `ESP_OK` once the new image is set as the boot partition.

To follow the download, set a progress callback (or register a handler for `OTA_API_EVENT` on the default event loop):

```c
static void on_progress(const ota_api_progress_t *p, void *ctx)
{
  printf("%d%% (%u/%u bytes)
", p->percent, (unsigned)p->bytes_read, (unsigned)p->total_bytes);
}

ota_config.progress_cb = on_progress;
ota_config.progress_interval_ms = 250;  // rate limit, 0 = every chunk
```

`ota_api_abort()` stops an update in progress; `ota_api_is_running()` reports whether one is active.

## Examples

| Example                          | Description                                                                                                                  |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| [basic](examples/basic)          | Full OTA flow with `ota_api_start_task()`: network connection, partition SHA-256 report and update from a configurable URL.   |
| [rollback](examples/rollback)    | Self-test after booting a new image, confirming it with `esp_ota_mark_app_valid_cancel_rollback()` or rolling back on failure. |
| [on_demand](examples/on_demand)  | Update triggered by a console command through the blocking `ota_api_update()`, with the reboot controlled by the application.  |
| [advanced](examples/advanced)    | Everything together, driven from a console: live percentage, abort mid-download, version check, ranged download and operator-confirmed rollback. |

[examples/common](examples/common) holds the shared local HTTPS server, which
also generates its own self-signed certificate — no `openssl` needed on Windows
or Linux.

Create a project from an example:

```bash
idf.py create-project-from-example "pedroluisdionisiofraga/ota-api:basic"
```

## API Reference

See [include/ota-api.h](include/ota-api.h) for the full public API.

## License

[MIT](LICENSE)
