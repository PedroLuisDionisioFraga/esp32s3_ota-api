# OTA API Rollback Example

Self-test and automatic rollback with the **ota-api** component, ported from
ESP-IDF's `native_ota_example`.

A firmware update that builds, downloads and boots can still be broken — wrong
Wi-Fi credentials, a peripheral that no longer answers, a crash after ten
seconds. With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` the bootloader starts a
newly written image in the `ESP_OTA_IMG_PENDING_VERIFY` state and expects the
application to vouch for itself. If it does not, the next reset goes back to
the previous firmware.

This example also uses `ota_api_update()`, the **blocking** API, instead of the
background task used by [basic](../basic): the download returns to the caller,
which then decides when to `esp_restart()`.

## How the flow works

```text
boot ──▶ esp_ota_get_state_partition()
          │
          ├─ PENDING_VERIFY ──▶ run_diagnostic()
          │                       ├─ pass ──▶ esp_ota_mark_app_valid_cancel_rollback()
          │                       └─ fail ──▶ esp_ota_mark_app_invalid_rollback_and_reboot()
          │                                     └──▶ reboots into the previous firmware
          └─ confirmed / factory ──▶ continue
                                      │
                                      ▼
                            ota_api_update()   (blocking)
                                      │
                                      ▼
                            esp_restart()  ──▶ next boot is PENDING_VERIFY
```

The diagnostic in [main/main.c](main/main.c) is whether `example_connect()`
joined the network. Replace `run_diagnostic()` with a real health check for
your product — a sensor responding, a server handshake, a GPIO jumper as in the
Espressif original.

> A device that never confirms its image will loop: boot new image → fail
> diagnostic → roll back. That is the intended behaviour, not a bug.

## What happens on each boot

The file `main.c` is compiled into a firmware binary. The `.c` file itself is
not sent to the ESP32. The first image is normally written over USB; later
images are downloaded by OTA.

### First boot after USB flashing

The bootloader starts the image in the `factory` partition. The application
gets the partition from which it is currently running with
`esp_ota_get_running_partition()` ([main/main.c](main/main.c)). It then asks
`esp_ota_get_state_partition()` for that partition's OTA state
([main/main.c](main/main.c)).

The `factory` partition is not an OTA slot, so it normally has no OTA state.
The function returns an error, the `else` branch logs that no self-test is
required, and `pending_verify` remains `false`. This is expected and is not a
failure. Consequently, the confirmation or rollback block is skipped.

The application connects to the network with `example_connect()` and checks
the result with `ESP_ERROR_CHECK(connect_err)`
([main/main.c](main/main.c)). If the connection succeeds, it reaches
`ota_api_update()` ([main/main.c](main/main.c)). This is the moment when the
OTA starts: the new firmware is downloaded and written to a different OTA
partition, such as `ota_0`, while the current image keeps running.

After a successful download, the component selects the new image for the next
boot. The application then calls `esp_restart()`
([main/main.c](main/main.c)). If the connection fails before the OTA starts,
`ESP_ERROR_CHECK(connect_err)` stops the application; there is no rollback from
the factory image because it is not a pending OTA image.

### Second boot after OTA

After the restart, the bootloader starts the newly downloaded OTA image. With
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image starts in
`ESP_OTA_IMG_PENDING_VERIFY`. The same call to
`esp_ota_get_state_partition()` now succeeds and writes that state into
`ota_state`. The comparison
`ota_state == ESP_OTA_IMG_PENDING_VERIFY` sets `pending_verify` to `true`
([main/main.c](main/main.c)).

The application connects to the network again. This connection is the
self-test. If it succeeds, `run_diagnostic()` returns `true` and
`esp_ota_mark_app_valid_cancel_rollback()` marks the new image as valid
([main/main.c](main/main.c)). If it fails, the application calls
`esp_ota_mark_app_invalid_rollback_and_reboot()`
([main/main.c](main/main.c)); the bootloader then tries to start the previous
valid image.

The confirmation function does not start the OTA. It only confirms an image
that was already downloaded and booted. If the new firmware never calls it,
the image remains pending; after a reset, the bootloader can roll back instead
of trusting that image. A crash, watchdog reset, or power loss can therefore
prevent confirmation.

### What if there is no previous image?

Rollback does not create a backup. It only returns to an image that already
exists and is valid in another partition. For reliable rollback, keep a valid
previous image in the other OTA slot. If no suitable previous image exists,
`esp_ota_mark_app_invalid_rollback_and_reboot()` can return an error and there
may be no firmware to which the bootloader can return. USB flashing or another
recovery mechanism is then required.

Also note the difference between a connection failure and an endless
connection attempt. If `example_connect()` returns an error, the diagnostic
can reject the pending image. If it never returns, execution never reaches the
rollback call; a watchdog or external reset is needed before the bootloader
can evaluate the pending image again.

## Configuration

```bash
idf.py menuconfig
```

- **Example Connection Configuration** — Wi-Fi SSID/password or Ethernet.
- **Example Configuration -> firmware upgrade url endpoint** — URL of the
  `.bin` to download. The server helper prints the exact URL when it starts.
- **Example Configuration -> Skip server certificate CN fieldcheck** — leave
  off. The certificate generated by `ota_server.py` carries the server IP in
  `subjectAltName`, so validation succeeds without it.

`sdkconfig.defaults` already enables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
and adds `../common/certs/ca_cert.pem` to the trusted certificate bundle.

## Local HTTPS server

```bash
python ../common/ota_server.py
```

Serves `examples/common/ota/` on port `8070` and generates the self-signed
certificate itself — no `openssl` required, on Windows or Linux. See
[examples/common/README.md](../common/README.md).

Rebuild and reflash whenever the script reports a newly generated certificate,
since it is baked into the binary at build time.

## Build and Flash

```bash
idf.py -p PORT flash monitor
```

(Replace `PORT` with the serial port name, e.g., `/dev/ttyUSB0` or `COM3`.
To exit the serial monitor, type `Ctrl-]`.)

## Trying both outcomes

**Successful update** — serve an image, let the device download it and reboot.
On the next boot the log shows:

```text
I (...) ota_rollback: Partition state: PENDING_VERIFY, self-test required
I (...) ota_rollback: Diagnostic PASSED: network is up
I (...) ota_rollback: Image confirmed, rollback cancelled
```

**Rollback** — power off the access point (or change its password) right after
the device reboots into the new image. The diagnostic fails and the device
returns to the previous firmware:

```text
I (...) ota_rollback: Partition state: PENDING_VERIFY, self-test required
E (...) ota_rollback: Diagnostic FAILED: could not join the network (ESP_ERR_TIMEOUT)
E (...) ota_rollback: Rejecting this image and rebooting into the previous firmware
```

Confirm with `idf.py -p PORT monitor` that the partition reported after the
reboot is the earlier one (`ota_0` instead of `ota_1`, or the reverse).

This test requires two images: the first image is flashed over USB, and the
second image is the `.bin` served by `ota_server.py`. A practical sequence is:

1. Start the local server and note the HTTPS URL it prints:

  ```bash
  python ../common/ota_server.py
  ```

2. Set that URL in `idf.py menuconfig` under **Example Configuration ->
  firmware upgrade url endpoint**.

3. Flash the first image and open the monitor:

  ```bash
  idf.py -p COM3 build flash monitor
  ```

4. Wait for the first image to connect, download the OTA image, and reboot.
  The second boot should log `PENDING_VERIFY`, then either `Diagnostic PASSED`
  and `Image confirmed`, or the rollback messages above.

5. To force rollback, make the network unavailable immediately after the
  reboot into the new image. The new image must fail its diagnostic while the
  previous image remains available in the other OTA slot.

## Going further

This example decides automatically, from a built-in diagnostic. [advanced](../advanced)
hands the same decision to an operator instead: the device waits for a `confirm`
on the console and rolls back on its own only if nobody answers in time.

## Project Structure

```text
examples/rollback/
├── CMakeLists.txt
├── sdkconfig.defaults          # OTA partitions + BOOTLOADER_APP_ROLLBACK_ENABLE
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
