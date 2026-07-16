# SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
import argparse
import http.server
import os
import socket
import ssl


def detect_host_ip() -> str:
    probe_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe_socket.connect(('8.8.8.8', 80))
        return probe_socket.getsockname()[0]
    finally:
        probe_socket.close()


def start_https_server(host_ip: str, image_dir: str, server_port: int, cert_dir: str) -> None:
    os.chdir(image_dir)

    cert_file = os.path.join(cert_dir, 'ca_cert.pem')
    key_file = os.path.join(cert_dir, 'ca_key.pem')

    httpd = http.server.HTTPServer((host_ip, server_port), http.server.SimpleHTTPRequestHandler)

    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_context.load_cert_chain(certfile=cert_file, keyfile=key_file)

    httpd.socket = ssl_context.wrap_socket(httpd.socket, server_side=True)
    print(f'Starting HTTPS server at https://{host_ip}:{server_port}')
    print(f'Serving files from: {os.path.abspath(image_dir)}')
    print(f'Using certificates from: {os.path.abspath(cert_dir)}')
    httpd.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description='Start a local HTTPS server for the basic OTA example.')
    parser.add_argument('--host', help='LAN IP to bind to, for example 192.168.1.50')
    parser.add_argument('image_dir', help='Directory that contains the firmware .bin file to serve')
    parser.add_argument('server_port', type=int, help='HTTPS server port, for example 8070')
    parser.add_argument(
        'cert_dir',
        nargs='?',
        default='certs',
        help='Directory that contains ca_cert.pem and ca_key.pem (default: certs)',
    )
    args = parser.parse_args()

    this_dir = os.path.dirname(os.path.realpath(__file__))
    host_ip = args.host or detect_host_ip()
    image_dir = args.image_dir if os.path.isabs(args.image_dir) else os.path.join(this_dir, args.image_dir)
    cert_dir = args.cert_dir if os.path.isabs(args.cert_dir) else os.path.join(this_dir, args.cert_dir)

    start_https_server(host_ip, image_dir, args.server_port, cert_dir)


if __name__ == '__main__':
    main()