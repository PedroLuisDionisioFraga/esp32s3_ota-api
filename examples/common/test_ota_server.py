# SPDX-License-Identifier: Unlicense OR CC0-1.0
"""Host tests for ota_server.py: ranged responses and the esp_https_ota download walk.

No device and no TLS involved -- the handler is the same over plain HTTP, so the
server runs on 127.0.0.1 with --http semantics:

    python -m pytest examples/common -v
"""
import http.client
import os
import random
import sys
import threading

import pytest

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import ota_server  # noqa: E402

# The size of the image in the lab report, and not a multiple of any request size
IMAGE_SIZE = 830400
IMAGE = random.Random(0).randbytes(IMAGE_SIZE)
IMAGE_NAME = 'fw.bin'


@pytest.fixture
def serve(tmp_path):
    """Start a server over tmp_path; returns a function taking the server options."""
    (tmp_path / IMAGE_NAME).write_bytes(IMAGE)
    servers = []

    def start(**options):
        server = ota_server.build_server('127.0.0.1', str(tmp_path), 0, **options)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
        return server.server_address[1]

    yield start
    for server in servers:
        server.shutdown()
        server.server_close()


def request(conn, method='GET', byte_range=None):
    conn.request(method, f'/{IMAGE_NAME}', headers={'Range': byte_range} if byte_range else {})
    response = conn.getresponse()
    return response, response.read()


def fetch(port, method='GET', byte_range=None):
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    try:
        return request(conn, method, byte_range)
    finally:
        conn.close()


def test_plain_get_serves_the_whole_image_and_advertises_ranges(serve):
    response, body = fetch(serve())

    assert response.status == 200
    assert response.getheader('Accept-Ranges') == 'bytes'
    assert body == IMAGE


def test_bounded_range_returns_206_with_exactly_that_slice(serve):
    response, body = fetch(serve(), byte_range='bytes=0-65535')

    assert response.status == 206
    assert response.getheader('Content-Range') == f'bytes 0-65535/{IMAGE_SIZE}'
    assert response.getheader('Content-Length') == '65536'
    assert body == IMAGE[:65536]


def test_open_ended_range_returns_the_rest_of_the_image(serve):
    response, body = fetch(serve(), byte_range='bytes=786432-')

    assert response.status == 206
    assert response.getheader('Content-Range') == f'bytes 786432-{IMAGE_SIZE - 1}/{IMAGE_SIZE}'
    assert body == IMAGE[786432:]


def test_suffix_range_returns_the_last_bytes(serve):
    response, body = fetch(serve(), byte_range='bytes=-100')

    assert response.status == 206
    assert response.getheader('Content-Range') == f'bytes {IMAGE_SIZE - 100}-{IMAGE_SIZE - 1}/{IMAGE_SIZE}'
    assert body == IMAGE[-100:]


def test_range_ending_past_the_image_is_clamped(serve):
    response, body = fetch(serve(), byte_range='bytes=800000-999999')

    assert response.status == 206
    assert response.getheader('Content-Range') == f'bytes 800000-{IMAGE_SIZE - 1}/{IMAGE_SIZE}'
    assert body == IMAGE[800000:]


@pytest.mark.parametrize('byte_range', [f'bytes={IMAGE_SIZE}-', 'bytes=-0'])
def test_unsatisfiable_range_returns_416(serve, byte_range):
    response, body = fetch(serve(), byte_range=byte_range)

    assert response.status == 416
    assert response.getheader('Content-Range') == f'bytes */{IMAGE_SIZE}'
    assert body == b''


@pytest.mark.parametrize('byte_range', ['bytes=0-1,5-6', 'bytes=5-1', 'bytes=abc', 'items=0-1'])
def test_range_it_does_not_serve_falls_back_to_the_whole_image(serve, byte_range):
    # RFC 9110 lets a server ignore a Range it does not want to honour
    response, body = fetch(serve(), byte_range=byte_range)

    assert response.status == 200
    assert body == IMAGE


def test_head_with_range_describes_the_slice_without_a_body(serve):
    response, body = fetch(serve(), method='HEAD', byte_range='bytes=0-65535')

    assert response.status == 206
    assert response.getheader('Content-Length') == '65536'
    assert body == b''


def test_no_range_option_answers_like_the_stock_server(serve):
    response, body = fetch(serve(honour_range=False), byte_range='bytes=0-65535')

    assert response.status == 200
    assert response.getheader('Accept-Ranges') is None
    assert body == IMAGE


def test_each_request_is_logged_with_its_status_and_range(serve, capsys):
    port = serve()
    fetch(port, byte_range='bytes=0-65535')
    fetch(port)

    log = capsys.readouterr().err
    assert f'"GET /{IMAGE_NAME} HTTP/1.1" 206 - range=bytes=0-65535' in log
    assert f'"GET /{IMAGE_NAME} HTTP/1.1" 200 - range=-' in log


def test_keep_alive_option_serves_several_requests_on_one_connection(serve):
    conn = http.client.HTTPConnection('127.0.0.1', serve(keep_alive=True), timeout=5)
    try:
        first, _ = request(conn, byte_range='bytes=0-9')
        sock = conn.sock
        second, body = request(conn, byte_range='bytes=10-19')
    finally:
        conn.close()

    assert first.version == 11 and not first.will_close
    assert conn.sock is None or sock is not None  # the second request did not need a new socket
    assert second.status == 206
    assert body == IMAGE[10:20]


def test_default_server_closes_the_connection_after_each_response(serve):
    response, _ = fetch(serve(), byte_range='bytes=0-9')

    assert response.will_close


def walk_like_esp_https_ota(port, max_request, keep_alive):
    """Download the image the way esp_https_ota does with partial_http_download.

    Same arithmetic as ESP-IDF 6.0.1 components/esp_https_ota/src/esp_https_ota.c:
    the first Range is set in esp_https_ota_begin() (L461-477) and every next one
    only once the current response body is exhausted (L871-875). Returns the
    reassembled image and the status of every GET.
    """
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    try:
        head, _ = request(conn, method='HEAD')
        image_length = int(head.getheader('Content-Length'))
        if not keep_alive:
            conn.close()

        received = b''
        statuses = []
        byte_range = f'bytes=0-{max_request - 1}' if image_length > max_request else None
        while True:
            response, body = request(conn, byte_range=byte_range)
            statuses.append(response.status)
            received += body
            if len(received) >= image_length:
                return received, statuses
            if not keep_alive:
                conn.close()
            start = len(received)
            if image_length - start > max_request:
                byte_range = f'bytes={start}-{start + max_request - 1}'
            else:
                byte_range = f'bytes={start}-'
    finally:
        conn.close()


@pytest.mark.parametrize('keep_alive', [False, True])
@pytest.mark.parametrize('max_request', [4096, 16384, 65536])
def test_esp_https_ota_walk_takes_one_206_per_slice(serve, max_request, keep_alive):
    received, statuses = walk_like_esp_https_ota(serve(keep_alive=keep_alive), max_request, keep_alive)

    assert received == IMAGE
    assert statuses == [206] * -(-IMAGE_SIZE // max_request)


def test_server_ignoring_range_ends_the_walk_in_one_200(serve):
    # The lab report: HEAD 200 followed by a single GET 200 carrying the image
    received, statuses = walk_like_esp_https_ota(serve(honour_range=False), 65536, keep_alive=False)

    assert received == IMAGE
    assert statuses == [200]


def test_command_line_selects_range_and_keep_alive():
    assert ota_server.parse_args(['--http']).no_range is False
    assert ota_server.parse_args(['--http']).keep_alive is False
    args = ota_server.parse_args(['--http', '--no-range', '--keep-alive'])
    assert args.no_range is True and args.keep_alive is True
