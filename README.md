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
- One callback for the whole update (`event_cb`): progress with a percentage, the incoming image's description, and the outcome. No second reporting path to choose between — for a decoupled observer, `esp_https_ota` already posts `ESP_HTTPS_OTA_EVENT` to the default event loop by itself.
- Stop an update from inside that callback: return anything but `ESP_OK` and it unwinds, leaving the running firmware untouched — refuse a version already installed before a single byte is written.
- Cancel an update in flight with `ota_api_abort()`, leaving the running firmware untouched.
- Automatic rollback for an image nobody vouches for: `ota_api_trial_begin()` arms a deadline, `ota_api_trial_confirm()` keeps the firmware, `ota_api_trial_reject()` goes back. A device that boots a broken update recovers on its own.
- Optional ranged downloads (`partial_download`) for links that drop long transfers.
- Server validation via the trusted root certificate bundle (default) or a custom PEM certificate — or none at all on a lab build (`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`). A plain `http://` URL needs no certificate either, only `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` — see below.
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

To follow the download, set the callback. One function receives every event, and its return value decides whether the update goes on:

```c
#include "esp_ota_ops.h"  // for ESP_ERR_OTA_VALIDATE_FAILED

static esp_err_t on_ota_event(ota_api_event_id_t event_id, const void *data, void *ctx)
{
  switch (event_id)
  {
    case OTA_API_EVENT_PROGRESS:
    {
      const ota_api_progress_t *p = (const ota_api_progress_t *)data;
      printf("%d%% (%u/%u bytes)\n", p->percent, (unsigned)p->bytes_read, (unsigned)p->total_bytes);
      break;
    }

    case OTA_API_EVENT_IMAGE_DESC:
    {
      const esp_app_desc_t *offered = (const esp_app_desc_t *)data;
      if (strcmp(offered->version, esp_app_get_description()->version) == 0)
        return ESP_ERR_OTA_VALIDATE_FAILED;  // already running it, nothing written yet
      break;
    }

    default:
      break;
  }

  return ESP_OK;  // anything else here stops the update
}

ota_config.event_cb = on_ota_event;
ota_config.progress_interval_ms = 250;  // rate limit, 0 = every chunk
```

### Downloading over plain HTTP

For a local server or a closed network, an `http://` URL works with no certificate and no bundle — set `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` and leave `cert_pem` at `NULL`:

```kconfig
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n   # optional, saves the flash the bundle costs
```

`cert_pem` is ignored for such a URL: server verification is a TLS notion and there is no TLS to verify. Without that option the update is refused with `ESP_ERR_INVALID_ARG` before it connects.

Be clear about what you give up: the image is neither encrypted nor authenticated in transit, so anyone on the path can read it or replace it. For anything reachable from outside a network you control, use HTTPS — or keep HTTP and sign the image, with [Secure Boot](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/secure-boot-v2.html), so a swapped binary fails verification at boot.

### HTTPS to a server with a self-signed certificate

On a closed test bench the OTA server often has a self-signed certificate that no bundle will trust. The clean fix is to pass that certificate in `cert_pem` — the examples do this; `examples/common/ota_server.py` writes one with the server IP in `subjectAltName`.

When that is impractical, tell esp-tls to skip verification globally and leave `cert_pem` at `NULL`:

```kconfig
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

That is all you need — in particular **not** `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`, which stays off so a plain `http://` URL is still refused.

Those two options are not enough on their own, which is why the component has to step in. `esp_https_ota_begin()` rejects a configuration in which `cert_pem`, `use_global_ca_store` and `crt_bundle_attach` are all unset — it reads them straight off the `esp_http_client_config_t` before the HTTP client even exists, so esp-tls never gets far enough to apply its global skip, and the update fails with `No option for server verification is enabled in esp_http_client config.` With the options above the component therefore installs a `crt_bundle_attach` hook that clears that gate and then puts the verification back down, leaving the handshake with no trust anchor.

The transfer is still encrypted, but the server is not authenticated: anyone who can intercept it can serve their own image. Use it only on a network you control, and prefer a pinned `cert_pem` everywhere else.

| Scenario | `cert_pem` | Kconfig needed |
| --- | --- | --- |
| Production, public HTTPS | `NULL` | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (default) |
| Production, pinned server | your PEM | none |
| Lab, self-signed HTTPS | `NULL` | `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` |
| Lab, plain HTTP | ignored | `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` |

With `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n` the lab HTTPS case still works, but `esp_http_client` logs one misleading `use_crt_bundle configured but not enabled in menuconfig` error as it drops the hook; esp-tls then reaches the same unverified handshake on its own.

### Trial run and rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` the bootloader starts a freshly written image *on trial* and goes back to the previous firmware on the next reset unless the application says the build is good. That is the safety net that keeps a device in the field from being stranded on an update that cannot even reach the network.

If the verdict is known before `app_main` returns, decide on the spot — no timer involved:

```c
if (ota_api_is_on_trial())
{
  if (self_test_passed())
    ota_api_trial_confirm();
  else
    ota_api_trial_reject();  // reboots into the previous firmware, does not return
}
```

If something else has to answer — an operator, a server, a test that takes minutes — arm a deadline instead and let the absence of an answer be the answer:

```c
ota_api_trial_begin(0);  // 0 = CONFIG_OTA_API_TRIAL_TIMEOUT_S, default 120 s
```

It refuses with `ESP_ERR_INVALID_STATE` when the image is not on trial, so it is safe to call unconditionally at startup. Whatever answers later calls `ota_api_trial_confirm()`, which also cancels the countdown.

#### Why the self-test is yours to write

There is no self-test callback, and looking for one means two questions are being confused:

| Question | Who answers | Where |
| --- | --- | --- |
| Did the transfer work? | the component | `event_cb` at `OTA_API_EVENT_SUCCEEDED` / `OTA_API_EVENT_FAILED` |
| Does the new firmware work? | **your application** | `ota_api_trial_confirm()` / `ota_api_trial_reject()` |

The second question can only be answered *after* the reboot, by the new image, in a process where nothing that ran the update still exists — there is nothing left for the component to call back into. And what "works" means belongs to the product: a server handshake, a sensor answering, a GPIO jumper. So the application calls the component, not the other way round.

The deadline armed by `ota_api_trial_begin()` is **not** the test. It is what happens when nobody answers at all, which is what saves a device that booted an image too broken to reach its own self-test.

[examples/rollback](examples/rollback) shows a self-test that decides at boot; [examples/advanced](examples/advanced) shows an operator answering from a console.

### Following an update from elsewhere

The component posts nothing to the default event loop. If a task that did *not* start the update needs to follow it — a display, an MQTT reporter — register a handler for `ESP_HTTPS_OTA_EVENT`, which `esp_https_ota` posts on its own during any update. Those events carry no percentage and cannot stop anything, which is exactly the gap `event_cb` fills.

`ota_api_abort()` stops an update in progress; `ota_api_is_running()` reports whether one is active.

## Examples

| Example                          | Description                                                                                                                  |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| [basic](examples/basic)          | Full OTA flow with `ota_api_start_task()`: network connection, partition SHA-256 report and update from a configurable URL.   |
| [rollback](examples/rollback)    | Self-test after booting a new image, confirming it with `ota_api_trial_confirm()` or discarding it with `ota_api_trial_reject()`. |
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
