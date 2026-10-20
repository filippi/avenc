from __future__ import annotations

import base64
import struct
import zlib

import numpy as np
import pytest

from avenc_xarray.codec import _decode_metadata_comment


def _string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def _v2_comment(*, include_coords: bool = True) -> str:
    flags = 1 if include_coords else 0
    payload = bytearray(b"AV2META\0")
    payload += struct.pack("<7I", 2, flags, 3, 2, 1, 1, 0)
    payload += struct.pack("<iIIiddIdIIII", 0, 1, 10, 6, 200.0, 300.0, 0, 1.0, 3, 2, 3, 2)
    payload += _string("") + _string("template")
    payload += struct.pack("<QiIiQddIii", 0, 0, 0, 0, 0, 200.0, 300.0, 0, 16, 0)
    for value in ("2t", "surface", "0", "20250101", "0000", "0", "2025-01-01T00:00:00Z", "0", "0", ""):
        payload += _string(value)
    if include_coords:
        payload += struct.pack("<I2dI3d", 2, 50.0, 49.5, 3, 1.0, 1.5, 2.0)
    return "AVENCv2|" + base64.b64encode(zlib.compress(payload)).decode("ascii")


def test_decode_binary_v2_to_packed_metadata() -> None:
    metadata = _decode_metadata_comment(_v2_comment())
    assert metadata["schema"] == "avenc.pack.v1"
    assert metadata["metadata_version"] == 2
    assert metadata["shape"] == [1, 2, 3]
    assert metadata["groups"][0]["short_name"] == "2t"
    assert metadata["groups"][0]["coords"]["time_g0"]["data"]["values"] == [
        "2025-01-01T00:00:00Z"
    ]
    np.testing.assert_allclose(metadata["coords"]["lat"]["data"]["values"], [50.0, 49.5])
    np.testing.assert_allclose(metadata["coords"]["lon"]["data"]["values"], [1.0, 1.5, 2.0])


def test_decode_original_binary_v2_without_coordinate_extension() -> None:
    metadata = _decode_metadata_comment(_v2_comment(include_coords=False))
    assert metadata["coords"] == {}
    assert metadata["groups"][0]["frame_count"] == 1


def test_decode_binary_v2_rejects_truncation() -> None:
    comment = _v2_comment()
    compressed = base64.b64decode(comment.removeprefix("AVENCv2|"))
    payload = zlib.decompress(compressed)[:-1]
    truncated = "AVENCv2|" + base64.b64encode(zlib.compress(payload)).decode("ascii")
    with pytest.raises(ValueError, match="Truncated AVENCv2 field"):
        _decode_metadata_comment(truncated)
