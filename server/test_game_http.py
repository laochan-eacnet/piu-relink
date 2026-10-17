"""LAN address reporting, real static assets and bounded diagnostic streaming."""
from contextlib import closing
import io
import json
from pathlib import Path
import sqlite3
import tempfile
from types import SimpleNamespace
import unittest

import piuserver as protocol


class CountingSink:
    def __init__(self):
        self.count = 0
        self.largest = 0
        self.prefix = b""

    def write(self, value):
        self.count += len(value)
        self.largest = max(self.largest, len(value))
        if not self.prefix:
            self.prefix = bytes(value[:256])
        return len(value)


class HTTPHarness(protocol.HTTP):
    def __init__(self, server, path):
        self.server = server
        self.client_address = ("10.23.4.5", 12345)
        self.command = "GET"
        self.path = path
        self.headers = {}
        self.rfile = io.BytesIO()
        self.wfile = CountingSink()
        self.response_headers = {}
        self.status = None

    def send_response(self, status):
        self.status = status

    def send_header(self, name, value):
        self.response_headers[name] = value

    def end_headers(self):
        pass


class GameHTTPTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.db = str(self.root / "piu.db")
        with closing(sqlite3.connect(self.db)) as connection:
            connection.executescript(protocol.SCHEMA)
        self.server = SimpleNamespace(db=self.db, country=9, assets=str(self.root / "assets"))

    def request(self, path):
        handler = HTTPHarness(self.server, path)
        handler.respond()
        return handler

    def test_country_and_connectivity_report_actual_lan_peer(self):
        country = self.request("/piu.module/piu.get_ip_to_country.php")
        self.assertEqual(country.status, 200)
        self.assertEqual(json.loads(country.wfile.prefix),
                         {"country_data": {"country_no": "9", "ip": "10.23.4.5"}})
        ip = self.request("/ip/")
        self.assertEqual(ip.status, 200)
        self.assertEqual(ip.wfile.prefix, b"10.23.4.5")

    def test_cdn_assets_are_real_local_files_and_cannot_escape_root(self):
        asset = Path(self.server.assets) / "2023" / "ad" / "banner.bin"
        asset.parent.mkdir(parents=True)
        asset.write_bytes(b"real local advertisement bytes")
        response = self.request("/2023/ad/banner.bin")
        self.assertEqual(response.status, 200)
        self.assertEqual(response.wfile.prefix, asset.read_bytes())
        self.assertEqual(response.wfile.count, len(asset.read_bytes()))
        self.assertEqual(self.request("/2023/update/missing.bin").status, 404)
        self.assertEqual(self.request("/assets/%2e%2e/piu.db").status, 404)

    def test_diagnostic_streams_full_advertised_length_in_bounded_chunks(self):
        response = self.request("/dummy/test500mb.db")
        self.assertEqual(response.status, 200)
        self.assertEqual(int(response.response_headers["Content-Length"]), 500 * 1024 * 1024)
        self.assertEqual(response.wfile.count, 500 * 1024 * 1024)
        self.assertLessEqual(response.wfile.largest, 1024 * 1024)
        self.assertEqual(response.wfile.prefix, bytes(256))


if __name__ == "__main__":
    unittest.main()
