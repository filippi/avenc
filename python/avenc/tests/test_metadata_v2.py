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


def _v2_comment(*, include_coords: bool = True, version: int = 2, transform: int = 0) -> str:
    flags = 1 if include_coords else 0
    payload = bytearray(b"AV2META\0")
    payload += struct.pack("<7I", version, flags, 3, 2, 1, 1, 0)
    payload += struct.pack("<iIIiddId", 0, 1, 10, 6, 200.0, 300.0, transform, 1.0)
    if version == 4:payload += struct.pack("<3d",.25,-10.,20.)
    payload += struct.pack("<4I",3,2,3,2)
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


@pytest.mark.parametrize("version,mode", [(2,0),(2,1),(3,2),(4,3),(4,4)])
def test_transform_modes(version, mode):
    metadata = _decode_metadata_comment(_v2_comment(version=version, transform=mode))
    options = metadata["groups"][0]["options"]
    assert options["apply_log2"] == (mode == 1)
    assert options["apply_asinh"] == (mode in (2,3))
    assert metadata["binary_format_version"] == version
    assert options["apply_shifted_log"] == (mode==4)
    assert options["transform_scale"] == (.25 if version==4 else 1)


@pytest.mark.parametrize("version,mode", [(2,2),(3,3),(99,0)])
def test_reject_unknown_transform_or_binary_revision(version, mode):
    with pytest.raises(ValueError, match="Unsupported"):
        _decode_metadata_comment(_v2_comment(version=version, transform=mode))
