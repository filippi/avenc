# AVENC

Video compression and lazy access for meteorological GRIB grids.

## Install with pip

```sh
pip install avenc
```

**First release is being prepared; not yet published.** The commands above become
available after the authorized PyPI release. Use a virtual environment. Supported
binary-wheel targets: Python 3.11–3.14, Linux x86_64/aarch64 (glibc >=2.28), macOS
Intel/Apple Silicon (>=15). Install prebuilt FFmpeg separately with
`brew install ffmpeg` or `sudo apt install ffmpeg`; no FFmpeg compilation required.

## Check the installation

```sh
grib2avenc --help
avenc2grib --help
python -c "import avenc; print(avenc.__version__)"
```

## Convert GRIB to AVENC and back

```sh
grib2avenc input.grib output.mp4
avenc2grib output.mp4 reconstructed.grib
```

The default is video-lossless with finite quantization; it does not promise
byte-identical scientific values.

## Open using xarray

```python
import xarray as xr

with xr.open_dataset("output.mp4", engine="avenc", cache=False) as ds:
    print(ds)                         # inspect variable/dimension names
    field = ds[next(iter(ds.data_vars))]
    print(field.isel({field.dims[0]: 0}).load())
```

The wheel installs the native tools automatically and preserves `avenc` and
`avenc_xarray`. See [installation](docs/INSTALL.md) for pipx, Homebrew, standalone
archives, source builds, dependencies and troubleshooting, and [releases](docs/RELEASE.md)
for maintainers.

---

## AVENC design and detailed usage: meteorological grids as video streams

AVENC packs GRIB fields into MP4 video streams and embeds the metadata needed to
reconstruct scientific grids. It provides command-line encoding, GRIB/Float32/Zarr
reconstruction, lazy Python/xarray reads and a browser viewer/extractor.

Repository: https://github.com/filippi/avenc

## Build the command-line tools

Native builds target Linux and macOS. Windows users should use a Linux/WSL
environment; the native implementation uses POSIX facilities.

Install a C++17 compiler, CMake 3.18+, ecCodes, zlib and FFmpeg/ffprobe. Available
encoders and pixel formats depend on the FFmpeg build.

```bash
# Debian/Ubuntu prerequisites:
sudo apt install build-essential cmake libeccodes-dev zlib1g-dev ffmpeg

# From this repository:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
bin/grib2avenc --help
```

On macOS, install the corresponding dependencies with Homebrew, including
`cmake`, `eccodes` and `ffmpeg`. If ecCodes is not found, set `ECCODES_HOME`
or pass `-DCMAKE_PREFIX_PATH=/path/to/dependencies`.
`./build.sh` is a convenience wrapper for the same build commands.

Optional installation:

```bash
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

The subset library is installed beside the tools and the HTTP helper under
`share/avenc/`. HTTP CLI input additionally requires `python3` on PATH.
Builds are portable by default; `-DENABLE_NATIVE_ARCH=ON` opts into optimization
for the build CPU. Keep `ENABLE_FAST_MATH=OFF` for missing-value correctness.

## Encode GRIB fields

```bash
bin/grib2avenc input.grib output.mp4
bin/grib2avenc --compact source/*.grib output.compact.mp4
bin/grib2avenc --high source/*.grib output.high.mp4
bin/grib2avenc --list-options
```

Without an option, AVENC selects **Video-lossless**. One input with no output
name writes `<input>.mp4`; multiple inputs require an explicit MP4 output path.

| Option | Resolved profile | Purpose |
| --- | --- | --- |
| default / `--lossless` | `av1_lossless12_auto_400` | Lossless 12-bit AV1 video; quantization error remains |
| `--high` | `hevc_vgood12` | Low additional codec error |
| `--medium` | `vp9_hq10_auto_420` | Balanced trade-off |
| `--compact` / `--low` | `av1_good10_auto_400` | Compression priority |
| `--fast` | `avc_xcr8` | Speed priority |
| `--apple` | `hevc_xcr8` | Software HEVC alias; does not select VideoToolbox |

Very small grids may not fit a profile's tile arrangement (the default AV1
profile currently fails on a 64 × 64 synthetic field). Use `--fast` or a custom
encoder configuration with suitable tiling for such inputs.

All shortcuts use automatic quantization-range selection. `auto` selects the
range policy per grouped stream; it can mix `frameMinMax` and `groupMinMax` in
one MP4. `--enc` selects a named configuration; `--enc_config_table profiles.csv`
loads custom configurations. See `grib2avenc --help` and `avpack --help` for
advanced codec, normalization, grouping and metadata options.

The original GRIB bitmap is authoritative for missing cells. Stable shared
bitmaps are stored in metadata; the default `taper3` fill reduces discontinuities
in masked video regions before the original mask is restored. `--mask-fill min`
selects minimum fill instead. Fill is not a change to the scientific valid-cell
mask and does not guarantee an error bound on neighboring valid values.

**Video losslessness is not scientific losslessness.** Mapping physical values
to finite integer codes, precision conversion and subsequent GRIB repacking can
introduce error even with a lossless video codec. Evaluate each dataset/profile.

## Inspect, reconstruct and compare

```bash
bin/avenc2grib --list output.mp4
bin/avenc2grib --stats output.mp4
bin/avenc2grib output.mp4 reconstructed.grib
bin/avenc2grib --group stream:0 output.mp4 one_stream.grib
bin/avenc-compare --full input.grib output.mp4 --json errors.json
bin/avenc-compare --fast input.grib output.mp4 --json sampled-errors.json
```

Use an identically concatenated reference for multi-file inputs:
`cat source/*.grib > reference.grib`. Comparison matches scientific identity,
not positional message order. Full and sampled comparison are distinct scopes.
Detailed metrics, normalization and output options: [avenc-compare.md](avenc-compare.md).
The legacy `ComputeError` utility remains for compatibility.

Subset output is little-endian Float32, ordered `(frame, row, column)`:

```bash
bin/avenc2grib --stream-id 0 --frame-start 0 --frame-count 4 \
  --ni-start 20 --nj-start 30 --width 16 --height 12 output.mp4 > subset.f32
```

## Python/xarray

Install the unified package from the repository root; it includes the C++ CLI tools.

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install .
```

```python
import xarray as xr

with xr.open_dataset("output.mp4", engine="avenc", cache=False) as ds:
    print(ds)
    field = next(iter(ds.data_vars))
    # Inspect the variable's dimensions before selecting a point or window.
    first_frame = ds[field].isel({ds[field].dims[0]: 0}).load()
```

GRIB-packed MP4 files are decoded through `avenc2grib`. Native Python array
packages use a separate pybind11 codec. The Python `compress_dataarray` API can
encode a three-dimensional DataArray with a `time` dimension; its options,
metadata and masking policy are not interchangeable with `grib2avenc` profiles.
It materializes a contiguous Float32 input and is not a streaming Dask encoder.
See [the Python guide](python/avenc/README.md) and the runnable examples there.

## HTTP access

```python
from avenc import open_avenc

with open_avenc("https://your-server.example/output.mp4") as ds:
    name = next(iter(ds.data_vars))
    first_frame = ds[name].isel({ds[name].dims[0]: 0}).load()
    print(ds.avenc.http_stats)
```

The CLI also accepts HTTP(S) input URLs. A shared seekable source provides a
bounded byte-range cache and transfer statistics. Servers ignoring Range can
use a bounded temporary-file fallback. Lazy opening does not prove minimal
transfer or spatially selective decoding: preceding GOP frames/full spatial
frames may still need decoding. Measure the actual request and bytes transferred.

## Experimental Zarr export

```bash
bin/avenc2grib --zarr-compress gzip output.mp4 decoded.zarr
bin/avenc2grib --stream-id 0 --frame-start 0 --frame-count 4 \
  --ni-start 20 --nj-start 30 --width 16 --height 12 \
  --zarr-out subset.zarr output.mp4
```

The exporter writes Zarr v2 arrays per stream, plus quantization/time metadata.
The layout is experimental, not a universal training-ready schema. This is an
export from reconstructed values, not a new accuracy guarantee.

## Browser tools

`www/viewer.html` and `www/extractor.html` use WebCodecs and MP4Box. Serve the
folder over HTTP on localhost; put your files under `www/datasets/` (not tracked).

```bash
python3 -m http.server 8000 --directory www
# Open http://localhost:8000/viewer.html?dataset=./datasets/output.mp4
```

`www/index.php` is an optional dataset listing and requires PHP rather than the
Python server. The metadata inspector uses Range, but the shared viewer/extractor
model currently loads the whole MP4. Browser codec support, secure-context
requirements, CORS and CDN availability apply. See [www/README.md](www/README.md).

## Tests

```bash
ctest --test-dir build --output-on-failure
python -m pip install '.[test]'
python -m pytest -q python/avenc/tests/test_metadata_v2.py
python -m unittest discover -s tests -p 'test_avenc_compare.py'
python -m unittest discover -s python/avenc/tests -p 'test_http_input.py'
node www/tests/metadata.mjs
node www/tests/decoder.mjs
```

The comparison and local HTTP tests generate small synthetic fields. External
network tests are opt-in. Dataset-dependent xarray/Dask checks require
`AVENC_TEST_MP4`, optionally `AVENC_TRUTH_GRIB` and `AVENC2GRIB_BIN`, and skip if
inputs are unavailable. `AVENC_BIN_DIR` selects an out-of-tree native test build.

## Research data and licence

The manuscript and large experiments are maintained separately from the software.
Dataset identifier: https://doi.org/10.5281/zenodo.23502851 (reserved for the
accompanying archive; availability depends on publication).

AVENC is licensed under [Apache-2.0](LICENSE). FFmpeg, ecCodes, codecs and browser
libraries retain their own licences. Hardware acceleration depends on the encoder,
FFmpeg build and device; it is not enabled merely by choosing a profile name.
Agent-oriented usage and limitations: [skills.md](skills.md).
