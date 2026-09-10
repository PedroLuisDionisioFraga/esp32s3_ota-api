# SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
"""Local HTTPS server shared by the ota-api examples.

Serves a firmware image over HTTPS and, when needed, generates the self-signed
certificate it is served with. Certificate generation uses the `cryptography`
package (already part of the ESP-IDF Python environment), so no external
`openssl` binary is required -- the script behaves the same on Windows and
Linux.

With --http the image is served over plain HTTP instead and no certificate is
read or generated; the device then needs CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y.

Range requests are honoured with 206 Partial Content, which is what
ota_api_config_t.partial_download needs: python -m http.server ignores Range
and answers 200 with the whole file, and esp_https_ota then takes the image in
that one response. --no-range brings that behaviour back to reproduce it, and
--keep-alive speaks HTTP/1.1 so every range can travel over one connection.
Each request is logged with its status and the Range it asked for.

Relative paths are resolved against the directory of this script, so the same
command works from any example directory:

    python ../common/ota_server.py
    python ../common/ota_server.py --http
"""
import argparse
import datetime
import functools
import http.server
import io
import ipaddress
import os
import re
import socket
import ssl
from http import HTTPStatus
from typing import List, Optional, Tuple

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    HAVE_CRYPTOGRAPHY = True
except ImportError:
    HAVE_CRYPTOGRAPHY = False

CERT_VALIDITY_DAYS = 365
CERT_NAME = 'ca_cert.pem'
KEY_NAME = 'ca_key.pem'

MISSING_CRYPTOGRAPHY_MSG = """\
error: the 'cryptography' package is required to generate the server certificate.

Use the ESP-IDF Python environment, which already ships it:

    . $IDF_PATH/export.sh           # Linux / macOS
    & $env:IDF_PATH\\export.ps1      # Windows PowerShell

or install it directly:

    python -m pip install cryptography

Alternatively, create the pair by hand and re-run with --no-gen-cert:

    openssl req -x509 -newkey rsa:2048 -keyout ca_key.pem -out ca_cert.pem \\
      -days 365 -nodes -subj "/CN=<host-ip>" -addext "subjectAltName=IP:<host-ip>"
"""


def detect_host_ip() -> str:
    probe_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe_socket.connect(('8.8.8.8', 80))
        return probe_socket.getsockname()[0]
    finally:
        probe_socket.close()


def certificate_matches(cert_file: str, host_ip: str) -> bool:
    """Return True if cert_file exists, has not expired and covers host_ip."""
    if not HAVE_CRYPTOGRAPHY or not os.path.exists(cert_file):
        return False

    try:
        with open(cert_file, 'rb') as handle:
            cert = x509.load_pem_x509_certificate(handle.read())

        # not_valid_after_utc replaces the deprecated naive not_valid_after
        expires = getattr(cert, 'not_valid_after_utc', None)
        if expires is None:
            expires = cert.not_valid_after.replace(tzinfo=datetime.timezone.utc)
        if expires <= datetime.datetime.now(datetime.timezone.utc):
            return False

        san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName)
        return ipaddress.ip_address(host_ip) in san.value.get_values_for_type(x509.IPAddress)
    except Exception:
        # Unreadable, malformed or SAN-less certificate: regenerate it
        return False


def generate_certificate(cert_file: str, key_file: str, host_ip: str) -> None:
    """Write a self-signed certificate/key pair valid for host_ip."""
    os.makedirs(os.path.dirname(cert_file) or '.', exist_ok=True)

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, host_ip)])
    now = datetime.datetime.now(datetime.timezone.utc)

    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        # Backdated by a day so a small clock skew on the device does not
        # reject a certificate that was just generated
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=CERT_VALIDITY_DAYS))
        # The IP goes in the SAN as well as the CN: mbedTLS validates the SAN,
        # so without it the device needs EXAMPLE_SKIP_COMMON_NAME_CHECK
        .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(host_ip))]),
                       critical=False)
        # Self-signed and used as a trust anchor by the device
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    with open(key_file, 'wb') as handle:
        handle.write(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.PKCS8,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
    if os.name != 'nt':
        os.chmod(key_file, 0o600)

    with open(cert_file, 'wb') as handle:
        handle.write(certificate.public_bytes(serialization.Encoding.PEM))


def ensure_certificate(cert_dir: str, host_ip: str, regenerate: bool, allow_generate: bool) -> None:
    cert_file = os.path.join(cert_dir, CERT_NAME)
    key_file = os.path.join(cert_dir, KEY_NAME)

    if regenerate:
        reason = 'requested with --regen-cert'
    elif not os.path.exists(cert_file) or not os.path.exists(key_file):
        reason = f'{CERT_NAME} or {KEY_NAME} is missing from {cert_dir}'
    elif not certificate_matches(cert_file, host_ip):
        reason = f'the existing certificate is expired or not valid for {host_ip}'
    else:
        print(f'Reusing certificate for {host_ip} from {cert_file}')
        return

    if not allow_generate:
        raise SystemExit(f'error: {reason}, and --no-gen-cert was given.')
    if not HAVE_CRYPTOGRAPHY:
        raise SystemExit(MISSING_CRYPTOGRAPHY_MSG)

    print(f'Generating certificate: {reason}')
    generate_certificate(cert_file, key_file, host_ip)

    rule = '!! ' + '-' * 70
    print('')
    print(rule)
    print(f'!! New self-signed certificate generated for {host_ip}')
    print(f'!!   {cert_file}')
    print('!!')
    print('!! The firmware embeds ca_cert.pem at build time, so a device flashed')
    print('!! earlier still trusts the previous certificate and the TLS handshake')
    print('!! will fail. Rebuild and reflash before testing:')
    print('!!     idf.py fullclean build flash monitor')
    print(rule)
    print('')


RANGE_PATTERN = re.compile(r'bytes=(\d*)-(\d*)')


class RangeNotSatisfiable(Exception):
    """A well-formed range that starts past the end of the file (416)."""


def parse_range(header: str, size: int) -> Optional[Tuple[int, int]]:
    """First and last byte a single-range header asks for, clamped to the file.

    Returns None for a header to ignore -- several ranges, another unit, or a
    malformed one -- which RFC 9110 lets a server answer with the whole file.
    """
    match = RANGE_PATTERN.fullmatch(header.strip())
    if not match or match.group(1) == match.group(2) == '':
        return None

    first, last = match.groups()
    if first == '':
        # Suffix range: the last N bytes
        if int(last) == 0 or size == 0:
            raise RangeNotSatisfiable(header)
        return max(size - int(last), 0), size - 1

    if last != '' and int(first) > int(last):
        return None
    if int(first) >= size:
        raise RangeNotSatisfiable(header)
    return int(first), size - 1 if last == '' else min(int(last), size - 1)


class OtaRequestHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler that honours a single byte range and logs it.

    The stock handler ignores Range and answers 200 with the whole file. Against
    that, esp_https_ota with partial_http_download receives the entire image in
    its first response and never sends a second request, so partial_download
    silently turns into one long transfer.
    """

    honour_range = True
    # Bounds how long a connection a device left open can hold on to its thread
    timeout = 60

    def send_head(self):
        byte_range = self.headers.get('Range') if self.honour_range else None
        path = self.translate_path(self.path)
        if byte_range is None or not os.path.isfile(path):
            return super().send_head()

        size = os.path.getsize(path)
        try:
            bounds = parse_range(byte_range, size)
        except RangeNotSatisfiable:
            self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
            self.send_header('Content-Range', f'bytes */{size}')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return None
        if bounds is None:
            return super().send_head()

        first, last = bounds
        with open(path, 'rb') as image:
            image.seek(first)
            body = image.read(last - first + 1)

        self.send_response(HTTPStatus.PARTIAL_CONTENT)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Content-Range', f'bytes {first}-{last}/{size}')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Last-Modified', self.date_time_string(int(os.path.getmtime(path))))
        self.end_headers()
        return io.BytesIO(body)

    def end_headers(self):
        if self.honour_range:
            self.send_header('Accept-Ranges', 'bytes')
        super().end_headers()

    def log_request(self, code='-', size='-'):
        if isinstance(code, HTTPStatus):
            code = code.value
        # Absent when the request line itself could not be parsed
        headers = getattr(self, 'headers', None)
        byte_range = headers.get('Range', '-') if headers is not None else '-'
        self.log_message('"%s" %s %s range=%s', self.requestline, str(code), str(size), byte_range)


def build_server(
    host_ip: str,
    image_dir: str,
    server_port: int,
    cert_dir: Optional[str] = None,
    honour_range: bool = True,
    keep_alive: bool = False,
) -> http.server.ThreadingHTTPServer:
    """Server for image_dir over HTTPS, or over plain HTTP when cert_dir is None."""
    handler_class = type(
        'Handler',
        (OtaRequestHandler,),
        {'honour_range': honour_range, 'protocol_version': 'HTTP/1.1' if keep_alive else 'HTTP/1.0'},
    )
    httpd = http.server.ThreadingHTTPServer(
        (host_ip, server_port), functools.partial(handler_class, directory=image_dir)
    )

    if cert_dir is not None:
        cert_file = os.path.join(cert_dir, CERT_NAME)
        key_file = os.path.join(cert_dir, KEY_NAME)

        ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_context.load_cert_chain(certfile=cert_file, keyfile=key_file)
        httpd.socket = ssl_context.wrap_socket(httpd.socket, server_side=True)

    return httpd


def start_server(
    host_ip: str,
    image_dir: str,
    server_port: int,
    cert_dir: Optional[str],
    honour_range: bool = True,
    keep_alive: bool = False,
) -> None:
    """Serve image_dir over HTTPS, or over plain HTTP when cert_dir is None."""
    httpd = build_server(host_ip, image_dir, server_port, cert_dir, honour_range, keep_alive)
    scheme = 'http' if cert_dir is None else 'https'

    print(f'Starting {scheme.upper()} server at {scheme}://{host_ip}:{server_port}')
    print(f'Serving files from: {os.path.abspath(image_dir)}')
    if cert_dir is None:
        print('Plain HTTP: the device must be built with CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y')
    else:
        print(f'Using certificates from: {os.path.abspath(cert_dir)}')
    if honour_range:
        print('Range requests: honoured with 206 Partial Content')
    else:
        print('Range requests: ignored (--no-range), every GET gets 200 and the whole file')
    if keep_alive:
        print('Connections: kept open between requests (HTTP/1.1)')
    else:
        print('Connections: closed after each response (HTTP/1.0)')
    for image in sorted(name for name in os.listdir(image_dir) if name.endswith('.bin')):
        print(f'  firmware upgrade url: {scheme}://{host_ip}:{server_port}/{image}')
    httpd.serve_forever()


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Start a local HTTPS (or plain HTTP) server for the ota-api examples.')
    parser.add_argument('--host', help='LAN IP to bind to, for example 192.168.1.50 (default: auto-detected)')
    # The certificate options only make sense for HTTPS, so --http excludes them
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        '--http',
        action='store_true',
        help='Serve over plain HTTP; no certificate is read or generated',
    )
    mode.add_argument('--regen-cert', action='store_true', help='Always generate a new certificate')
    mode.add_argument(
        '--no-gen-cert',
        action='store_true',
        help='Never generate a certificate; fail if a usable one is not present',
    )
    parser.add_argument(
        '--no-range',
        action='store_true',
        help='Ignore Range headers and answer every GET with 200 and the whole file, like python -m http.server',
    )
    parser.add_argument(
        '--keep-alive',
        action='store_true',
        help='Speak HTTP/1.1 and keep the connection open, so every range can use the same connection',
    )
    parser.add_argument(
        'image_dir',
        nargs='?',
        default='ota',
        help='Directory that contains the firmware .bin file to serve (default: ota)',
    )
    parser.add_argument('server_port', nargs='?', type=int, default=8070, help='Server port (default: 8070)')
    parser.add_argument(
        'cert_dir',
        nargs='?',
        default='certs',
        help=f'Directory that contains {CERT_NAME} and {KEY_NAME} (default: certs; ignored with --http)',
    )
    return parser.parse_args(argv)


def main() -> None:
    args = parse_args()
    honour_range = not args.no_range

    this_dir = os.path.dirname(os.path.realpath(__file__))
    host_ip = args.host or detect_host_ip()
    image_dir = args.image_dir if os.path.isabs(args.image_dir) else os.path.join(this_dir, args.image_dir)
    cert_dir = args.cert_dir if os.path.isabs(args.cert_dir) else os.path.join(this_dir, args.cert_dir)

    if not os.path.isdir(image_dir):
        raise SystemExit(f'error: firmware directory not found: {image_dir}')

    if args.http:
        start_server(host_ip, image_dir, args.server_port, None, honour_range, args.keep_alive)
    else:
        ensure_certificate(cert_dir, host_ip, args.regen_cert, not args.no_gen_cert)
        start_server(host_ip, image_dir, args.server_port, cert_dir, honour_range, args.keep_alive)


if __name__ == '__main__':
    main()
