"""Seekable local/HTTP inputs and an ephemeral FFmpeg byte-range adapter.

HTTP counters count upstream response-body bytes, including probes and repeated
transfers, excluding HTTP/TLS headers. The loopback adapter is never counted as
upstream traffic. No remote service or third-party HTTP dependency is required.
"""
from __future__ import annotations

import argparse
from collections import OrderedDict
import http.server
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import threading
import time
from typing import BinaryIO
import urllib.error
import urllib.request
from urllib.parse import urlsplit


def is_http_url(value: object) -> bool:
    """Return whether a string/path names an HTTP(S) object."""
    return urlsplit(str(value)).scheme.lower() in {"http", "https"}


class SeekableSource:
    """Read byte intervals without changing a shared file position."""
    def read(self, offset: int, length: int) -> bytes:
        raise NotImplementedError

    def size(self) -> int:
        raise NotImplementedError

    def close(self) -> None:
        """Release input resources."""

    def __enter__(self) -> SeekableSource:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


class LocalFileSource(SeekableSource):
    """Seekable filesystem input, serialized for concurrent FFmpeg reads."""
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._file: BinaryIO = self.path.open("rb")
        self._size = os.fstat(self._file.fileno()).st_size
        self._lock = threading.RLock()

    def size(self) -> int:
        return self._size

    def read(self, offset: int, length: int) -> bytes:
        if offset < 0 or length < 0:
            raise ValueError("offset and length must be nonnegative")
        with self._lock:
            self._file.seek(offset)
            return self._file.read(length)

    def close(self) -> None:
        self._file.close()


class HTTPRangeSource(SeekableSource):
    """HTTP(S) input with validated ranges, an LRU cache and bounded fallback.

    ``block_size`` coalesces nearby reads (default 1 MiB); ``cache_blocks`` bounds
    resident cached blocks. Servers ignoring Range may be downloaded to disk up
    to ``fallback_limit`` (default 1 GiB). Broken 206 responses always fail.
    """
    def __init__(self, url: str, *, block_size: int = 1024**2,
                 cache_blocks: int = 8, timeout: float = 30,
                 fallback: bool = True, fallback_limit: int = 1024**3) -> None:
        if not is_http_url(url):
            raise ValueError("Expected an HTTP(S) URL")
        if block_size <= 0 or cache_blocks <= 0 or timeout <= 0 or fallback_limit <= 0:
            raise ValueError("HTTP sizes, cache count and timeout must be positive")
        self.url = url
        self.block_size = block_size
        self.cache_blocks = cache_blocks
        self.timeout = timeout
        self._size: int | None = None
        self._cache: OrderedDict[int, bytes] = OrderedDict()
        self._lock = threading.RLock()
        self._fallback: LocalFileSource | None = None
        self._temporary: str | None = None
        self._closed = False
        self._validator: tuple[str, str] | None = None
        self._started = time.monotonic()
        self._finished: float | None = None
        self._stats = {"http_requests": 0, "bytes_transferred": 0, "cache_hits": 0,
                       "mode": "byte-range", "errors": []}
        owner = self

        class Redirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):
                if not is_http_url(newurl):
                    raise OSError("HTTP redirect to an unsupported protocol")
                owner._stats["http_requests"] += 1
                owner._stats["bytes_transferred"] += len(fp.read())
                return super().redirect_request(req, fp, code, msg, headers, newurl)

        self._opener = urllib.request.build_opener(Redirect())
        try:
            try:
                with self._request(method="HEAD") as response:
                    self._set_size(response.headers.get("Content-Length"))
                    self._set_validator(response.headers)
            except urllib.error.HTTPError as exc:
                if exc.code not in (405, 501):
                    raise
            # Accept-Ranges is advisory: validate an actual request.
            with self._request(start=0, end=0) as response:
                if response.status == 206:
                    self._read_range(response, 0, 0)
                elif response.status == 200:
                    if not fallback:
                        raise OSError("Server ignores HTTP Range; full-download fallback disabled")
                    self._download(response, fallback_limit)
                else:
                    raise OSError(f"Unexpected HTTP status {response.status} for Range probe")
        except Exception:
            self.close()
            raise

    def _request(self, *, method: str = "GET", start: int | None = None,
                 end: int | None = None):
        headers = {"Accept-Encoding": "identity"}
        if start is not None:
            headers["Range"] = f"bytes={start}-{end}"
            if self._validator:
                headers["If-Range"] = self._validator[1]
        self._stats["http_requests"] += 1
        response = self._opener.open(urllib.request.Request(self.url, headers=headers, method=method),
                                     timeout=self.timeout)
        self.url = response.geturl()  # Avoid repeating redirects for every cached block.
        return response

    def _set_size(self, text: str | None) -> None:
        if text is None:
            return
        if not text.isdigit():
            raise OSError(f"Malformed HTTP object size: {text!r}")
        size = int(text)
        if self._size is not None and size != self._size:
            raise OSError("HTTP object size changed during access")
        self._size = size

    def _set_validator(self, headers) -> None:
        for name in ("ETag", "Last-Modified"):
            value = headers.get(name)
            if value and not (name == "ETag" and value.startswith("W/")):
                if self._validator and self._validator != (name, value):
                    raise OSError("HTTP object changed during access")
                self._validator = (name, value)
                break

    def _read_range(self, response, start: int, end: int) -> bytes:
        match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", response.headers.get("Content-Range", ""))
        if not match or (int(match[1]), int(match[2])) != (start, end):
            raise OSError("Malformed/mismatched HTTP Content-Range")
        self._set_size(match[3])
        if end >= self.size():
            raise OSError("HTTP range exceeds object size")
        self._set_validator(response.headers)
        if response.headers.get("Content-Encoding", "identity") != "identity":
            raise OSError("Encoded HTTP range response is unsupported")
        expected = end - start + 1
        length = response.headers.get("Content-Length")
        if length is not None and (not length.isdigit() or int(length) != expected):
            raise OSError("Malformed/mismatched HTTP Content-Length for range")
        data = bytearray()
        try:
            while len(data) <= expected:
                part = response.read(min(65536, expected + 1 - len(data)))
                if not part:
                    break
                self._stats["bytes_transferred"] += len(part)
                data.extend(part)
        except Exception as exc:
            raise OSError(f"Interrupted HTTP range response: {exc}") from exc
        if len(data) != expected:
            raise OSError(f"Truncated/oversized HTTP range: expected {expected} bytes, received {len(data)}")
        return bytes(data)

    def _download(self, response, limit: int) -> None:
        self._set_size(response.headers.get("Content-Length"))
        if self._size is not None and self._size > limit:
            raise OSError(f"HTTP fallback exceeds {limit} bytes; use a Range-capable server")
        if response.headers.get("Content-Encoding", "identity") != "identity":
            raise OSError("Encoded full-download response is unsupported")
        fd, self._temporary = tempfile.mkstemp(prefix="avenc-http-", suffix=".mp4")
        count = 0
        with os.fdopen(fd, "wb") as output:
            while True:
                part = response.read(min(1024**2, limit - count + 1))
                if not part:
                    break
                count += len(part)
                self._stats["bytes_transferred"] += len(part)
                if count > limit:
                    raise OSError(f"HTTP fallback exceeds {limit} bytes")
                output.write(part)
        if self._size is not None and count != self._size:
            raise OSError(f"Truncated HTTP full download: expected {self._size}, received {count}")
        self._size = count
        self._fallback = LocalFileSource(self._temporary)
        self._stats["mode"] = "full-download fallback"

    def size(self) -> int:
        if self._size is None:
            raise OSError("Remote object size could not be determined")
        return self._size

    def read(self, offset: int, length: int) -> bytes:
        if offset < 0 or length < 0:
            raise ValueError("offset and length must be nonnegative")
        with self._lock:
            if self._closed:
                raise ValueError("Source is closed")
            if self._fallback:
                return self._fallback.read(offset, length)
            end = min(self.size(), offset + length)
            parts = []
            while offset < end:
                block = offset // self.block_size
                if block in self._cache:
                    self._stats["cache_hits"] += 1
                    data = self._cache.pop(block)
                else:
                    start = block * self.block_size
                    last = min(self.size(), start + self.block_size) - 1
                    with self._request(start=start, end=last) as response:
                        if response.status != 206:
                            raise OSError("HTTP Range support disappeared or object changed")
                        data = self._read_range(response, start, last)
                self._cache[block] = data
                while len(self._cache) > self.cache_blocks:
                    self._cache.popitem(last=False)
                inside = offset - block * self.block_size
                count = min(end - offset, len(data) - inside)
                parts.append(data[inside:inside + count])
                offset += count
            return b"".join(parts)

    @property
    def stats(self) -> dict:
        """Snapshot of exact upstream body-byte/request counters."""
        with self._lock:
            return dict(self._stats, remote_object_size=self._size,
                        fraction_transferred=(self._stats["bytes_transferred"] / self._size
                                              if self._size else None),
                        elapsed_seconds=(self._finished or time.monotonic()) - self._started,
                        counter_scope="upstream response bodies; includes probes/repeated bytes, excludes headers",
                        errors=list(self._stats["errors"]))

    def close(self) -> None:
        with self._lock:
            self._closed = True
            if self._finished is None:
                self._finished = time.monotonic()
            self._cache.clear()
            if self._fallback:
                self._fallback.close()
                self._fallback = None
            if self._temporary:
                os.unlink(self._temporary)
                self._temporary = None


def open_source(path_or_url: str | Path, **http_options) -> SeekableSource:
    """Open a local file or HTTP(S) URL through the same seekable interface."""
    if is_http_url(path_or_url):
        return HTTPRangeSource(str(path_or_url), **http_options)
    return LocalFileSource(path_or_url)


class FFmpegSource:
    """Expose a seekable remote source to FFmpeg via ephemeral loopback HTTP.

    Local paths pass through unchanged. Close after the last lazy dataset read.
    ``stats`` is available before/after close and includes all FFmpeg reads.
    """
    def __init__(self, path_or_url: str | Path, **http_options) -> None:
        self.original = str(path_or_url)
        self.source = None
        self._server = None
        self._thread = None
        self.path = self.original
        if not is_http_url(path_or_url):
            return
        self.source = open_source(path_or_url, **http_options)
        source = self.source
        # A random path prevents unrelated local requests from consuming this source.
        token = os.urandom(16).hex()

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_HEAD(self):
                self.respond(False)

            def do_GET(self):
                self.respond(True)

            def respond(self, body: bool):
                if self.path != "/" + token:
                    self.send_error(404)
                    return
                size = source.size()
                start, end = 0, size - 1
                requested = self.headers.get("Range")
                if requested:
                    match = re.fullmatch(r"bytes=(\d*)-(\d*)", requested)
                    if not match or not any(match.groups()):
                        self.send_error(400, "Invalid Range")
                        return
                    if not match[1]:
                        start = max(0, size - int(match[2]))
                    else:
                        start = int(match[1])
                        if match[2]:
                            end = min(end, int(match[2]))
                    if start >= size or end < start:
                        self.send_response(416)
                        self.send_header("Content-Range", f"bytes */{size}")
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                self.send_response(206 if requested else 200)
                self.send_header("Content-Length", str(max(0, end - start + 1)))
                self.send_header("Accept-Ranges", "bytes")
                if requested:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.end_headers()
                if not body:
                    return
                try:
                    while start <= end:
                        data = source.read(start, min(65536, end - start + 1))
                        if not data:
                            raise OSError("Source returned a short read")
                        self.wfile.write(data)
                        start += len(data)
                except (BrokenPipeError, ConnectionResetError):
                    self.close_connection = True
                except Exception as exc:
                    source._stats["errors"].append(str(exc))
                    self.close_connection = True

        class Server(http.server.ThreadingHTTPServer):
            daemon_threads = False
            block_on_close = True

        try:
            self._server = Server(("127.0.0.1", 0), Handler)
            self.path = f"http://127.0.0.1:{self._server.server_port}/{token}"
            self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
            self._thread.start()
        except Exception:
            self.source.close()
            raise

    @property
    def stats(self) -> dict:
        return self.source.stats if isinstance(self.source, HTTPRangeSource) else {"mode": "local"}

    def close(self) -> None:
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._thread.join()
            self._server = None
        if self.source:
            self.source.close()

    def __enter__(self) -> FFmpegSource:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


def _main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--block-size", type=int, default=1024**2)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--fallback-limit", type=int, default=1024**3)
    parser.add_argument("--no-http-fallback", action="store_true")
    args = parser.parse_args()
    try:
        with FFmpegSource(args.url, block_size=args.block_size, timeout=args.timeout,
                          fallback_limit=args.fallback_limit, fallback=not args.no_http_fallback) as adapter:
            print(adapter.path, flush=True)
            print(adapter.source.size(), flush=True)
            print(adapter.stats["mode"], flush=True)
            sys.stdin.readline()  # Parent closes this pipe on every exit path.
        print(json.dumps(adapter.stats), flush=True)
        if adapter.stats["errors"]:
            raise OSError("; ".join(adapter.stats["errors"]))
    except Exception as exc:
        print(f"HTTP input failed: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    _main()
