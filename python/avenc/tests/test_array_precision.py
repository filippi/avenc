"""Check numerical precision through the public array API and its lazy reader."""
from __future__ import annotations

import copy
import shutil
import subprocess

import numpy as np
import pytest
import xarray as xr

from avenc_xarray import compress_dataarray, decompress_to_dataarray
from avenc_xarray import _avenc_ext
from avenc_xarray.backend import AvencBackendEntrypoint
from avenc_xarray.codec import _encode_metadata_comment

pytestmark = pytest.mark.skipif(
    not shutil.which("ffmpeg") or not shutil.which("ffprobe"),
    reason="FFmpeg and ffprobe are required for precision integration checks",
)


def require_encoder(name):
    available = subprocess.check_output(["ffmpeg", "-hide_banner", "-encoders"], text=True)
    if name not in available:
        pytest.skip(f"FFmpeg encoder {name} unavailable")


def ramp():
    # Every 16-bit code, a physically plausible temperature range, and two times
    # exercise quantization independently of the image's spatial complexity.
    plane = np.linspace(150, 450, 65536, dtype=np.float32).reshape(256, 256)
    return xr.DataArray(
        np.stack([plane, plane[::-1, ::-1]]),
        dims=("time", "y", "x"), name="temperature",
        coords={"time": [0, 1], "y": np.arange(256), "x": np.arange(256)},
        attrs={"units": "K"},
    )


def check_roundtrip(data, summary, bits):
    result = decompress_to_dataarray(summary.video_path)
    tolerance = 300 / (2 * (2**bits - 1)) + 6e-5  # Float32 reconstruction slack
    assert np.max(np.abs(result.values - data.values)) <= tolerance
    assert np.unique(result.values[0]).size == 2**bits
    np.testing.assert_array_equal(result.time, data.time)
    assert result.attrs["units"] == "K"
    with AvencBackendEntrypoint().open_dataset(summary.video_path) as ds:
        subset = ds.temperature.isel(time=slice(1, 2), y=slice(31, 53), x=slice(61, 89)).values
    np.testing.assert_array_equal(subset, result.values[1:2, 31:53, 61:89])


@pytest.mark.parametrize("bits,pix_fmt", [(8, "gray"), (10, "gray10le"), (12, "gray12le"), (16, "gray16le")])
def test_ffv1_array_precision(tmp_path, bits, pix_fmt):
    require_encoder("ffv1")
    data = ramp()
    summary = compress_dataarray(data, tmp_path, bits=bits, codec="ffv1", quality="lossless",
                                 bitmask_enabled=False, video_name="payload.mkv")
    assert summary.options["pix_fmt"] == pix_fmt
    assert summary.metadata["options"]["pix_fmt"] == pix_fmt
    check_roundtrip(data, summary, bits)


def test_hevc_twelve_bit_array_precision(tmp_path):
    require_encoder("libx265")
    data = ramp()
    summary = compress_dataarray(data, tmp_path, bits=12, codec="hevc", quality="lossless",
                                 bitmask_enabled=False)
    check_roundtrip(data, summary, 12)


def test_legacy_implicit_gray16_package_still_reads(tmp_path):
    require_encoder("ffv1")
    data = ramp()
    summary = compress_dataarray(data, tmp_path / "new", bits=12, codec="ffv1", quality="lossless",
                                 bitmask_enabled=False, video_name="payload.mkv")
    metadata = copy.deepcopy(summary.metadata)
    del metadata["options"]["pix_fmt"]
    options = dict(metadata["options"], comment_metadata=_encode_metadata_comment(metadata))
    legacy = tmp_path / "legacy.mkv"
    _avenc_ext.encode(np.asarray(data.values, dtype=np.float32, order="C"), str(legacy), options)
    result = decompress_to_dataarray(legacy)
    # Preserve the historical interpretation; missing low bits cannot be restored.
    assert np.max(np.abs(result.values - data.values)) < 2
    assert np.unique(result.values[0]).size < 300
    with AvencBackendEntrypoint().open_dataset(legacy) as ds:
        subset = ds.temperature.isel(time=slice(1, 2), y=slice(31, 53), x=slice(61, 89)).values
    np.testing.assert_array_equal(subset, result.values[1:2, 31:53, 61:89])
