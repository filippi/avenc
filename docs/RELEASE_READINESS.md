# AVENC release readiness — 20 October 2026

## Candidate verification

The repository is public on main at https://github.com/filippi/avenc. The author
configured the pending PyPI publisher and authorized the first release.

Both source jobs passed: https://github.com/filippi/avenc/actions/runs/39472390491
All 16 wheels and four standalone installers passed:
https://github.com/filippi/avenc/actions/runs/39472387693

The wheel matrix covers CPython3.11–3.14 on Linux x86_64/aarch64 and macOS
Intel/Apple Silicon. The final release workflow rebuilds and retests the exact
release tag before publishing. The candidate checksums/native inventories are
retained in packaging/audits/verified_1.0.0_candidates.json.

Linux now uses pinned standard ecCodes/libaec sources, with PNG, OpenJPEG, AEC
and embedded definitions. It excludes the optional eckit-geo networking backend.
Actual candidate wheels on both Linux architectures contain only ecCodes,
libaec, PNG/OpenJPEG and AVENC's subset library; their upstream notices are
included. Unexpected non-system Linux dependencies fail the build. The existing
Linux dependency-approval and release-enable gates remain in place.

The local checks below describe initial preparation; earlier missing-service
or unexecuted-platform notes are historical and superseded by the hosted runs.

## Implemented files

- Root pyproject.toml, VERSION and CMake integration: one avenc distribution,
  native executables, subset library and pybind11/xarray packages. The independent
  python/avenc/pyproject.toml was retired to avoid overlapping distributions.
- src/avenc_version.h and CLI entry points: common --version; avenc2grib --help
  now exits successfully. Numerical/bitmap/codec algorithms are unchanged.
- python/avenc/avenc/cli.py: seven PATH wrappers; actionable FFmpeg diagnostics.
  Backend discovery prefers the installed bundled decoder.
- packaging/: build-only ecCodes discovery, dependency bundling/repair, installed
  wheel smoke tests, CI-only FFmpeg bootstrap, native archive/installer/uninstaller,
  artifact-size/checksum checks and stable Homebrew formula generation.
- .github/workflows/: source checks, four-target cibuildwheel matrix, gated tag/
  manual release, source/wheel/native artifacts, OIDC publishing and GitHub Release.
- Formula/avenc.rb: HEAD formula, native build and functional test. Stable formula
  generation uses a real downloaded tag archive and computed SHA-256.
- README.md, docs/INSTALL.md, docs/RELEASE.md, THIRD_PARTY.md, retained licence
  notices and synchronized agent/Python guides. Historical cleanup results are
  archived in docs/PRE_PACKAGING_CHECKS.md.

## Platform targets and local evidence

Initial automated targets are CPython **3.11, 3.12, 3.13, 3.14**, standard GIL
builds, on Linux x86_64/aarch64 (manylinux_2_28) and macOS x86_64/arm64 (>=15).
Python 3.10 has no wheel in the pinned current native dependency release.
Windows uses incompatible POSIX process/resource APIs; WSL2 Linux is the current
route. No Windows or musllinux wheel is claimed.

Local verification used macOS/arm64, AppleClang17, FFmpeg8.0.1:

| Check | Observed result |
| --- | --- |
| Root PEP 517 sdist -> unified CPython3.11 wheel | Passed in isolated build; all seven native tools included |
| Root unified CPython3.14 wheel | Built; installed in separate clean venv |
| Minimal wheel install without eccodeslib/eckitlib runtime packages | Import, --version, GRIB conversion both ways and xarray subset passed |
| Installed-wheel synthetic smoke | Passed: two wind variables, two forecast steps, field identities/counts, shared bitmap, physical reconstruction tolerance, backend discovery and selective read |
| Lossless sample check | Decoded 12-bit video samples exactly match independently converted gray16->gray12 samples at valid cells |
| Existing native comparison tests against installed tools | 11 passed |
| Installed backend/HTTP/xarray/Dask suite | 17 passed, 2 skipped, 7 subtests passed |
| Native CMake / CTest | All targets compiled; 1 metric test passed |
| Browser metadata / decoder tests | Both Node scripts passed |
| CPython3.14 Python-array example | Passed, reported RMSE0.0114252 for the documented AVC8 synthetic example |
| Native archive install, GRIB round trip and uninstall | Passed in a temporary user prefix |
| PyPI metadata validation | twine check passed for sdist and CP311 wheel |
| GitHub workflow validation | actionlint1.7.12 passed; hosted jobs have not run |
| Shell / formula syntax | bash -n and ruby -c passed |

The skipped tests are public-network HTTP (opt-in) and the optional separate truth
GRIB fixture. Native synthetic tests independently compare reference GRIB values;
skips are not accuracy evidence. The native archive test used no Python package
for local conversion. Python remains optional for the CLI's HTTP helper.
No large scientific experiments were rerun. Generated artifacts and environments
remain outside the public source tree.

The macOS wheel is approximately **11 MB**, including embedded ecCodes definitions
and native dependencies; it is below PyPI's default file-size limit. Each release
runs a conservative 100,000,000-byte wheel check. Cross-platform sizes remain to
be measured by CI.

## Numerical scope retained

The video-lossless path includes finite scientific quantization and FFmpeg pixel
format conversion. The synthetic check observed that the existing gray16->gray12
conversion can lower a sample by one code point. The round-trip tolerance includes
that conversion and GRIB repacking (1.5 quantization steps plus roundoff); the
separate video check uses converted samples. This is documented, not silently
fixed during packaging. The default AV1 tiling also still rejects some very narrow
grids; the tiny test uses width512. There is no new scientific error guarantee.

## Open release blockers and manual work

1. **Linux native dependency closure:** static ELF inspection of official Linux
   eccodeslib2.50.0.33 / eckitlib2.5.0.33 wheels found additional curl/TLS/Kerberos/
   LDAP/SSH/IDN libraries. Their redistribution notices/source obligations and
   security status are not yet cleared. The current curated licence set does not
   cover this whole Linux closure. See packaging/audits/linux_ecmwf_dependencies.json.
   Resolve the obligations or replace the dependency build with a minimal one;
   only then set AVENC_LINUX_DEPENDENCIES_APPROVED=true. The publication job
   requires that gate as well as AVENC_RELEASE_ENABLED=true and manual approval.
2. **Hosted verification:** Linux x86_64/aarch64 and macOS Intel builds have not
   run locally. The cibuildwheel jobs repair and install/test each wheel and native
   archive; they must pass before treating those platforms as verified support.
3. **FFmpeg is external:** install prebuilt FFmpeg/ffprobe. No LGPL/GPL media
   executables enter AVENC wheels. Available codecs, hardware support and AVC/HEVC
   patent terms depend on that separate build; no patent clearance is asserted.
4. **Service configuration:** first authorized source commit/push; protected
   branches/environments; pending TestPyPI/PyPI trusted publishers; author-approved
   tag/release; separate Homebrew tap and functional brew test. These account
   operations were not performed. The exact release commands are in RELEASE.md.
5. **Installation modes not locally exercised:** pipx and --user wrappers follow
   standard console-script semantics but were not executed; PEP668 managed Python
   should use venv/pipx. Homebrew source install and Linux tarball remain CI/manual
   checks; no .deb package or homebrew-core inclusion is claimed.

## Primary references used for packaging decisions

- scikit-build-core: https://scikit-build-core.readthedocs.io/
- cibuildwheel repair: https://cibuildwheel.pypa.io/en/stable/options/#repair-wheel-command
- PyPI trusted publishers: https://docs.pypi.org/trusted-publishers/using-a-publisher/
- PyPI size limits: https://docs.pypi.org/project-management/storage-limits/
- Runner labels: https://docs.github.com/en/actions/reference/runners/github-hosted-runners
- FFmpeg licensing: https://ffmpeg.org/legal.html

See INSTALL.md for scientist-facing commands and RELEASE.md for the maintainer
checklist, first tag, TestPyPI trial, production gates and Homebrew update.
