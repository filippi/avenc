# AVENC usage guide for agents

Read README.md for installation and current shortcuts. This guide concerns the
software repository, not the separate paper or large datasets. Check actual CLI
help and installed FFmpeg capabilities before choosing an encoder. Do not commit
or push changes unless the author asks.

## Choose the right interface

| Task | Interface | Limitation |
| --- | --- | --- |
| Pack meteorological GRIB | grib2avenc → avpack → FFmpeg | Source field grouping/quantization is scientific state |
| Reconstruct GRIB or export Float32 | avenc2grib | GOP predecessors and full spatial frames may decode |
| Read local/HTTP GRIB-packed MP4 lazily | xarray engine="avenc", avenc.open_avenc | Wheel includes the CLI decoder; FFmpeg remains external |
| Encode an array | avenc_xarray.compress_dataarray | Separate implementation; materializes Float32 T,Y,X |
| Compute reconstruction metrics | avenc-compare | Field identity first; sampled/full scope matters |
| Export Zarr | avenc2grib output.zarr / --zarr-out | Experimental Zarr v2 per-stream layout |
| Inspect metadata in JavaScript | www/js/avenc_metadata.js | HTTP Range/CORS requirements apply |
| View/extract in browser | www/viewer.html, www/extractor.html | Shared model currently downloads the whole MP4 |

## Build and prerequisites

Native C++17/CMake tools require ecCodes and zlib. FFmpeg/ffprobe are runtime
executables and must include the requested codec/pixel format. The Python package
build is defined at the root pyproject.toml. The wheel includes its pybind11
extension and all C++ tools with ecCodes libraries; it does not include FFmpeg.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
python -m pip install '.[test]'
export PATH="$PWD/bin:$PATH"
```

Do not hard-code a workstation dependency path. Use ECCODES_HOME or
CMAKE_PREFIX_PATH. ENABLE_NATIVE_ARCH=ON is optional and makes builds less
portable. ENABLE_FAST_MATH must remain OFF for scientific missing-value handling.
Native code targets Linux/macOS POSIX facilities; Windows requires a compatible
Linux/WSL environment.

## Encode, inspect, compare

```bash
bin/grib2avenc --lossless input.grib output.mp4
bin/grib2avenc --high source/*.grib high.mp4
bin/grib2avenc --compact source/*.grib compact.mp4
bin/grib2avenc --enc hevc_good10_crf42 input.grib custom.mp4
bin/grib2avenc --list-options
bin/avenc2grib --list output.mp4
bin/avenc2grib --stats output.mp4
bin/avenc2grib output.mp4 restored.grib
bin/avenc-compare --full input.grib output.mp4 --json errors.json
```

Default/--lossless is av1_lossless12_auto_400. --high is hevc_vgood12; --medium is
vp9_hq10_auto_420; --compact/--low is av1_good10_auto_400; --fast is avc_xcr8.
All shortcuts use auto range selection; --apple is a software HEVC alias,
not automatic VideoToolbox acceleration. Profiles are not universal guarantees.

Use --enc_config_table for a custom CSV or avpack directly for low-level options.
Copy the exact resolved encoder, bit depth, pixel format, quantization mode,
FFmpeg options and versions into measurement records. Hardware encoders require
an appropriate FFmpeg build/device; selecting a codec family alone is insufficient.

Auto selects frameMinMax/groupMinMax policy per stream. One MP4 may contain
both. Shared GRIB bitmaps are authoritative; default taper3 fills only masked
video cells before reconstruction reapplies the bitmap. --mask-fill min selects
minimum fill. Neighboring valid cells can still incur codec errors.

Video-lossless preserves video codes, not original scientific Float32/Float64
values. Quantization, precision conversion and GRIB repacking introduce separate
errors. Message order can change: match variable, level, time, member and grid.
Read avenc-compare.md for normalization, reservoirs, quantiles and sampled flags.
--fast comparison can miss extremes; it is not the --fast encoder setting.

## Lazy Python reads

```python
import xarray as xr

with xr.open_dataset("output.mp4", engine="avenc", cache=False) as ds:
    print(ds)
    name = next(iter(ds.data_vars))
    da = ds[name]
    selected = da.isel({dim: (0 if i == 0 else slice(0, 8))
                       for i, dim in enumerate(da.dims)}).load()
```

Inspect dimensions/coordinates before selection; don't assume all variables
share one time axis or that alphabetical stream order matches ensemble members.
Use backend_kwargs={"avenc2grib_bin": "/path/to/avenc2grib"} if not on PATH.
Close datasets. A lazy object is not evidence of cheap random access or minimal
network transfer. Dask coordinates requests but doesn't remove GOP dependencies.

## HTTP

```python
from avenc import open_avenc

with open_avenc("https://your-server.example/output.mp4") as ds:
    name = next(iter(ds.data_vars))
    frame = ds[name].isel({ds[name].dims[0]: 0}).load()
    print(ds.avenc.http_stats)
```

The CLI takes HTTP(S) URLs as inputs as well. Local HTTP source helpers use
Python's standard library and bounded caches. Backend http_options include
block_size, cache_blocks, timeout, fallback and fallback_limit. Servers ignoring
Range can trigger a bounded full download. Report actual bytes/requests, cache
conditions, decoded fields and timing scope, rather than claiming streaming from
an HTTP URL alone. JavaScript needs CORS in addition to Range; Python does not.
Do not fetch large remote datasets for a routine code check.

## Python array encoding

```python
from avenc_xarray import compress_dataarray, decompress_to_dataarray

compress_dataarray(da, "field.avenc", bits=8, codec="avc", quality="high",
                   bitmask_enabled=False)
restored = decompress_to_dataarray("field.avenc")
```

Input must be three-dimensional with a time dimension. Other leading dimensions
need an explicitly reversible mapping outside this helper. Preserve shape, dtype,
coordinates and axis meaning. It materializes contiguous Float32 and writes
payload.mp4 with a native-array JSON schema; it is not the C++ GRIB packer or a
streaming Dask writer. Its defaults/profiles/masks are separate.

Default special-value masking uses values close to zero; disable it when near-zero
values must remain distinct. Constant and all-nonfinite arrays are rejected.
Do not assume lossy-video NaN codes are an exact changing missing-value mask.
Check missingness, Float64 conversion error, odd dimensions, padding and actual
encoder bit-depth support against the original array. The codec has optional
nonlinear/color modes; use only with a measured scientific justification.

## Subsets and Zarr

```bash
bin/avenc2grib --stream-id 0 --frame-start 0 --frame-count 4 \
  --ni-start 20 --nj-start 30 --width 16 --height 12 output.mp4 > subset.f32
bin/avenc2grib --zarr-compress gzip output.mp4 decoded.zarr
bin/avenc2grib --stream-id 0 --frame-start 0 --frame-count 4 \
  --ni-start 20 --nj-start 30 --width 16 --height 12 \
  --zarr-out subset.zarr output.mp4
```

Float32 output is little-endian (frame,row,column), flattened as
((t*height)+row)*width+column. Zarr export creates new paths, Zarr v2 stream arrays
and quantization/time metadata; gzip/none are supported. Inspect the schema before
assuming a training-ready layout or complete CF metadata. It doesn't recover
precision already lost in MP4. xarray exports require their own zarr/netCDF extras.

## Browser tools and validation

See www/README.md. Serve localhost/HTTPS; WebCodecs depends on browser and codec.
The metadata inspector performs Range reads; viewer/extractor currently downloads
the complete MP4. CDN dependencies and CORS apply. Bounded decoder queues do not
prove a bounded initial compressed-file download.

```bash
ctest --test-dir build --output-on-failure
python -m pytest -q python/avenc/tests/test_metadata_v2.py
python -m unittest discover -s tests -p test_avenc_compare.py
python -m unittest discover -s python/avenc/tests -p test_http_input.py
node www/tests/metadata.mjs
node www/tests/decoder.mjs
```

AVENC_BIN_DIR selects an out-of-tree native test build. AVENC_TEST_MP4,
AVENC_TRUTH_GRIB and AVENC2GRIB_BIN configure data-dependent xarray/Dask checks.
External HTTP checks require AVENC_HTTP_INTEGRATION=1 and AVENC_HTTP_TEST_URL.
Do not report skipped tests as passed. Use tiny synthetic data for development;
never add private datasets, credentials, local environments or generated media
to the public tree. Read RELEASE_CHECKS.md for the most recent local validation.
