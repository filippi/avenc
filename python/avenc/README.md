> Installation is now unified at the repository root: `python -m pip install .`
> (or `pip install avenc` after publication). The distribution is `avenc` and
> includes native CLI tools; imports below are unchanged. See ../../docs/INSTALL.md.

# AVENC xarray engine

Python modules shipped in the unified `avenc` distribution, providing the `avenc` xarray engine, convenience
`avenc.open_avenc`, and a separate native array encoder.

## Install

Install FFmpeg/ffprobe first. Reading GRIB-packed MP4s also requires the C++
`avenc2grib` tool from the repository root on PATH. Python packaging does not
bundle the CLI or FFmpeg.

```bash
python -m pip install .
# From this directory instead: python -m pip install ../..
```

A compiler and Python development headers are needed to build the pybind11
extension. PEP 517 installs pybind11/scikit-build-core as build dependencies;
CMake does not clone a separate Git checkout. Both source distributions and
platform-specific wheels include their own Apache-2.0 licence file.

## Read a GRIB-packed MP4 lazily

```python
import xarray as xr

with xr.open_dataset("weather.mp4", engine="avenc", cache=False) as ds:
    print(ds)
    name = next(iter(ds.data_vars))
    first = ds[name].isel({ds[name].dims[0]: 0}).load()
```

Supply an explicit decoder when needed:
`backend_kwargs={"avenc2grib_bin": "/path/to/avenc2grib"}`.
Variable/dimension names come from the file; inspect them before selecting.
The backend reads GRIB-packed AVENC v1/v2 metadata and its native array schema.

## HTTP

```python
from avenc import open_avenc

with open_avenc("https://your-server.example/weather.mp4") as ds:
    print(ds)
    print(ds.avenc.http_stats)
```

Configure access with `backend_kwargs={"http_options": {"block_size": 1048576,
"cache_blocks": 8, "timeout": 30, "fallback": True,
"fallback_limit": 1073741824}}`. Close datasets to release their sources.
Metadata reads do not decode video. Late frame requests can still read preceding
GOPs; servers ignoring Range may trigger a bounded full-download fallback.

## Encode a DataArray

```python
from avenc_xarray import compress_dataarray, decompress_to_dataarray

# da must have exactly three dimensions including time.
summary = compress_dataarray(da, "array.avenc", bits=10,
                             codec="avc", quality="high",
                             bitmask_enabled=False)
restored = decompress_to_dataarray("array.avenc")
```

This separate path writes `array.avenc/payload.mp4` with JSON array metadata.
It materializes a contiguous Float32 input. CLI profiles, C++ bitmap authority
and its auto-range policy are separate from this helper. Default special-value
masking can treat values close to zero as mask values; disable it when those
values must remain distinct. Constant/all-nonfinite inputs are not supported by
the helper. Float64 precision, missing-value fidelity, odd dimensions and encoder
bit-depth support need explicit validation for the intended data.

Examples: `examples/encode_array.py`, `examples/read_subset.py`.
Tests (repository root): `python -m pip install '.[test]'` and the root README test commands.
