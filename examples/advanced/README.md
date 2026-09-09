# OTA API Advanced Example

The whole update lifecycle under console control: start an update and watch it
download percent by percent, cancel it mid-flight, then decide from the console
whether the new firmware stays or the device goes back.

This is the other three examples combined, plus what only the step-by-step
`esp_https_ota` flow makes possible.

## What it shows

| Capability | How |
| --- | --- |
| Live download percentage | `ota_api_config_t::event_cb` at `OTA_API_EVENT_PROGRESS`, throttled by `progress_interval_ms` |
| Refusing a redundant image | the same `event_cb` returns `ESP_ERR_OTA_VALIDATE_FAILED` at `OTA_API_EVENT_IMAGE_DESC` |
| Acting between the write and the reboot | the same `event_cb` at `OTA_API_EVENT_SUCCEEDED` |
| Cancelling mid-download | `ota_api_abort()` |
| Surviving a flaky link | `partial_download` — the image arrives over several ranged requests |
| Operator-approved rollback | `ota_api_trial_confirm()` driven by the `confirm` command |
| Unattended safety net | `ota_api_trial_begin()` — automatic rollback when nobody confirms in time |

Everything the application does about an update is in **one function**,
`on_ota_event_cb()`, and everything the operator does is in the console
commands. That is the whole file — there is no OTA task, no semaphore and no
trial state machine, because `ota_api_start_task()` owns the first and
`ota_api_trial_begin()` the last.

The callback earns its keep at two moments a plain notification could not
handle. At `OTA_API_EVENT_IMAGE_DESC` its **return value** stops the update, so
a redundant version is refused before a byte reaches flash. At
`OTA_API_EVENT_SUCCEEDED` the transfer is over but `ota_api_start_task()` has
not restarted the device yet, so that is where the network is shut down and the
operator is told what happens next — the reason this example no longer needs an
update task of its own.

If a part of the system that did *not* start the update needs to follow one,
register a handler for `ESP_HTTPS_OTA_EVENT` instead. `esp_https_ota` posts that
base by itself during any update; this component adds nothing to the event loop.

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
written image on trial and waits to be told the firmware is good. `app_main`
calls `ota_api_trial_begin(0)` to arm the deadline, and **this example never
confirms itself** — it hands the decision to you:

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
watching must recover from a bad update without help. The window belongs to the
component, not to this example: tune it under **OTA API Configuration -> Trial
window before automatic rollback**.

## Configuration

```bash
idf.py menuconfig
```

- **Example Connection Configuration** — Wi-Fi SSID/password or Ethernet.
- **Example Configuration -> default firmware upgrade url endpoint** — URL used
  by a bare `ota` command.
- **Example Configuration -> Minimum gap between progress reports** — rate limit
  for the progress line, in ms (default 250). Setting it to 0 reports every
  chunk and floods the console on a fast link.
- **Example Configuration -> Bytes per ranged HTTP request** — size of each
  ranged request (default 65536). Smaller recovers faster from a dropped
  connection, at the cost of more request overhead.
- **OTA API Configuration -> Trial window before automatic rollback** — seconds
  to wait for `confirm` (default 120). This one is the component's, so any
  project using it gets the same safety net.

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
I (12345) ota-api: Downloading update from https://192.168.0.3:8070/hello_world.bin
I (12410) ota-api: New image: project 'hello_world' version '2'

  offered : hello_world version 2 (built Sep  3 2026 18:20:11)
  running : ota_api_advanced_example version 1
I (12460) ota-api: Image size: 872448 bytes
  [##########################....]  87%  742/852 KB  61.3 KB/s  ETA 1s
I (25980) ota-api: Update written, new firmware boots on next restart

  update stored. Rebooting into it for its trial run.
I (26990) ota-api: OTA succeeded, rebooting...
```

The `ota-api` lines come from the component, the indented ones from this
example's `event_cb`. Log timestamps and the exact percentage of course differ
from run to run.

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
  offered : hello_world version 2 (built Sep  3 2026 18:20:11)
  running : hello_world version 2
  same version already running, refusing. Use 'ota <url> force' to install anyway.
W (13102) ota-api: event_cb returned ESP_ERR_OTA_VALIDATE_FAILED for event 1
E (13108) ota-api: Update failed (ESP_ERR_OTA_VALIDATE_FAILED)

  update did not complete: ESP_ERR_OTA_VALIDATE_FAILED
E (13120) ota-api: OTA task finished with error
```

### Cancelling a download

```text
ota> ota
  [########......................]  27%  230/852 KB  58.9 KB/s  ETA 10s
ota> abort
Stop requested, unwinding...
W (31002) ota-api: Update stopped on request
E (31008) ota-api: Update failed (ESP_ERR_NOT_FINISHED)

  update did not complete: ESP_ERR_NOT_FINISHED
E (31020) ota-api: OTA task finished with error
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
