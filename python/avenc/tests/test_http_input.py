"""Run with: python -m unittest discover -s python/avenc/tests -p test_http_input.py.

Set AVENC_HTTP_INTEGRATION=1 and AVENC_HTTP_TEST_URL for the public network test.
"""
from __future__ import annotations
import contextlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
BIN_DIR = Path(os.environ.get("AVENC_BIN_DIR", ROOT / "bin"))
SOURCE = ROOT / "python/avenc/avenc_xarray/source.py"
spec = importlib.util.spec_from_file_location("avenc_http_source_test", SOURCE)
source_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source_module)
HTTPRangeSource = source_module.HTTPRangeSource
LocalFileSource = source_module.LocalFileSource
PUBLIC_URL = os.environ.get("AVENC_HTTP_TEST_URL", "")


@contextlib.contextmanager
def serving(payload: bytes):
    requests = []
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"
        def log_message(self, *args): pass
        def do_HEAD(self): self.respond(False)
        def do_GET(self): self.respond(True)
        def respond(self, body):
            requests.append((self.command, self.path, self.headers.get("Range")))
            if self.path == "/redirect":
                self.send_response(302); self.send_header("Location", "/range"); self.end_headers(); return
            if self.path == "/missing": self.send_error(404); return
            if self.path == "/no-head" and not body: self.send_error(405); return
            if self.path == "/timeout" and body: time.sleep(.1)
            start, end = 0, len(payload) - 1
            ranged = body and self.path != "/no-range" and self.headers.get("Range")
            if ranged:
                import re
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", ranged)
                start = int(match[1]); end = min(end, int(match[2]) if match[2] else end)
            self.send_response(206 if ranged else 200)
            self.send_header("Content-Length", str(end - start + 1))
            if ranged:
                value = f"bytes {start}-{end}/{len(payload)}"
                if self.path == "/malformed": value = "broken"
                self.send_header("Content-Range", value)
            self.end_headers()
            if body:
                chunk = payload[start:end + 1]
                if self.path == "/truncated" or (self.path == "/later-truncated" and end > start): chunk = chunk[:len(chunk)//2]
                try: self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError): pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try: yield f"http://127.0.0.1:{server.server_port}", requests
    finally: server.shutdown(); server.server_close(); thread.join()


class SourceTests(unittest.TestCase):
    data = bytes(range(256)) * 4096
    def test_local_random_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input"; path.write_bytes(self.data)
            with LocalFileSource(path) as source:
                for offset in (1000, 0, 700000, len(self.data) + 1):
                    self.assertEqual(source.read(offset, 1024), self.data[offset:offset+1024])
                self.assertEqual(source.size(), len(self.data))

    def test_ranges_cache_redirect_and_no_head(self):
        for route in ("/range", "/redirect", "/no-head"):
            with self.subTest(route=route), serving(self.data) as (base, requests):
                with HTTPRangeSource(base + route, block_size=4096, cache_blocks=2) as source:
                    for offset in (0, 10, 4090, 100000, 3):
                        self.assertEqual(source.read(offset, 1024), self.data[offset:offset+1024])
                    self.assertGreater(source.stats["cache_hits"], 0)
                    self.assertLessEqual(len(source._cache), 2)
                    self.assertEqual(source.stats["http_requests"], len(requests))
                    self.assertLess(source.stats["bytes_transferred"], len(self.data))

    def test_fallback_and_limit(self):
        with serving(self.data) as (base, _):
            with HTTPRangeSource(base + "/no-range") as source:
                self.assertEqual(source.stats["mode"], "full-download fallback")
                temporary = source._temporary
                self.assertEqual(source.read(90000, 120), self.data[90000:90120])
                self.assertEqual(source.stats["bytes_transferred"], len(self.data))
            self.assertFalse(Path(temporary).exists())
            with self.assertRaisesRegex(OSError, "disabled"):
                HTTPRangeSource(base + "/no-range", fallback=False)
            with self.assertRaisesRegex(OSError, "exceeds"):
                HTTPRangeSource(base + "/no-range", fallback_limit=100)

    def test_interrupted_read_after_probe(self):
        with serving(self.data) as (base, _):
            with HTTPRangeSource(base + "/later-truncated", block_size=4096) as source:
                with self.assertRaisesRegex(OSError, "Truncated"):
                    source.read(10000, 1024)
                self.assertFalse(source._cache, "failed blocks must not enter the cache")

    def test_errors(self):
        with serving(self.data) as (base, _):
            for route, text in (("/missing", "404"), ("/truncated", "Truncated"),
                                ("/malformed", "Content-Range"), ("/timeout", "timed out")):
                with self.subTest(route=route), self.assertRaisesRegex(Exception, text):
                    HTTPRangeSource(base + route, timeout=.02)


class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = BIN_DIR / "avenc2grib"
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        for name in ("avenc2grib", "avenc_test_generator", "grib2avenc"):
            if not (BIN_DIR / name).exists(): raise unittest.SkipTest("Build AVENC binaries first")
        subprocess.run([str(BIN_DIR / "avenc_test_generator"), "--ni", "64", "--nj", "64", "--nt", "2", "--nlev", "1", "--out-dir", str(cls.directory)], check=True, capture_output=True)
        cls.mp4 = cls.directory / "tiny.mp4"
        # Test HTTP transport independently of AV1 tile-size constraints.
        subprocess.run([str(BIN_DIR / "grib2avenc"), "--fast", "--threads", "2", str(cls.directory / "synthetic_all.grib"), str(cls.mp4)], check=True, capture_output=True)
        # A valid trailing free box lets opening prove it did not fetch the full object.
        import struct
        with cls.mp4.open("ab") as output:
            output.write(struct.pack(">I4s", 4*1024**2, b"free")); output.write(bytes(4*1024**2 - 8))
        cls.payload = cls.mp4.read_bytes()

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_cli(self, *args):
        return subprocess.run([str(self.binary), *map(str, args)], capture_output=True)

    def test_local_remote_grib_identical(self):
        local = self.directory / "local.grib"
        result = self.run_cli(self.mp4, local); self.assertEqual(result.returncode, 0, result.stderr)
        for route in ("/range", "/redirect", "/no-range"):
            with serving(self.payload) as (base, _):
                remote = self.directory / "remote.grib"
                result = self.run_cli("--http-stats", base + route, remote)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(remote.read_bytes(), local.read_bytes())
                stats = json.loads(result.stderr.decode().split("HTTP_STATS ")[1].splitlines()[0])
                self.assertGreater(stats["bytes_transferred"], 0)

    def test_subset_and_no_fallback(self):
        selectors = ["--stream-id", "0", "--frame-start", "1", "--frame-count", "1", "--width", "8", "--height", "8"]
        local = self.run_cli(*selectors, self.mp4); self.assertEqual(local.returncode, 0, local.stderr)
        with serving(self.payload) as (base, _):
            remote = self.run_cli(*selectors, base + "/range")
            self.assertEqual(remote.returncode, 0, remote.stderr)
            self.assertEqual(remote.stdout, local.stdout)
            error = self.run_cli("--no-http-fallback", "--list", base + "/no-range")
            self.assertNotEqual(error.returncode, 0)
            self.assertIn(b"disabled", error.stderr)
            error = self.run_cli("--list", base + "/later-truncated")
            self.assertNotEqual(error.returncode, 0)
            self.assertIn(b"Truncated", error.stderr)

    def test_xarray_lazy_equivalence(self):
        try:
            from avenc import open_avenc
            import numpy as np
            import xarray as xr
        except ImportError as exc:
            self.skipTest(f"Python AVENC environment required: {exc}")
        with serving(self.payload) as (base, _):
            with open_avenc(self.mp4) as local, xr.open_dataset(base + "/range", engine="avenc") as remote:
                adapter = remote.encoding["avenc_source"]
                self.assertLess(adapter.stats["bytes_transferred"], len(self.payload))
                self.assertEqual(set(remote.data_vars), set(local.data_vars))
                for name in local.data_vars:
                    np.testing.assert_array_equal(remote[name].isel({remote[name].dims[0]: 1, "lat": slice(0, 8), "lon": slice(0, 8)}).values,
                                                  local[name].isel({local[name].dims[0]: 1, "lat": slice(0, 8), "lon": slice(0, 8)}).values)
                self.assertGreater(remote.avenc.http_stats["cache_hits"], 0)
            self.assertTrue(adapter.source._closed)

    @unittest.skipUnless(os.environ.get("AVENC_HTTP_INTEGRATION") == "1" and bool(PUBLIC_URL), "set AVENC_HTTP_INTEGRATION=1 and AVENC_HTTP_TEST_URL for public network checks")
    def test_public_url(self):
        result = self.run_cli("--http-stats", "--list", PUBLIC_URL)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"HTTP_STATS", result.stderr)


if __name__ == "__main__": unittest.main()
