# OTA API Basic Example

Full OTA flow using the **ota-api** component: connects to Wi-Fi or Ethernet, prints the SHA-256 of the bootloader and running firmware, then downloads the image from the configured URL and reboots into it.

This example uses `ota_api_start_task()`, the background-task mode: the component downloads the image and restarts the device on its own. See [rollback](../rollback) and [on_demand](../on_demand) for the blocking `ota_api_update()` mode, where the application decides when to reboot, and [advanced](../advanced) for the full lifecycle with live progress, abort and operator-confirmed rollback.

The example also supports binding the OTA connection to a specific interface (Ethernet or Wi-Fi Station) when multiple networking interfaces are enabled: `idf.py menuconfig -> Example Configuration -> Support firmware upgrade bind specified interface`.

## Configuration

```bash
idf.py menuconfig
```

- **Example Connection Configuration** — Wi-Fi SSID/password or Ethernet.
- **Example Configuration -> firmware upgrade url endpoint** — URL of the `.bin` to download (e.g. `https://192.168.0.3:8070/hello_world.bin`). The server helper prints the exact URL when it starts.
- **Example Configuration -> Enable certificate bundle** — enabled by default; works with any public HTTPS server. Disable it to validate against `../common/certs/ca_cert.pem`, which `main/CMakeLists.txt` embeds into the binary — that is what you want for the local server below.

## Local HTTPS server (for testing)

```bash
python ../common/ota_server.py
```

The script serves `examples/common/ota/` on port `8070`, auto-detects your LAN
IP and **generates the self-signed certificate itself** — no `openssl` needed,
on Windows or Linux. See [examples/common/README.md](../common/README.md) for
the options.

Because the certificate is embedded at build time, rebuild and reflash whenever
the script reports that it generated a new one:

```bash
idf.py fullclean build flash monitor
```

## Build and Flash

```bash
idf.py -p PORT flash monitor
```

(Replace `PORT` with the serial port name, e.g., `/dev/ttyUSB0` or `COM3`.
To exit the serial monitor, type `Ctrl-]`.)

## Project Structure

```text
examples/basic/
├── CMakeLists.txt
├── sdkconfig.defaults          # OTA partition table + example defaults
├── sdkconfig.defaults.esp32h2
├── README.md
└── main/
    ├── CMakeLists.txt          # embeds ../common/certs/ca_cert.pem
    ├── idf_component.yml       # ota-api + protocol_examples_common deps
    ├── Kconfig.projbuild       # Example Configuration menu
    └── main.c
```

The HTTPS server, certificates and firmware images live in
[examples/common/](../common), shared with the other examples.
