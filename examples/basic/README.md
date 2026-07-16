# OTA API Basic Example

Full OTA flow using the **ota-api** component: connects to Wi-Fi or Ethernet, prints the SHA-256 of the bootloader and running firmware, then downloads the image from the configured URL and reboots into it.

The example also supports binding the OTA connection to a specific interface (Ethernet or Wi-Fi Station) when multiple networking interfaces are enabled: `idf.py menuconfig -> Example Configuration -> Support firmware upgrade bind specified interface`.

## Configuration

```bash
idf.py menuconfig
```

- **Example Connection Configuration** — Wi-Fi SSID/password or Ethernet.
- **Example Configuration -> firmware upgrade url endpoint** — URL of the `.bin` to download (e.g. `https://192.168.0.3:8070/firmware.bin`).
- **Example Configuration -> Enable certificate bundle** — enabled by default; works with any public HTTPS server. Disable it to validate against the embedded `certs/ca_cert.pem` instead (required for a self-signed local server).

## Local HTTPS server (for testing)

Generate a self-signed certificate (on Windows, run via WSL) inside `certs/`.

```bash
cd certs
openssl req -x509 -newkey rsa:2048 -keyout ca_key.pem -out ca_cert.pem \
  -days 365 -nodes -subj "/CN=<your-host-ip>"
```

The firmware `.bin` stays in `ota/`. To serve the image and use the local
certificate files together, run the server helper:

```bash
python ota_server.py ota 8070 certs
```

The script detects your LAN IP automatically and binds the server to that
address. If the auto-detected IP is not the one the ESP32 should use, override
it explicitly:

```bash
python ota_server.py --host 192.168.xxx.xxx ota 8070 certs
```

Replace `192.168.xxx.xxx` with the IP address of the machine running the
server. The ESP32 must reach that IP over the network.

`main/CMakeLists.txt` embeds `certs/ca_cert.pem`, so rebuild after regenerating
the certificate and disable the certificate bundle option so the embedded
certificate is used.

## Build and Flash

```bash
idf.py -p PORT flash monitor
```

(Replace `PORT` with the serial port name, e.g., `/dev/ttyUSB0` or `COM3`.
To exit the serial monitor, type `Ctrl-]`.)

## Local HTTPS Server

Use `ota_server.py` to serve the firmware over HTTPS from the `ota/` folder.

By default, the server auto-detects the LAN IP:

```bash
python ota_server.py ota 8070 certs
```

This serves the files inside `ota/` over HTTPS on port `8070` and uses the
certificate files inside `certs/`.

If needed, you can still bind to a specific IP with `--host`:

```bash
python ota_server.py --host 192.168.xxx.xxx ota 8070 certs
```

In practice:

- `--host` is optional and overrides the auto-detected LAN IP.
- `ota` contains the firmware image to serve.
- `certs` contains `ca_cert.pem` and `ca_key.pem`.
- The server expects a `.bin` file such as `simple_ota.bin` or `hello_world.bin`.

## Project Structure

```text
examples/basic/
├── CMakeLists.txt
├── sdkconfig.defaults          # OTA partition table + example defaults
├── sdkconfig.defaults.esp32h2
├── certs/
│   ├── ca_cert.pem
│   └── ca_key.pem
├── ota/
│   ├── bootloader.bin
│   └── hello_world.bin
├── ota_server.py               # local HTTPS server helper
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml       # ota-api + protocol_examples_common deps
    ├── Kconfig.projbuild       # Example Configuration menu
    └── main.c
```
