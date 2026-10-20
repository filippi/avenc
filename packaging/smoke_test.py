"""Test an installed wheel from an unrelated directory using synthetic GRIBs.

The 512-column fixture accommodates the unchanged default AV1 tile setting.
Video-lossless tolerance includes the existing FFmpeg gray16->gray12 conversion
and GRIB repacking (up to 1.5 quantization steps, plus roundoff). Quantized
video samples are checked separately after pixel-format conversion; this is NOT
a claim of scientific bitwise losslessness.
"""
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import tempfile
import eccodes as ec
import numpy as np
import xarray as xr
import avenc

TOOLS = ("grib2avenc", "avenc2grib", "avpack", "avenc-compare", "ComputeError", "grib_pair_extract", "avenc_test_generator")

def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def read(path):
    fields = {}
    with open(path, "rb") as source:
        while (handle := ec.codes_grib_new_from_file(source)) is not None:
            key = tuple(ec.codes_get(handle, k) for k in ("shortName", "typeOfLevel", "level", "step"))
            fields[key] = (ec.codes_get_values(handle), ec.codes_get_array(handle, "bitmap"))
            ec.codes_release(handle)
    return fields


def main():
    assert avenc.__version__ == importlib.metadata.version("avenc")
    assert "avenc" in xr.backends.list_engines()
    for tool in TOOLS:
        assert avenc.__version__ in run(tool, "--version")
    run("grib2avenc", "--help")
    run("avenc2grib", "--help")
    with tempfile.TemporaryDirectory(prefix="avenc-wheel-") as tmp:
        os.chdir(tmp)
        with open("reference.grib", "wb") as destination:
            for name in ("10u", "10v"):
                for step in (0, 3):
                    h = ec.codes_grib_new_from_samples("regular_ll_sfc_grib2")
                    for key, value in {"Ni":512, "Nj":128, "latitudeOfFirstGridPointInDegrees":60., "latitudeOfLastGridPointInDegrees":47.3, "longitudeOfFirstGridPointInDegrees":0., "longitudeOfLastGridPointInDegrees":51.1, "iDirectionIncrementInDegrees":0.1, "jDirectionIncrementInDegrees":0.1, "shortName":name, "step":step, "bitsPerValue":24, "bitmapPresent":1, "missingValue":9999.}.items():
                        ec.codes_set(h, key, value)
                    y, x = np.mgrid[:128, :512]
                    values = (4 + np.sin(x/40) + np.cos(y/20) + step/10 + (name == "10v")).ravel()
                    values[:512] = 9999.
                    ec.codes_set_values(h, values)
                    ec.codes_write(h, destination)
                    ec.codes_release(h)
        original = read("reference.grib")
        for profile in ("--lossless", "--fast"):
            run("grib2avenc", profile, "reference.grib", "encoded.mp4")
            run("avenc2grib", "encoded.mp4", "decoded.grib")
            decoded = read("decoded.grib")
            assert original.keys() == decoded.keys(), "Field identities/count changed"
            for key, (reference, bitmap) in original.items():
                actual, actual_bitmap = decoded[key]
                np.testing.assert_array_equal(bitmap, actual_bitmap)
                valid = bitmap.astype(bool)
                # Auto may use a group range across both time steps.
                span = max(v[0][v[1].astype(bool)].max() for k,v in original.items() if k[0] == key[0]) - min(v[0][v[1].astype(bool)].min() for k,v in original.items() if k[0] == key[0])
                tolerance = 1.5 * span / 4094 + 2e-5 if profile == "--lossless" else 0.25
                np.testing.assert_allclose(actual[valid], reference[valid], atol=tolerance, rtol=0)
            if profile == "--lossless":
                from avenc_xarray.codec import _decode_metadata_comment
                info = json.loads(run("ffprobe", "-v", "error", "-show_format", "-of", "json", "encoded.mp4"))
                metadata = _decode_metadata_comment(info["format"]["tags"]["comment"])
                for group in metadata["groups"]:
                    record = group["records"][0]
                    key = (group["short_name"], group["type_of_level"], int(group["level"]), int(record["step_range"]))
                    reference, bitmap = original[key]
                    valid = bitmap.astype(bool)
                    low, high = record["quantization_min"], record["quantization_max"]
                    codes = np.zeros(reference.shape, dtype="<u2")
                    codes[valid] = (1 + np.floor(np.clip((reference[valid]-low)/(high-low),0,1)*4094+0.5)).astype("<u2") << 4
                    converted = subprocess.check_output(["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray16le", "-s", "512x128", "-i", "pipe:0", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "gray12le", "pipe:1"], input=codes.tobytes())
                    decoded_codes = subprocess.check_output(["ffmpeg", "-v", "error", "-i", "encoded.mp4", "-map", f"0:{group['stream_index']}", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "gray12le", "pipe:1"])
                    np.testing.assert_array_equal(np.frombuffer(converted, dtype="<u2")[valid], np.frombuffer(decoded_codes, dtype="<u2")[valid])
            with xr.open_dataset("encoded.mp4", engine="avenc", cache=False) as dataset:
                assert len(dataset.data_vars) >= 2
                field = next(iter(dataset.data_vars.values()))
                subset = field.isel({d:slice(1,3) for d in field.dims}).load()
                assert subset.size > 0
        print("PASS: installed tools, version, field identities, masks, quantization accuracy, xarray subset")

if __name__ == "__main__": main()
