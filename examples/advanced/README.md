# OTA API Advanced Example

The whole update lifecycle under console control: start an update and watch it
download percent by percent, cancel it mid-flight, then decide from the console
whether the new firmware stays or the device goes back.

This is the other three examples combined, plus what only the step-by-step
`esp_https_ota` flow makes possible.

## What it shows

| Capability | How |
| --- | --- |
| Live download percentage | `ota_api_config_t::progress_cb`, throttled by `progress_interval_ms` |
| Lifecycle notifications | `OTA_API_EVENT` handlers on the default event loop |
| Refusing a redundant image | `validate_cb` compares the offered version with the running one |
| Cancelling mid-download | `ota_api_abort()` |
| Surviving a flaky link | `partial_download` — the image arrives over several ranged requests |
| Operator-approved rollback | `esp_ota_mark_app_valid_cancel_rollback()` driven by the `confirm` command |
| Unattended safety net | Automatic rollback when nobody confirms within the trial window |

Progress arrives through **both** reporting mechanisms at once, on purpose: the
percentage line comes from the direct callback, while "started / image header /
succeeded / failed" is consumed as events. A real application would normally
pick one — the callback for the short path, events when unrelated parts of the
system (a display, an MQTT reporter) need to follow an update they did not
start.

## Console commands

```text
ota [url] [force]   start an update; without a url the configured one is used
abort               cancel the update in flight
confirm             keep the firmware currently on trial
rollback            reject it and reboot into the previous firmware
version             running project, version, partition and trial state
status              whether an update is running or awaiting confirmation
help                list the commands
```

## The trial run

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the bootloader starts a newly
written image in `ESP_OTA_IMG_PENDING_VERIFY` and waits to be told the firmware
is good. **This example never confirms itself** — it hands the decision to you:

```text
  ==========================================================
  This firmware is ON TRIAL and has not been confirmed yet.
  Type 'confirm' to keep it, or 'rollback' to go back now.
  Without a confirmation it rolls back in 120 seconds.
  ==========================================================
```

Three outcomes:

- `confirm` — the image is marked valid and the trial timer is cancelled.
- `rollback` — the device reboots into the previous firmware immediately.
- Nothing — the trial window expires and the device rolls back on its own.

That last one is the point of the timeout: a device in the field with nobody
watching must recover from a bad update without help. Tune the window under
**Example Configuration -> Trial window before automatic rollback**.

## Configuration

```bash
idf.py menuconfig
```

- **Example Connection Configuration** — Wi-Fi SSID/password or Ethernet.
- **Example Configuration -> default firmware upgrade url endpoint** — URL used
  by a bare `ota` command.
- **Example Configuration -> Trial window before automatic rollback** — seconds
  to wait for `confirm` (default 120).
- **Example Configuration -> Minimum gap between progress reports** — rate limit
  for the progress line, in ms (default 250). Setting it to 0 reports every
  chunk and floods the console on a fast link.
- **Example Configuration -> Bytes per ranged HTTP request** — size of each
  ranged request (default 65536). Smaller recovers faster from a dropped
  connection, at the cost of more request overhead.

`sdkconfig.defaults` already enables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
and `CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD`, and adds
`../common/certs/ca_cert.pem` to the trusted certificate bundle.

## Local HTTPS server

```bash
python ../common/ota_server.py
```

Serves `examples/common/ota/` on port `8070`, prints the exact URL to paste
after `ota`, and generates the self-signed certificate itself — no `openssl`
required, on Windows or Linux. See
[examples/common/README.md](../common/README.md).

Rebuild and reflash whenever the script reports a newly generated certificate,
since it is baked into the binary at build time.

## Build and Flash

```bash
idf.py -p PORT flash monitor
```

(Replace `PORT` with the serial port name, e.g., `/dev/ttyUSB0` or `COM3`.
To exit the serial monitor, type `Ctrl-]`.)

## A full session

```text
ota> version
project     : ota_api_advanced_example
version     : 1
partition   : ota_0 at offset 0x00010000
state       : confirmed

ota> ota https://192.168.0.3:8070/hello_world.bin
Starting update from https://192.168.0.3:8070/hello_world.bin
I (12345) ota_advanced: event: download started

  offered : hello_world version 2
  running : ota_api_advanced_example version 1
I (12420) ota_advanced: event: image header read, version '2' built Sep  3 2026 18:20:11
  [##########################....]  87%  742/852 KB  61.3 KB/s  ETA 1s
I (25980) ota_advanced: event: image written successfully
  update stored. Rebooting into it for its trial run.
```

After the reboot:

```text
  ==========================================================
  This firmware is ON TRIAL and has not been confirmed yet.
  ...
ota> confirm
Image confirmed, rollback cancelled
```

### Refusing the same version

Running `ota` again with the image already installed stops before a single byte
is written to flash:

```text
  offered : hello_world version 2
  running : hello_world version 2
  same version already running, refusing. Use 'ota <url> force' to install anyway.
  update did not complete: ESP_ERR_OTA_VALIDATE_FAILED
```

### Cancelling a download

```text
ota> ota
  [########......................]  27%  230/852 KB  58.9 KB/s  ETA 10s
ota> abort
Stop requested, unwinding...
E (31002) ota_advanced: event: update failed (ESP_ERR_NOT_FINISHED)
  update did not complete: ESP_ERR_NOT_FINISHED
```

The abort is cooperative: the component notices the request between downloaded
chunks, unwinds and discards what it had written. The running firmware is never
touched, and the console stays available for another attempt.

## Project Structure

```text
examples/advanced/
├── CMakeLists.txt
├── sdkconfig.defaults          # OTA partitions, rollback, partial download
├── sdkconfig.defaults.esp32h2
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml       # ota-api + protocol_examples_common deps
    ├── Kconfig.projbuild       # Example Configuration menu
    └── main.c
```

The HTTPS server, certificates and firmware images live in
[examples/common/](../common), shared with the other examples.
