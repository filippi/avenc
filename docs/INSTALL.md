# Installation

## Release status

The release system is implemented locally. No GitHub push, tag, PyPI release,
TestPyPI release or Homebrew tap has been published by this preparation task.
Commands that download AVENC require the first authorized release.

## Python and command-line tools

Use Python 3.11–3.14 in a virtual environment:

```sh
python3.12 -m venv .venv
. .venv/bin/activate
python -m pip install avenc
grib2avenc --help
avenc2grib --version
```

Binary wheels target Linux x86_64/aarch64 (glibc >=2.28) and macOS Intel/Apple
Silicon (macOS >=15). Wheels bundle the native tools and redistributable ecCodes
dependency closure. FFmpeg and ffprobe remain external executables:

```sh
# macOS
brew install ffmpeg
# Debian/Ubuntu
sudo apt install ffmpeg
```

Default video-lossless encoding needs libaom AV1 with gray12le support. `--fast`
uses libx264; other profiles depend on the encoders shown by
`grib2avenc --list-options` and `ffmpeg -encoders`. Hardware encoding depends on
FFmpeg, hardware and drivers; a wheel does not guarantee GPU codec availability.
CLI wrappers give an actionable missing-FFmpeg diagnostic. Python array encoding
also calls FFmpeg; ensure both ffmpeg and ffprobe are on PATH.

For CLI-only use, `pipx install avenc` creates its own environment. This does not
make imports available in a separate notebook environment; install AVENC in that
notebook's Python as well. `python -m pip install --user avenc` is appropriate only
for an unmanaged Python permitting user installs; ensure its scripts directory is
on PATH. Prefer venv/pipx on PEP 668 managed systems; do not override their guard.
Uninstall with `python -m pip uninstall avenc` or `pipx uninstall avenc`.

Do not install the old `avenc-xarray` distribution alongside `avenc`: they share
module names. Uninstall the former first. Existing `avenc` and `avenc_xarray`
imports and `engine="avenc"` registration are retained.

## Source install

```sh
git clone https://github.com/filippi/avenc.git
cd avenc
python -m pip install .
```

This PEP 517 build needs a C++17 compiler, CMake, zlib headers, and internet access
to pinned build dependencies. It obtains ecCodes headers/libraries from the
upstream binary wheel, bundles their dependency closure and builds all tools.
Run from the root; the former python/avenc-only distribution is retired.

## Standalone binaries

Download the native .tar.gz for your platform and SHA256SUMS from a release.
Verify the digest, extract, then:

```sh
./install.sh "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
grib2avenc --version
# Uninstall this native installation:
sh "$HOME/.local/share/avenc/uninstall.sh" "$HOME/.local"
```

For /usr/local use `sudo ./install.sh /usr/local`. Existing commands are never
overwritten. All native libraries live in libexec/avenc; no Python is needed for
local GRIB conversion. HTTP range access uses an optional Python 3 stdlib helper.
FFmpeg is still external. Standalone archives do not include Python bindings.
No .deb package is provided initially: tarballs and Homebrew cover installation
without maintaining another package format.

## Native source build

Install ecCodes/zlib development packages and FFmpeg, then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
# Alternatively: sudo cmake --install build --prefix /usr/local
```

Use ECCODES_HOME or CMAKE_PREFIX_PATH for nonstandard ecCodes installations.
Source builds use system ecCodes; keep that installation available. The subset
library is installed beside the executables with relative runtime search paths.
CMake installs are recorded in build/install_manifest.txt; remove only the listed
files if uninstalling a manual source build. Prefer the archive installer for an
automated uninstall. Fast-math and machine-specific instructions stay disabled.

## Homebrew

Initially the checked-in Formula/avenc.rb supports `--HEAD` after the first push:

```sh
brew install --HEAD ./Formula/avenc.rb
brew test avenc
```

Stable distribution uses the dedicated filippi/homebrew-avenc repository, which
must first be created/populated by the author. After publishing its formula:

```sh
brew tap filippi/avenc
brew install avenc
```

This is a planned tap, not a claim of inclusion in homebrew-core. See RELEASE.md
for generating its stable URL and real source-archive checksum.

## Troubleshooting and limits

- Unsupported wheel: check Python version, CPU and OS floor; Python 3.10 is not
  in the initial matrix because the pinned ecCodes binary dependencies omit it.
- Windows: C++ uses fork/execvp, pipe, popen, unistd.h, sys/wait.h and resource
  APIs. No native Windows wheel is claimed. Use a supported Linux in WSL2.
- Missing encoder/pixel format: inspect the installed FFmpeg build. Preset names
  are not proof that your FFmpeg supports a codec/bit depth.
- Tiny default grids: the existing four-tile AV1 profile can reject very narrow
  images (e.g. width64); packaging does not alter this numerical/profile choice.
- HTTP access requires server range support; see existing HTTP documentation.
  Zarr export is a native CLI feature and writes arrays from selected fields;
  optional zarr/Dask readers need their own Python packages.
- No scientific error guarantee is implied by a profile or codec losslessness.
