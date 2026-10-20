# Local release preparation — 20 October 2026

Prepared for https://github.com/filippi/avenc. The directory is not yet a Git
repository. No initial commit, tag, remote update or push has been made.

## What changed

- Preserved copied local environments, builds and old generated demonstrations
  outside this source tree; added ignore rules for future generated files.
- Replaced personal build paths with portable CMake wrappers; made CPU-specific
  optimization opt-in and retained fast-math OFF.
- Verified installation of the CLI, shared subset library, public header and
  Python HTTP helper; installed binaries locate their sibling library.
- Repaired standalone Python licensing, dependency discovery and wheel ABI tags;
  the build uses the declared pybind11 dependency rather than cloning it.
- Added native comparison tests and CTest metric checks to the clean source tree.
- Included the requested browser tools and their synthetic Node tests.
- Updated current defaults, auto quantization, bitmap fill, HTTP/browser limits,
  Python-array differences and experimental Zarr export documentation.
- Replaced private-data examples with runnable synthetic and generic read examples.

The native compression/quantization/mask algorithms and Python codec source were
not changed. Native source text changes are limited to help/demo output paths.
Packaging, compiler portability, examples, tests and docs have changed.
The article and research data remain separate repositories/archives.

## Checks completed

On macOS / Apple Silicon, AppleClang 17 and FFmpeg 8.0.1:

| Check | Result |
| --- | --- |
| Fresh native CMake build | Passed, all targets |
| CTest metric/mask/sampling/SHA-256 checks | 1 passed |
| Native identity-matched comparison integration tests | 11 passed |
| Installed-wheel metadata/HTTP/xarray/Dask tests with tiny synthetic input | 17 passed, 2 skipped, 7 subtests passed |
| Browser metadata and decoder lifecycle/cache tests | Both Node scripts passed |
| Standalone Python sdist → CPython 3.11 wheel | Built successfully, installed and imported outside checkout |
| CPython 3.14 wheel | Built successfully; installed import and array example passed |
| Synthetic Python array encode/decode | Passed on Python 3.11 and 3.14 |
| Installed CLI default Video-lossless profile | Passed on a 512 × 128 synthetic grid |
| Zarr v2 gzip subset export | Values exactly equal direct Float32 subset output |
| Markdown local links / static credential-pattern scan | No broken file links or matching credential patterns found |

The two skips are the opt-in external HTTP test and the optional independent
truth-GRIB fixture test. Native comparison tests independently compare decoded
values/metrics to synthetic reference values. No large experiments were rerun.
Node tests use synthetic/mocked browser primitives, not physical GPU decoders.
The static pattern scan is not a comprehensive security audit.

The clean Python 3.11 checks used NumPy 2.4.6, xarray 2026.9.0, pandas 3.0.6,
Dask 2026.8.0, cfgrib 0.9.15.1 and Python ecCodes 2.50.0. Native ecCodes is the
locally installed C library. Python 3.14 import/array checks used NumPy 2.5.4.
Wheels are platform/ABI-specific; no universal compiled wheel is claimed.
The pybind11 3.x build dependency supports current Python versions; upstream
release details: https://pybind11.readthedocs.io/en/stable/changelog.html.

## Known limits retained

- The default AV1 profile's tiling does not fit a 64 × 64 field in this FFmpeg
  build. It works on the tested 512 × 128 field. Use --fast or a custom tiling
  configuration for tiny grids; historical profile behavior was not silently
  changed during cleanup.
- CLI/Python remote reads can decode preceding GOPs/full frames. Browser
  viewer/extractor loads the entire MP4; metadata-only inspection can use Range.
- Python array encoding materializes Float32 and has separate masking/schema
  rules from the C++ GRIB packer; see its guide for unsupported inputs.
- Linux and Windows/WSL were not executed locally; native source uses POSIX APIs.
  Hardware acceleration and external public HTTP endpoints were not validated.
- Lossless video is not a scientific error bound. Validate each input/profile.

The working build environments and wheels were kept outside the public source
tree. GitHub publication and the first source commit remain author actions.
